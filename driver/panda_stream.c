/* Stream device for retrieving captured data. */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <asm/atomic.h>

#include "error.h"
#include "panda.h"

#include "panda_device.h"


#define PCAP_IRQ_STATUS ((25<<12) | (4 << 2))


static atomic_t device_open = ATOMIC_INIT(0);


struct stream_open {
    struct panda_pcap *pcap;
};


static irqreturn_t stream_isr(int irq, void *dev_id)
{
    struct stream_open *open = dev_id;
    void *reg_base = open->pcap->base_addr;

    printk(KERN_INFO "IRQ received: %08x\n", readl(reg_base + PCAP_IRQ_STATUS));
    return IRQ_HANDLED;
}


static int panda_stream_open(struct inode *inode, struct file *file)
{
printk(KERN_INFO "Opening stream\n");

    /* Only permit one user. */
    if (!atomic_add_unless(&device_open, 1, 1))
        return -EBUSY;

    int rc = 0;
    struct stream_open *open = kmalloc(sizeof(*open), GFP_KERNEL);
    TEST_PTR(open, rc, no_open, "Unable to allocate open structure");

    *open = (struct stream_open) {
        /* Pick up the pcap structure. */
        .pcap = container_of(inode->i_cdev, struct panda_pcap, cdev),
    };
    file->private_data = open;

    /* Establish interrupt handler. */
    struct panda_pcap *pcap = open->pcap;
    rc = devm_request_irq(
        &pcap->pdev->dev, pcap->irq, stream_isr, 0, pcap->pdev->name, open);
    TEST_RC(rc, no_irq, "Unable to request irq");
printk(KERN_INFO "IRQ allocated\n");

    return rc;

no_irq:
    kfree(open);
no_open:
    return rc;
}


static int panda_stream_release(struct inode *inode, struct file *file)
{
printk(KERN_INFO "Closing stream\n");
    struct stream_open *open = file->private_data;
    struct panda_pcap *pcap = open->pcap;
    devm_free_irq(&pcap->pdev->dev, pcap->irq, open);
    kfree(open);
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


int panda_stream_init(struct file_operations **fops)
{
    *fops = &panda_stream_fops;
    return 0;
}
