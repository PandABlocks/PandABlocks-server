/* Entry point for PandA interface kernel module. */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>

#include "error.h"


MODULE_AUTHOR("Michael Abbott, Diamond Light Source Ltd");
MODULE_DESCRIPTION("PandA device driver");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("panda");
MODULE_VERSION("0");


#define PANDA_MINORS    3           // We have 3 sub devices

static dev_t panda_dev;             // Major device for PandA support
static struct cdev panda_cdev;      // Top level file functions
static struct class *panda_class;   // Sysfs class support

static struct file_operations panda_fops[PANDA_MINORS] = { };

static const char *device_names[PANDA_MINORS] = {
    "panda.map",
    "panda.aaa",
    "panda.bbb",
};


static int panda_open(struct inode *inode, struct file *file)
{
    printk(KERN_INFO "Opening panda device: %d:%d\n",
        imajor(inode), iminor(inode));

    file->f_op = &panda_fops[iminor(inode)];
    if (file->f_op->open)
        return file->f_op->open(inode, file);
    else
        return 0;
}


static struct file_operations base_fops = {
    owner: THIS_MODULE,
    open: panda_open,
};


static int __init panda_init(void)
{
    printk(KERN_INFO "Loading PandA driver\n");
    int rc;

    /* First initialise the three PandA subcomponents. */
    for (int i = 0; i < PANDA_MINORS; i ++)
        panda_fops[i].owner = THIS_MODULE;
//     init_panda_mem(&panda_fops[0]);
//     init_panda_posn(&panda_fops[1]);
//     init_panda_pgen(&panda_fops[2]);

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
            panda_class, NULL, panda_dev + i, NULL, device_names[i]);

    return 0;


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
