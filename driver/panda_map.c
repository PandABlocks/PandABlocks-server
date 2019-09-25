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
#include <linux/vmalloc.h>
#include <asm/atomic.h>

#include "error.h"
#include "panda.h"

#include "panda_device.h"


static atomic_t device_open = ATOMIC_INIT(0);


static int panda_map_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct panda_pcap *pcap = file->private_data;

    size_t size = vma->vm_end - vma->vm_start;
    unsigned long end = (vma->vm_pgoff << PAGE_SHIFT) + size;
    if (end > pcap->length)
    {
        printk(KERN_WARNING "PandA map area out of range\n");
        return -EINVAL;
    }

    /* Good advice and examples on using this function here:
     *  http://www.makelinux.net/ldd3/chp-15-sect-2.shtml
     * Also see drivers/char/mem.c in kernel sources for guidelines. */
    return io_remap_pfn_range(
        vma, vma->vm_start, pcap->base_page + vma->vm_pgoff, size,
        pgprot_noncached(vma->vm_page_prot));
}


static long panda_map_ioctl(
    struct file *file, unsigned int cmd, unsigned long arg)
{
    struct panda_pcap *pcap = file->private_data;
    switch (cmd)
    {
        case PANDA_MAP_SIZE:
            return pcap->length;
        default:
            return -EINVAL;
    }
}


static int panda_map_open(struct inode *inode, struct file *file)
{
    if (!atomic_add_unless(&device_open, 1, 1))
        return -EBUSY;

    /* A slightly tricky dance here: the cdev we're passed is in fact part of
     * the panda_pcap structure containing the platform information we want! */
    struct cdev *cdev = inode->i_cdev;
    file->private_data = container_of(cdev, struct panda_pcap, cdev);
    return 0;
}


static int panda_map_release(struct inode *inode, struct file *file)
{
    atomic_dec(&device_open);
    return 0;
}


struct file_operations panda_map_fops = {
    .owner = THIS_MODULE,
    .open = panda_map_open,
    .release = panda_map_release,
    .mmap = panda_map_mmap,
    .unlocked_ioctl = panda_map_ioctl,
};
