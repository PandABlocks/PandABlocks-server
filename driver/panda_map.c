/* Device for mapping PandA register space into user space memory.
 *
 * A register space of 65536 double word registers (256KB) is allocated for
 * PandA configuration and core functionality.  This is mapped via the device
 * node /dev/panda.map defined here. */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/semaphore.h>
#include <asm/atomic.h>

#include "error.h"
#include "panda.h"

#include "panda_device.h"



/* The PandA IO registers are in this area of memory. */
#define PANDA_REGISTER_BASE     0x43C00000
#define PANDA_REGISTER_LENGTH   0x00040000

/* Page containing first PandA register. */
#define PANDA_BASE_PAGE         (PANDA_REGISTER_BASE >> PAGE_SHIFT)


/* Allocating more blocks than this will fail. */
#define MAX_BLOCK_COUNT     16


struct map_open {
    unsigned long base_page;
    unsigned long area_length;

    struct semaphore lock;
    struct block_info {
        unsigned int order;
        struct page *pages;
    } blocks[MAX_BLOCK_COUNT];
};


static atomic_t device_open = ATOMIC_INIT(0);


static int panda_map_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct map_open *open = file->private_data;

    printk(KERN_INFO "Mapping panda.map\n");
    printk(KERN_INFO " %lx..%lx, %lx, %x\n",
        vma->vm_start, vma->vm_end, vma->vm_pgoff, vma->vm_page_prot);

    size_t size = vma->vm_end - vma->vm_start;
    unsigned long end = (vma->vm_pgoff << PAGE_SHIFT) + size;
    if (end > open->area_length)
    {
        printk(KERN_WARNING "PandA map area out of range\n");
        return -EINVAL;
    }

    /* Good advice and examples on using this function here:
     *  http://www.makelinux.net/ldd3/chp-15-sect-2
     * Also see drivers/char/mem.c in kernel sources for guidelines. */
    return io_remap_pfn_range(
        vma, vma->vm_start, open->base_page + vma->vm_pgoff, size,
        pgprot_noncached(vma->vm_page_prot));
}


#define COPY_TO_USER(result, value) \
    (copy_to_user(result, &(value), sizeof(value)) == 0 ? 0 : -EFAULT)


static unsigned int find_free_block_id(struct map_open *open)
{
    for (unsigned int i = 0; i < MAX_BLOCK_COUNT; i ++)
        if (open->blocks[i].pages == NULL)
            return i;
    return MAX_BLOCK_COUNT;
}


static long create_block(
    struct file *file, struct panda_block __user *user_block)
{
    struct map_open *open = file->private_data;

    /* Lock while we allocate.  Need to do this first so that the block count
     * test is valid. */
    int rc = down_interruptible(&open->lock);
    TEST_RC(rc, no_lock, "Interrupted during lock");

    /* Ensure we haven't already allocated our quota of blocks. */
    unsigned int block_id = find_free_block_id(open);
    TEST_(block_id >= MAX_BLOCK_COUNT,
        rc = -ENOSPC, no_free, "Too many blocks");

    /* Read the requested order. */
    struct panda_block block;
    TEST_(copy_from_user(&block, user_block, sizeof(block)),
        rc = -EFAULT, no_copy_from, "Unable to read ioctl block");

    /* Allocate the requested number of pages. */
    struct page *pages = alloc_pages(GFP_KERNEL, block.order);
    TEST_(!pages, rc = -ENOMEM, no_pages, "Unable to allocate pages");

    /* The caller wants to know how many bytes have been allocated, the
     * "logical" or virtual address of the block, the corresponding physical
     * address for the hardware, and the block id for flushing and release. */
    block.block_size = 1U << (block.order + PAGE_SHIFT);
    block.block = page_address(pages);
    block.phy_address = page_to_phys(pages);
    block.block_id = block_id;

    /* Record this block so we can process it properly later on. */
    open->blocks[block_id] = (struct block_info) {
        .order = block.order,
        .pages = pages,
    };

    up(&open->lock);
    return COPY_TO_USER(user_block, block);

no_pages:
no_copy_from:
no_free:
    up(&open->lock);
no_lock:
    return rc;
}


static long flush_block(struct file *file, unsigned int block_id)
{
    return -EINVAL;
}


static long release_block(struct file *file, unsigned int block_id)
{
    struct map_open *open = file->private_data;

    /* Lock while we allocate.  Need to do this first so that the block id
     * test is valid. */
    int rc = down_interruptible(&open->lock);
    TEST_RC(rc, no_lock, "Interrupted during lock");

    TEST_(block_id >= MAX_BLOCK_COUNT,
        rc = -EINVAL, bad_id, "Invalid block id");
    struct block_info *block = &open->blocks[block_id];
    TEST_(block->pages == NULL,
        rc = -EINVAL, bad_id, "Invalid block id");

    __free_pages(block->pages, block->order);
    block->pages = NULL;

bad_id:
    up(&open->lock);
no_lock:
    return rc;
}


static long panda_map_ioctl(
    struct file *file, unsigned int cmd, unsigned long arg)
{
    printk(KERN_INFO "panda.map ioctl %u %08lx\n", cmd, arg);

    uint32_t size = PANDA_REGISTER_LENGTH;
    void *target = (void __user *) arg;
    switch (cmd)
    {
        case PANDA_MAP_SIZE:
            return COPY_TO_USER(target, size);

        case PANDA_BLOCK_CREATE:
            return create_block(file, target);

        case PANDA_BLOCK_RELEASE:
            return release_block(file, arg);

        case PANDA_BLOCK_FLUSH:
            return flush_block(file, arg);

        default:
            return -EINVAL;
    }
}


static int panda_map_open(struct inode *inode, struct file *file)
{
    if (!atomic_add_unless(&device_open, 1, 1))
        return -EBUSY;

    int rc = 0;
    struct map_open *open = kmalloc(sizeof(*open), GFP_KERNEL);
    TEST_PTR(open, rc, no_open, "Unable to allocate open structure");
    file->private_data = open;

    *open = (struct map_open) {
        .base_page = PANDA_BASE_PAGE,
        .area_length = PANDA_REGISTER_LENGTH,
    };
    sema_init(&open->lock, 1);

no_open:
    return rc;
}


static int panda_map_release(struct inode *inode, struct file *file)
{
    struct map_open *open = file->private_data;

    for (unsigned int i = 0; i < MAX_BLOCK_COUNT; i ++)
        if (open->blocks[i].pages)
            __free_pages(open->blocks[i].pages, open->blocks[i].order);

    kfree(open);

    atomic_dec(&device_open);
    return 0;
}


static struct file_operations panda_map_fops = {
    .owner = THIS_MODULE,
    .open = panda_map_open,
    .release = panda_map_release,
    .mmap = panda_map_mmap,
    .unlocked_ioctl = panda_map_ioctl,
};


int panda_map_init(struct file_operations **fops, const char **name)
{
    *fops = &panda_map_fops;
    *name = "map";
    return 0;
}
