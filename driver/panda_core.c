/* Entry point for PandA interface kernel module. */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/mod_devicetable.h>
#include <linux/io.h>

#include "error.h"
#include "panda.h"
#include "panda_drv.h"


MODULE_AUTHOR("Michael Abbott, Diamond Light Source Ltd");
MODULE_DESCRIPTION("PandA device driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("0");


#define PANDA_MINORS    ARRAY_SIZE(panda_info)


/* The fops and name fields of this structure are filled in by the appropriate
 * subcomponents of this module. */
struct panda_info {
    const char *name;
    struct file_operations *fops;
};
static struct panda_info panda_info[] = {
    { .name = "map",    .fops = &panda_map_fops, },
    { .name = "block",  .fops = &panda_block_fops, },
    { .name = "stream", .fops = &panda_stream_fops, },
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
    pcap->base_page = res->start >> PAGE_SHIFT;
    pcap->length = resource_size(res);
    pcap->reg_base = devm_ioremap_resource(&pdev->dev, res);
    TEST_PTR(pcap->reg_base, rc, no_res, "Unable to map resource");

    rc = platform_get_irq(pdev, 0);
    TEST_RC(rc, no_irq, "Unable to read irq");
    pcap->irq = rc;

    /* Check the driver and FPGA protocol version match. */
    TEST_OK(readl(pcap->reg_base + COMPAT_VERSION) == DRIVER_COMPAT_VERSION,
        rc = -EINVAL, bad_version, "Driver compatibility version mismatch");

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
bad_version:
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

    /* Allocate device number for this function. */
    int rc = alloc_chrdev_region(&panda_dev, 0, PANDA_MINORS, "panda");
    TEST_RC(rc, no_chrdev, "unable to allocate dev region");

    /* Publish devices in sysfs. */
    panda_class = class_create(THIS_MODULE, "panda");
    TEST_PTR(panda_class, rc, no_class, "unable to create class");

    /* Register the platform driver. */
    rc = platform_driver_register(&panda_driver);
    TEST_RC(rc, no_platform, "Unable to register platform");

    return 0;

    platform_driver_unregister(&panda_driver);
no_platform:
    class_destroy(panda_class);
no_class:
    unregister_chrdev_region(panda_dev, PANDA_MINORS);
no_chrdev:
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
