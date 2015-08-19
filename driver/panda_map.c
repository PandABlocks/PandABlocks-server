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

#include "error.h"
#include "panda.h"

#include "panda_device.h"



/* The PandA IO registers are in this area of memory. */
#define PANDA_REGISTER_BASE     0x43C00000
#define PANDA_REGISTER_LENGTH   0x00040000

/* Page containing first PandA register. */
#define PANDA_BASE_PAGE         (PANDA_REGISTER_BASE >> PAGE_SHIFT)


static int panda_map_mmap(struct file *file, struct vm_area_struct *vma)
{
    printk(KERN_INFO "Mapping panda.map\n");
    printk(KERN_INFO " %lx..%lx, %lx, %x\n",
        vma->vm_start, vma->vm_end, vma->vm_pgoff, vma->vm_page_prot);

    size_t size = vma->vm_end - vma->vm_start;
    unsigned long end = (vma->vm_pgoff << PAGE_SHIFT) + size;
    if (end > PANDA_REGISTER_LENGTH)
    {
        printk(KERN_WARNING "PandA map area out of range\n");
        return -EINVAL;
    }

    /* Good advice and examples on using this function here:
     *  http://www.makelinux.net/ldd3/chp-15-sect-2 */
    return io_remap_pfn_range(
        vma, vma->vm_start, PANDA_BASE_PAGE + vma->vm_pgoff, size,
        vma->vm_page_prot);
}


#define COPY_TO_USER(result, value) \
    (copy_to_user(result, &(value), sizeof(value)) == 0 ? 0 : -EFAULT)

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

        default:
            return -EINVAL;
    }
}


static struct file_operations panda_map_fops = {
    .owner = THIS_MODULE,
    .mmap = panda_map_mmap,
    .unlocked_ioctl = panda_map_ioctl,
};


int panda_map_init(struct file_operations **fops, const char **name)
{
    *fops = &panda_map_fops;
    *name = "map";
    return 0;
}
