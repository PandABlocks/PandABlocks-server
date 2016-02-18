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
#include <asm/atomic.h>

#include "error.h"
#include "panda.h"

#include "panda_device.h"


/* Information associated with an open file. */
struct block_open {
    struct semaphore lock;
    unsigned int order;         // log2 number of pages in block
    size_t block_size;          // Bytes in block
    unsigned long block;        // Kernel address of allocated block
    dma_addr_t dma;             // DMA address of block
    struct vm_area_struct *vma; // Virtual memory area associated with block
};


static int panda_block_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct block_open *open = file->private_data;

    int rc = down_interruptible(&open->lock);
    TEST_RC(rc, no_lock, "Interrupted during lock");

    /* Check that we've allocated our pages and haven't already been mapped. */
    TEST_(open->block, rc = -ENXIO, bad_request, "No block allocated");
    TEST_(!open->vma, rc = -EBUSY, bad_request, "Block already mapped");

    /* Check the mapped area is in range. */
    size_t size = vma->vm_end - vma->vm_start;
    unsigned long end = (vma->vm_pgoff << PAGE_SHIFT) + size;
    TEST_(end <= open->block_size,
        rc = -EINVAL, bad_request, "Requested area out of range");

    /* Let remap_pfn_range do all the work. */
    open->vma = vma;
    rc = remap_pfn_range(
        vma, vma->vm_start, open->block + vma->vm_pgoff, size,
        vma->vm_page_prot);

bad_request:
    up(&open->lock);

no_lock:
    return rc;
}


static long create_block(struct file *file, unsigned long order)
{
    struct block_open *open = file->private_data;

    /* Lock while we allocate.  Need to do this first so that the block count
     * test is valid. */
    long result = down_interruptible(&open->lock);
    TEST_RC(result, no_lock, "Interrupted during lock");

    /* Record the block size. */
    open->order = order;
    open->block_size = 1U << (order + PAGE_SHIFT);

    /* Allocate the requested block and map it for DMA. */
    open->block = __get_free_pages(GFP_KERNEL, order);
    TEST_(open->block, result = -ENOMEM, no_block, "Unable to allocate block");
    open->dma = dma_map_single(
        NULL, (void *) open->block, open->block_size, DMA_TO_DEVICE);
    TEST_(!dma_mapping_error(NULL, open->dma),
        result = -EIO, no_dma, "Unable to map DMA area");

    /* Caller wants the physical page address. */
    up(&open->lock);
    return open->dma;

no_dma:
    free_pages((unsigned long) open->block, order);
    open->block = 0;
no_block:
    up(&open->lock);
no_lock:
    return result;
}


static long flush_block(struct file *file)
{
    struct block_open *open = file->private_data;

    /* Lock while we allocate.  Need to do this first so that the block count
     * test is valid. */
    long rc = down_interruptible(&open->lock);
    TEST_RC(rc, no_lock, "Interrupted during lock");

    /* Check we have a mapped area to flush. */
    TEST_(open->vma, rc = -ENXIO, no_vma, "No mapping for block");

    dma_sync_single_for_device(
        NULL, open->dma, open->block_size, DMA_FROM_DEVICE);

no_vma:
    up(&open->lock);
no_lock:
    return rc;
}


static long panda_block_ioctl(
    struct file *file, unsigned int cmd, unsigned long arg)
{
    printk(KERN_INFO "panda.block ioctl %u %08lx\n", cmd, arg);

    switch (cmd)
    {
        case PANDA_BLOCK_CREATE:
            return create_block(file, arg);

        case PANDA_BLOCK_FLUSH:
            return flush_block(file);

        default:
            return -EINVAL;
    }
}


static int panda_block_open(struct inode *inode, struct file *file)
{
    int rc = 0;
    struct block_open *open = kmalloc(sizeof(*open), GFP_KERNEL);
    TEST_PTR(open, rc, no_open, "Unable to allocate open structure");
    file->private_data = open;

    *open = (struct block_open) {
    };
    sema_init(&open->lock, 1);

no_open:
    return rc;
}


static int panda_block_release(struct inode *inode, struct file *file)
{
    struct block_open *open = file->private_data;
    if (open->block)
    {
        dma_unmap_single(NULL, open->dma, open->block_size, DMA_TO_DEVICE);
        free_pages(open->block, open->order);
    }
    kfree(open);
    return 0;
}


static struct file_operations panda_block_fops = {
    .owner = THIS_MODULE,
    .open = panda_block_open,
    .release = panda_block_release,
    .mmap = panda_block_mmap,
    .unlocked_ioctl = panda_block_ioctl,
};


int panda_block_init(struct file_operations **fops)
{
    *fops = &panda_block_fops;
    return 0;
}
