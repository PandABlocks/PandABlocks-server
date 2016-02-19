/* Entry point for PandA interface kernel module. */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/io.h>

#include "error.h"
#include "panda.h"


MODULE_AUTHOR("Michael Abbott, Diamond Light Source Ltd");
MODULE_DESCRIPTION("PandA device driver");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("panda");
MODULE_VERSION("0");


#define PANDA_MINORS    ARRAY_SIZE(panda_info)


/* The fops and name fields of this structure are filled in by the appropriate
 * subcomponents of this module. */
struct panda_info {
    const char *name;
    int (*init)(struct file_operations **fops);
    struct file_operations *fops;
};
static struct panda_info panda_info[] = {
    { .name = "map",    .init = panda_map_init, },
    { .name = "block",  .init = panda_block_init, },
    { .name = "stream", .init = panda_stream_init, },
};


/* Kernel file and device interface state. */
static dev_t panda_dev;             // Major device for PandA support
static struct class *panda_class;   // Sysfs class support



static int panda_open(struct inode *inode, struct file *file)
{
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


static int panda_probe(struct platform_device *pdev)
{
    int rc = 0;

    /* Allocate global platform capability structure. */
    struct panda_pcap *pcap = kmalloc(sizeof(struct panda_pcap), GFP_KERNEL);
    TEST_PTR(pcap, rc, no_pcap, "Unable to allocate pcap");
    platform_set_drvdata(pdev, pcap);
    pcap->pdev = pdev;

    /* Pick up the register area and assigned IRQ from the device tree. */
    struct resource *res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    pcap->reg_base = res->start;
    pcap->length = resource_size(res);
    pcap->base_addr = devm_ioremap_resource(&pdev->dev, res);
    TEST_PTR(pcap->base_addr, rc, no_res, "Unable to map resource");

    rc = platform_get_irq(pdev, 0);
    TEST_RC(rc, no_irq, "Unable to read irq");
    pcap->irq = rc;

    /* Create character device support. */
    cdev_init(&pcap->cdev, &base_fops);
    pcap->cdev.owner = THIS_MODULE;
    rc = cdev_add(&pcap->cdev, panda_dev, PANDA_MINORS);
    TEST_RC(rc, no_cdev, "unable to add device");

    /* Create the device nodes. */
    int major = MAJOR(panda_dev);
    for (int i = 0; i < PANDA_MINORS; i ++)
        device_create(
            panda_class, &pdev->dev, MKDEV(major, i), NULL,
            "panda.%s", panda_info[i].name);

    return 0;

no_cdev:
no_irq:
no_res:
    kfree(pcap);
no_pcap:
    return rc;
}


static int panda_remove(struct platform_device *pdev)
{
    struct panda_pcap *pcap = platform_get_drvdata(pdev);
    int major = MAJOR(panda_dev);
    for (int i = 0; i < PANDA_MINORS; i ++)
        device_destroy(panda_class, MKDEV(major, i));
    cdev_del(&pcap->cdev);
    kfree(pcap);
    printk(KERN_INFO "PandA removed\n");
    return 0;
}


static struct platform_driver panda_driver = {
    .probe = panda_probe,
    .remove = panda_remove,
    .driver = {
        .name = "panda",
        .owner = THIS_MODULE,
        .of_match_table = (struct of_device_id[]) {
            { .compatible = "xlnx,panda-pcap-1.0", },
            { /* End of table. */ },
        },
    },
};


static int __init panda_init(void)
{
    printk(KERN_INFO "Loading PandA driver\n");

    /* First initialise the three PandA subcomponents. */
    int rc = 0;
    for (int i = 0; rc == 0  &&  i < PANDA_MINORS; i ++)
    {
        struct panda_info *info = &panda_info[i];
        rc = info->init(&info->fops);
    }
    TEST_RC(rc, no_panda, "Unable to initialise PandA");

    /* Allocate device number for this function. */
    rc = alloc_chrdev_region(&panda_dev, 0, PANDA_MINORS, "panda");
    TEST_RC(rc, no_chrdev, "unable to allocate dev region");

    /* Publish devices in sysfs. */
    panda_class = class_create(THIS_MODULE, "panda");
    TEST_PTR(panda_class, rc, no_class, "unable to create class");

    /* Register the platform driver. */
    rc = platform_driver_register(&panda_driver);
    TEST_RC(rc, no_platform, "Unable to register platform");

    return 0;

no_platform:
no_class:
    unregister_chrdev_region(panda_dev, PANDA_MINORS);
no_chrdev:
no_panda:
    platform_driver_unregister(&panda_driver);

    return rc;
}


static void __exit panda_exit(void)
{
    printk(KERN_INFO "Unloading PandA driver\n");

    platform_driver_unregister(&panda_driver);
    class_destroy(panda_class);
    unregister_chrdev_region(panda_dev, PANDA_MINORS);
}


module_init(panda_init);
module_exit(panda_exit);
