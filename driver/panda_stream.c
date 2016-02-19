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
#include <linux/dma-mapping.h>
#include <asm/atomic.h>

#include "error.h"
#include "panda.h"

#include "panda_device.h"
#include "panda_drv.h"          // *DRV register definitions


/* Module parameters for buffer blocks. */
static int block_shift = 6;     // In log2 pages, default is 256KB
static int block_count = 3;     // Number of buffers in circular buffer
static int block_timeout = 12500000;    // 100ms in 125MHz FPGA clock ticks

#define BUF_BLOCK_SIZE      (1U << (block_shift + PAGE_SHIFT))

module_param(block_shift, int, S_IRUGO);
module_param(block_count, int, S_IRUGO);
module_param(block_timeout, int, S_IRUGO);


/* We only permit a single device instance at a time. */
static atomic_t device_open = ATOMIC_INIT(0);


/* The character device interface provides a very simple streaming API: open
 * /dev/panda.stream and read blocks continuously to access the data stream.
 * If reads are not fast enough then overrun is detected and read() will
 * eventually fail (with EIO).
 *
 * A circular buffer of DMA buffers is managed by the driver.  At any instant
 * two of the buffers are assigned to the hardware (one actively being
 * transferred into, one configured for the next DMA transfer).  Each
 * transfer generates an interrupt: the first buffer is then handed over to
 * the reader, and a fresh DMA buffer is configured for transfer.
 *
 * Buffers transition through the following sequence of states:
 *
 *  +-> block_free       Block is currently unassigned
 *  |       |
 *  |       | ISR assigns block to hardware
 *  |       v
 *  |   block_dma        Block is assigned to hardware for DMA
 *  |       |
 *  |       | ISR marks block as complete
 *  |       v
 *  |   block_data       Block contains valid data to be read
 *  |       |
 *  |       | read() completes, marks block as free
 *  +-------+
 */

enum block_state {
    BLOCK_FREE,                 // Not in use
    BLOCK_DMA,                  // Allocated to DMA
    BLOCK_DATA,                 // Contains useful data.
};


struct stream_open {
    struct panda_pcap *pcap;

    /* Communication with interrupt routine. */
    wait_queue_head_t wait_queue;   // Used by read to wait for data ready
    struct completion isr_done; // Used on close to sync with final interrupt
    int isr_block_index;        // Block currently being written by hardware
    bool buffer_overrun;        // Whoops.

    /* Reader status. */
    int read_block_index;       // Block being read
    int read_offset;            // Read offset into block

    /* Circular buffer of blocks. */
    struct block {
        void *block;            // Virtual address of block
        dma_addr_t dma;         // DMA address of block
        enum block_state state; // State
        size_t length;          // Number of bytes in this block
    } buffers[];
};


/* Allocate the block buffers. */
static int allocate_blocks(struct stream_open *open)
{
    struct device *dev = &open->pcap->pdev->dev;
    int rc = 0;
    int blk = 0;
    for (; blk < block_count; blk ++)
    {
        struct block *block = &open->buffers[blk];
        block->block = (void *) __get_free_pages(GFP_KERNEL, block_shift);
        TEST_(block->block,
            rc = -ENOMEM, no_block, "Unable to allocate buffer");
        block->dma =
            dma_map_single(dev, block->block, BUF_BLOCK_SIZE, DMA_FROM_DEVICE);
        TEST_(!dma_mapping_error(dev, block->dma),
            rc = -EIO, no_dma_map, "Unable to map DMA block");
        block->state = BLOCK_FREE;
    }
    return 0;

    /* Release circular buffer resources on error.  Rather tricky interaction
     * with allocation loop above so that we release precisely those resources
     * we allocated, in reverse order. */
    do {
        blk -= 1;
        dma_unmap_single(
            dev, open->buffers[blk].dma, BUF_BLOCK_SIZE, DMA_FROM_DEVICE);
no_dma_map:
        free_pages((unsigned long) open->buffers[blk].block, block_shift);
no_block:
        ;
    } while (blk > 0);
    return rc;
}


static void free_blocks(struct stream_open *open)
{
    struct device *dev = &open->pcap->pdev->dev;
    for (int blk = 0; blk < block_count; blk ++)
    {
        struct block *block = &open->buffers[blk];
        dma_unmap_single(dev, block->dma, BUF_BLOCK_SIZE, DMA_FROM_DEVICE);
        free_pages((unsigned long) block->block, block_shift);
    }
}


static void assign_buffer(struct stream_open *open, int n)
{
    void *reg_base = open->pcap->base_addr;
    struct block *block = &open->buffers[n];
    writel(block->dma, reg_base + PCAP_DMA_ADDR);
    block->state = BLOCK_DMA;
}


static void start_hardware(struct stream_open *open)
{
    void *reg_base = open->pcap->base_addr;
    writel(block_timeout, reg_base + PCAP_TIMEOUT);
    writel(BUF_BLOCK_SIZE, reg_base + PCAP_BLOCK_SIZE);
    assign_buffer(open, 0);
    writel(1, reg_base + PCAP_DMA_START);
    assign_buffer(open, 1);
}


static irqreturn_t stream_isr(int irq, void *dev_id)
{
    struct stream_open *open = dev_id;
    void *reg_base = open->pcap->base_addr;

    printk(KERN_INFO "IRQ received: %08x\n", readl(reg_base + PCAP_IRQ_STATUS));
    return IRQ_HANDLED;
}


static int panda_stream_open(struct inode *inode, struct file *file)
{
printk(KERN_INFO "Opening stream: %u\n", BUF_BLOCK_SIZE);
    struct panda_pcap *pcap =
        container_of(inode->i_cdev, struct panda_pcap, cdev);

    /* Only permit one user. */
    if (!atomic_add_unless(&device_open, 1, 1))
        return -EBUSY;

    /* Stop the hardware, just in case it's still going. */
    writel(0, pcap->base_addr + PCAP_DMA_RESET);

    int rc = 0;
    struct stream_open *open = kmalloc(
        sizeof(*open) + block_count * sizeof(struct block), GFP_KERNEL);
    TEST_PTR(open, rc, no_open, "Unable to allocate open structure");

    file->private_data = open;
    *open = (struct stream_open) {
        .pcap = pcap,
    };

    /* Initialise the ISR and read fields. */
    init_waitqueue_head(&open->wait_queue);
    init_completion(&open->isr_done);

    rc = allocate_blocks(open);
    if (rc) goto no_blocks;

    /* Establish interrupt handler. */
    rc = devm_request_irq(
        &pcap->pdev->dev, pcap->irq, stream_isr, 0, pcap->pdev->name, open);
    TEST_RC(rc, no_irq, "Unable to request irq");

    /* Start the hardware going. */
    start_hardware(open);
    return 0;

no_irq:
    free_blocks(open);
no_blocks:
    kfree(open);
no_open:
    return rc;
}


static int panda_stream_release(struct inode *inode, struct file *file)
{
printk(KERN_INFO "Closing stream\n");
    struct stream_open *open = file->private_data;
    struct panda_pcap *pcap = open->pcap;
    struct device *dev = &pcap->pdev->dev;

    devm_free_irq(dev, pcap->irq, open);
    free_blocks(open);
    kfree(open);
    atomic_dec(&device_open);
    return 0;
}


static ssize_t panda_stream_read(
    struct file *file, char __user *buf, size_t count, loff_t *f_pos)
{
printk(KERN_INFO "read timeout\n");
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
