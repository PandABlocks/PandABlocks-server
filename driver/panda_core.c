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


/* Flag to enable ARM performance counter PMCCNTR. */
static int enable_pmccntr = 0;

module_param(enable_pmccntr, int, S_IRUGO);


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
    pcap->length = resource_size(res);
    pcap->reg_base = devm_ioremap_resource(&pdev->dev, res);
    TEST_PTR(pcap->reg_base, rc, no_res, "Unable to map resource");

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


/* The ARM CPU cycle counter can be read by the instruction
 *      mrc p15, 0, Rd, c9, c13, 0      // PMCCNTR
 * Alas, this is disabled from user-space by default, so we do the necessary
 * dance here to enable it.  Much of the information here is taken from
 *      http://neocontra.blogspot.co.uk/2013/05/
 *          user-mode-performance-counters-for.html
 * and the ARMv7-A architecture reference manual. */
static void enable_cpu_counters(void *data)
{
    /* Set the E bit in PMCR to enable performance counters and specifically
     * enable PMCCNTR by setting the C bit in PMINTENSET. */
    asm volatile ("mcr p15, 0, %0, c9, c12, 0" :: "r"(1));
    asm volatile ("mcr p15, 0, %0, c9, c12, 1" :: "r"(0x80000000));

    /* Write 1 to PMUSERENR to enable user access to performance monitor
     * registers. */
    asm volatile ("mcr p15, 0, %0, c9, c14, 0" :: "r"(1));
}

/* Disables all the CPU counters -- this seems to be the default state. */
static void disable_cpu_counters(void *data)
{
    asm volatile ("mcr p15, 0, %0, c9, c12, 0" :: "r"(0));
    asm volatile ("mcr p15, 0, %0, c9, c12, 1" :: "r"(0));
    asm volatile ("mcr p15, 0, %0, c9, c14, 0" :: "r"(0));
}


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

    /* If enable_pmccntr selected then configure the CPU counters for userspace
     * applications. */
    if (enable_pmccntr)
        on_each_cpu(enable_cpu_counters, NULL, 1);

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

    if (enable_pmccntr)
        on_each_cpu(disable_cpu_counters, NULL, 1);
    platform_driver_unregister(&panda_driver);
    class_destroy(panda_class);
    unregister_chrdev_region(panda_dev, PANDA_MINORS);
}


module_init(panda_init);
module_exit(panda_exit);
