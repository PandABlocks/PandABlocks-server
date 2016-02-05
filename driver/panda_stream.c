/* Stream device for retrieving captured data. */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <asm/atomic.h>

#include "error.h"
#include "panda.h"

#include "panda_device.h"


static atomic_t device_open = ATOMIC_INIT(0);



static int panda_stream_open(struct inode *inode, struct file *file)
{
    /* Only permit one user. */
    if (!atomic_add_unless(&device_open, 1, 1))
        return -EBUSY;

    return 0;
}


static int panda_stream_release(struct inode *inode, struct file *file)
{
    atomic_dec(&device_open);
    return 0;
}


static ssize_t panda_stream_read(
    struct file *file, char __user *buf, size_t count, loff_t *f_pos)
{
    return 0;
}


static struct file_operations panda_stream_fops = {
    .owner = THIS_MODULE,
    .open       = panda_stream_open,
    .release    = panda_stream_release,
    .read       = panda_stream_read,
};


int panda_stream_init(struct file_operations **fops, const char **name)
{
    *fops = &panda_stream_fops;
    *name = "stream";
    return 0;
}
