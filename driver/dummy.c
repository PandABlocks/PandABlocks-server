/* Dummy initialisation for remaining file nodes. */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>

#include "error.h"
#include "panda.h"


static int dummy_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "Rejecting open request\n");
    return -EIO;
}

static struct file_operations dummy_fops = {
    owner: THIS_MODULE,
    open: dummy_open,
};

int panda_dummy1_init(struct file_operations **fops, const char **name)
{
    *fops = &dummy_fops;
    *name = "dummy1";
    return 0;
}

int panda_dummy2_init(struct file_operations **fops, const char **name)
{
    *fops = &dummy_fops;
    *name = "dummy2";
    return 0;
}

