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
#include <asm/atomic.h>

#include "error.h"
#include "panda.h"

#include "panda_device.h"


/* Information associated with an open file. */
struct block_open {
    struct semaphore lock;
    unsigned int order;
    size_t block_size;
    struct page *pages;
    struct vm_area_struct *vma;
};


static int panda_block_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct block_open *open = file->private_data;

    int rc = down_interruptible(&open->lock);
    TEST_RC(rc, no_lock, "Interrupted during lock");

    /* Check that we've allocated our pages and haven't already been mapped. */
    TEST_(open->pages, rc = -ENXIO, bad_request, "No pages allocated");
    TEST_(!open->vma, rc = -EBUSY, bad_request, "Block already mapped");

    /* Check the mapped area is in range. */
    size_t size = vma->vm_end - vma->vm_start;
    unsigned long end = (vma->vm_pgoff << PAGE_SHIFT) + size;
    TEST_(end <= open->block_size,
        rc = -EINVAL, bad_request, "Requested area out of range");

    /* Let remap_pfn_range do all the work. */
    open->vma = vma;
    rc = remap_pfn_range(
        vma, vma->vm_start, page_to_phys(open->pages) + vma->vm_pgoff, size,
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

    /* Allocate the requested number of pages. */
    open->pages = alloc_pages(GFP_KERNEL, order);
    TEST_(open->pages, result = -ENOMEM, no_pages, "Unable to allocate pages");

    /* Record everything relevant. */
    open->order = order;
    open->block_size = 1U << (order + PAGE_SHIFT);

    /* Caller wants the physical page address. */
    result = page_to_phys(open->pages);

no_pages:
    up(&open->lock);
no_lock:
printk(KERN_INFO "returning block: %u %zu %08lx\n",
open->order, open->block_size, result);
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

    /* Flush the entire range. */
    flush_cache_range(open->vma, open->vma->vm_start, open->vma->vm_end);

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
    if (open->pages)
        __free_pages(open->pages, open->order);
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
