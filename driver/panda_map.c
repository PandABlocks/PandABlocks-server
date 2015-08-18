/* Device for mapping PandA register space into user space memory.
 *
 * A register space of 65536 double word registers (256KB) is allocated for
 * PandA configuration and core functionality.  This is mapped via the device
 * node /dev/panda.map defined here. */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>

#include "error.h"
#include "panda.h"



#define PANDA_REGISTER_BASE     0x43C00000
#define PANDA_REGISTER_LENGTH   0x00040000




static int panda_map_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "Opening panda.map\n");
    return 0;
}

static int panda_map_release(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "Closing panda.map\n");
    return 0;
}

static int panda_map_mmap(struct file *file, struct vm_area_struct *vma)
{
    printk(KERN_INFO "Mapping panda.map\n");
    return -EIO;
}


static struct file_operations panda_map_fops = {
    owner: THIS_MODULE,
    open: panda_map_open,
    release: panda_map_release,
    mmap: panda_map_mmap,
};


int panda_map_init(struct file_operations **fops, const char **name)
{
    *fops = &panda_map_fops;
    *name = "map";
    return 0;
}
