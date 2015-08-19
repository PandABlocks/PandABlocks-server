/* Entry point for PandA interface kernel module. */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>

#include "error.h"
#include "panda.h"


MODULE_AUTHOR("Michael Abbott, Diamond Light Source Ltd");
MODULE_DESCRIPTION("PandA device driver");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("panda");
MODULE_VERSION("0");


#define PANDA_MINORS    3           // We have 3 sub devices


/* The fops and name fields of this structure are filled in by the appropriate
 * subcomponents of this module. */
struct panda_info {
    struct file_operations *fops;
    const char *name;
    int (*init)(struct file_operations **fops, const char **name);
};
static struct panda_info panda_info[PANDA_MINORS] = {
    { .init = panda_map_init },
    { .init = panda_dummy1_init },
    { .init = panda_dummy2_init },
};


/* Kernel file and device interface state. */
static dev_t panda_dev;             // Major device for PandA support
static struct cdev panda_cdev;      // Top level file functions
static struct class *panda_class;   // Sysfs class support



static int panda_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "Opening panda device: %d:%d\n",
        imajor(inode), iminor(inode));

    file->f_op = panda_info[iminor(inode)].fops;
    if (file->f_op->open)
        return file->f_op->open(inode, file);
    else
        return 0;
}


static struct file_operations base_fops = {
    .owner = THIS_MODULE,
    .open = panda_open,
};


static int __init panda_init(void)
{
    printk(KERN_INFO "Loading PandA driver\n");
    int rc = 0;

    /* First initialise the three PandA subcomponents. */
    for (int i = 0; rc == 0  &&  i < PANDA_MINORS; i ++)
    {
        struct panda_info *info = &panda_info[i];
        rc = info->init(&info->fops, &info->name);
    }
    TEST_RC(rc, no_panda, "Unable to initialise PandA");

    /* Allocate device number for this function. */
    rc = alloc_chrdev_region(&panda_dev, 0, PANDA_MINORS, "panda");
    TEST_RC(rc, no_chrdev, "unable to allocate dev region");

    /* Create character device support. */
    cdev_init(&panda_cdev, &base_fops);
    panda_cdev.owner = THIS_MODULE;
    rc = cdev_add(&panda_cdev, panda_dev, PANDA_MINORS);
    TEST_RC(rc, no_cdev, "unable to add device");

    /* Publish devices in sysfs. */
    panda_class = class_create(THIS_MODULE, "panda");
    TEST_PTR(panda_class, rc, no_class, "unable to create class");
    for (int i = 0; i < PANDA_MINORS; i ++)
        device_create(
            panda_class, NULL, panda_dev + i, NULL,
            "panda.%s", panda_info[i].name);

    return 0;


no_panda:
no_class:
    cdev_del(&panda_cdev);
no_cdev:
    unregister_chrdev_region(panda_dev, PANDA_MINORS);
no_chrdev:

    return rc;
}


static void __exit panda_exit(void)
{
    printk(KERN_INFO "Unloading PandA driver\n");
    for (int i = 0; i < PANDA_MINORS; i ++)
        device_destroy(panda_class, panda_dev + i);
    class_destroy(panda_class);

    cdev_del(&panda_cdev);
    unregister_chrdev_region(panda_dev, PANDA_MINORS);
}


module_init(panda_init);
module_exit(panda_exit);
