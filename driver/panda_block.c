/* Support for memory mapped contiguous blocks of memory. */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/semaphore.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <asm/atomic.h>

#include "error.h"
#include "panda.h"

#include "panda_device.h"


/* Information associated with an open file. */
struct block_open {
    struct semaphore lock;

    size_t block_size;          // Bytes in block
    void *block_addr;           // Kernel address of allocated block
    dma_addr_t dma;             // DMA address of block
    struct vm_area_struct *vma; // Virtual memory area associated with block

    struct panda_pcap *pcap;    // Device capabilites
    struct panda_block block;   // Block specification
};


static int panda_block_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct block_open *open = file->private_data;

    down(&open->lock);

    /* Check that we've allocated our pages and haven't already been mapped. */
    int rc = 0;
    TEST_OK(open->block_addr, rc = -ENXIO, bad_request, "No block allocated");
    TEST_OK(!open->vma, rc = -EBUSY, bad_request, "Block already mapped");

    /* Check the mapped area is in range. */
    size_t size = vma->vm_end - vma->vm_start;
    unsigned long end = (vma->vm_pgoff << PAGE_SHIFT) + size;
    TEST_OK(end <= open->block_size,
        rc = -EINVAL, bad_request, "Requested area out of range");

    /* Let remap_pfn_range do all the work. */
    unsigned long base_page = virt_to_phys(open->block_addr) >> PAGE_SHIFT;
    rc = remap_pfn_range(
        vma, vma->vm_start, base_page + vma->vm_pgoff, size, vma->vm_page_prot);
    open->vma = vma;

bad_request:
    up(&open->lock);
    return rc;
}


/* Prepares the backing memory as a fixed block. */
static int create_block(
    struct block_open *open, const struct panda_block __user *block)
{
    struct panda_pcap *pcap = open->pcap;
    struct device *dev = &pcap->pdev->dev;

    /* Lock while we allocate. */
    down(&open->lock);

    /* Check the block isn't already allocated. */
    int rc = 0;
    TEST_OK(!open->block_addr,
        rc = -EBUSY, bad_request, "Block already allocated");

    /* Try to retrieve the ioctl arguments and validate them. */
    TEST_OK(!copy_from_user(&open->block, block, sizeof(struct panda_block)),
        rc = -EFAULT, bad_request, "Error copying block");
    TEST_OK(
        open->block.block_base < pcap->length - 4  &&
        open->block.block_length < pcap->length - 4,
        rc = -EINVAL, bad_request, "Invalid register argument for block");

    /* Ok, all in order.  Allocate the requested block and map it for DMA.
     * Flag GFP_DMA32 is set to avoid using bounce buffer. */
    open->block_addr = (void *) __get_free_pages(GFP_KERNEL | GFP_DMA32, open->block.order);
    TEST_OK(open->block_addr, rc = -ENOMEM, bad_request,
        "Unable to allocate block");
    open->dma = dma_map_single(
        dev, open->block_addr, open->block_size, DMA_TO_DEVICE);
    TEST_OK(!dma_mapping_error(dev, open->dma),
        rc = -EIO, no_dma, "Unable to map DMA area");

    /* Inform the hardware via the register we were given. */
    writel(open->dma, pcap->reg_base + open->block.block_base);

    /* Start the block in device mode. */
    open->block_size = 1U << (open->block.order + PAGE_SHIFT);
    dma_sync_single_for_device(
        dev, open->dma, open->block_size, DMA_TO_DEVICE);

    up(&open->lock);
    return (int) open->block_size;

no_dma:
    free_pages((unsigned long) open->block_addr, open->block.order);
    open->block_addr = NULL;
bad_request:
    up(&open->lock);
    return rc;
}


static long panda_block_ioctl(
    struct file *file, unsigned int cmd, unsigned long arg)
{
    struct block_open *open = file->private_data;
    switch (cmd)
    {
        case PANDA_BLOCK_CREATE:
            return create_block(open, (const void __user *) arg);
        default:
            return -EINVAL;
    }
}


static ssize_t panda_block_write(
    struct file *file, const char __user *data, size_t length, loff_t *offset)
{
    struct block_open *open = file->private_data;
    struct panda_pcap *pcap = open->pcap;
    struct device *dev = &pcap->pdev->dev;

    /* Lock while we write. */
    down(&open->lock);

    /* Check the block is allocated. */
    int rc = 0;
    TEST_OK(open->block_addr,
        rc = -EBUSY, bad_request, "No block allocated");
    /* Check the requested length is valid. */
    TEST_OK(*offset + length <= open->block_size,
        rc = -EFBIG, bad_request, "Write segment too long");

    /* Good.  Tell the hardware, switch into CPU mode, copy the data, switch
     * back into device mode, tell the hardware. */
    writel(0, pcap->reg_base + open->block.block_length);
    dma_sync_single_for_cpu(
        dev, open->dma, open->block_size, DMA_TO_DEVICE);
    TEST_OK(!copy_from_user(open->block_addr + *offset, data, length),
        rc = -EFAULT, bad_copy, "Fault copying data from user");
    dma_sync_single_for_device(
        dev, open->dma, open->block_size, DMA_TO_DEVICE);
    writel(*offset + length, pcap->reg_base + open->block.block_length);

    up(&open->lock);
    return (ssize_t) length;

bad_copy:
    dma_sync_single_for_device(
        dev, open->dma, open->block_size, DMA_TO_DEVICE);
bad_request:
    up(&open->lock);
    return rc;
}


static loff_t panda_block_seek(struct file *file, loff_t offset, int whence)
{
    file->f_pos = offset;
    return offset;
}


static int panda_block_open(struct inode *inode, struct file *file)
{
    int rc = 0;
    struct block_open *open = kmalloc(sizeof(*open), GFP_KERNEL);
    TEST_PTR(open, rc, no_open, "Unable to allocate open structure");

    *open = (struct block_open) {
        .pcap = container_of(inode->i_cdev, struct panda_pcap, cdev),
    };
    sema_init(&open->lock, 1);
    file->private_data = open;

no_open:
    return rc;
}


static int panda_block_release(struct inode *inode, struct file *file)
{
    struct block_open *open = file->private_data;
    struct panda_pcap *pcap = open->pcap;
    struct device *dev = &pcap->pdev->dev;

    if (open->block_addr)
    {
        /* Disable hardware access to memory. */
        writel(0, pcap->reg_base + open->block.block_length);
        dma_unmap_single(dev, open->dma, open->block_size, DMA_TO_DEVICE);
        free_pages((unsigned long) open->block_addr, open->block.order);
    }
    kfree(open);
    return 0;
}


struct file_operations panda_block_fops = {
    .owner = THIS_MODULE,
    .open = panda_block_open,
    .release = panda_block_release,
    .write = panda_block_write,
    .llseek = panda_block_seek,
    .mmap = panda_block_mmap,
    .unlocked_ioctl = panda_block_ioctl,
};
