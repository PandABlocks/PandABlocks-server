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



/* The PandA IO registers are in this area of memory. */
#define PANDA_REGISTER_BASE     0x43C00000
#define PANDA_REGISTER_LENGTH   0x00040000

/* Page containing first PandA register. */
#define PANDA_BASE_PAGE         (PANDA_REGISTER_BASE >> PAGE_SHIFT)


struct map_open {
    unsigned long base_page;
    unsigned long area_length;
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


static long panda_map_ioctl(
    struct file *file, unsigned int cmd, unsigned long arg)
{
    printk(KERN_INFO "panda.map ioctl %u %08lx\n", cmd, arg);

    switch (cmd)
    {
        case PANDA_MAP_SIZE:
        {
            uint32_t size = PANDA_REGISTER_LENGTH;
            void *target = (void __user *) arg;
            return COPY_TO_USER(target, size);
        }

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

no_open:
    return rc;
}


static int panda_map_release(struct inode *inode, struct file *file)
{
    struct map_open *open = file->private_data;
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


int panda_map_init(struct file_operations **fops)
{
    *fops = &panda_map_fops;
    return 0;
}
