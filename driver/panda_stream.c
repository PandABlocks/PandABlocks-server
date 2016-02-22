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

module_param(block_shift, int, S_IRUGO);
module_param(block_count, int, S_IRUGO);
module_param(block_timeout, int, S_IRUGO);

#define BUF_BLOCK_SIZE      (1U << (block_shift + PAGE_SHIFT))


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

    bool shutdown;              // Set when shutdown requested
    bool end_of_stream;         // Set by ISR on completion
    uint32_t completion;        // Copy of interrupt status register

    /* Reader status. */
    int read_block_index;       // Block being read
    int read_offset;            // Read offset into block

    /* Circular buffer of blocks. */
    struct block {
        void *block;            // Virtual address of block
        dma_addr_t dma;         // DMA address of block
        enum block_state state; // State
        size_t length;          // Number of bytes in this block
    } blocks[];
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Interrupt handling. */


/* Pushes specified DMA buffer to hardware. */
static void assign_buffer(struct stream_open *open, int n)
{
    void *reg_base = open->pcap->reg_base;
    struct block *block = &open->blocks[n];
    writel(block->dma, reg_base + PCAP_DMA_ADDR);
    block->state = BLOCK_DMA;
}


/* Advances circular buffer pointer. */
static inline int step_index(int ix)
{
    ix += 1;
    if (ix >= block_count)
        ix -= block_count;
    return ix;
}


/* We get an interrupt every time the hardware has finished with a DMA block.
 * This can be because the block is full, because there was a data transfer
 * timeout, or because data transfer is (currently) complete.
 *    The interrupt status register records the following information:
 *
 *   31           16     6 5 4 3 2 1 0
 *  +---------------+---+-+-+-+-+-+-+-+
 *  | sample count  | 0 | | | | | | | |
 *  +---------------+---+-+-+-+-+-+-+-+
 *                       | | | | | | +-- Transfer complete, no more interrupts
 *                       | | | | | +---- Experiment disarmed
 *                       | | | | +------ Capture framing error
 *                       | | | +-------- DMA error
 *                       | | +---------- DMA address not written in time
 *                       | +------------ Timeout
 *                       +-------------- Block complete
 * Bits 4:1 are used to record the completion reason (when bit 0 is set).
 * Otherwise one of bits 0, 5, 6 is set.  The sample count is in 4-byte
 * transfers. */
#define IRQ_STATUS_COMPLETE(status)     ((status) & 0x01)
#define IRQ_STATUS_TIMEOUT(status)      ((status) & 0x20)
#define IRQ_STATUS_FULL(status)         ((status) & 0x40)
#define IRQ_STATUS_LENGTH(status)       (((status) >> 8) << 2)


static void advance_isr_block(struct stream_open *open)
{
    struct panda_pcap *pcap = open->pcap;
    struct device *dev = &pcap->pdev->dev;

    /* Prepare for next interrupt. */
    open->isr_block_index = step_index(open->isr_block_index);
    int next_ix = step_index(open->isr_block_index);
    struct block *block = &open->blocks[next_ix];

    smp_rmb();  // Guards copy_to_user for free block.
    if (block->state == BLOCK_FREE)
    {
        dma_sync_single_for_device(
            dev, block->dma, BLOCK_SIZE, DMA_FROM_DEVICE);
        assign_buffer(open, next_ix);
    }
    else
        /* Whoops.  Next buffer isn't free. */
        printk(KERN_DEBUG "Data buffer overrun\n");
}


static irqreturn_t stream_isr(int irq, void *dev_id)
{
    struct stream_open *open = dev_id;
    struct panda_pcap *pcap = open->pcap;
    struct device *dev = &pcap->pdev->dev;
    void *reg_base = open->pcap->reg_base;

    uint32_t status = readl(reg_base + PCAP_IRQ_STATUS);

    open->completion = status;
    open->end_of_stream = IRQ_STATUS_COMPLETE(status);

    /* Update the block we've just completed. */
    struct block *block = &open->blocks[open->isr_block_index];
    block->length = IRQ_STATUS_LENGTH(status);
    dma_sync_single_for_cpu(dev, block->dma, BLOCK_SIZE, DMA_FROM_DEVICE);
    smp_wmb();  // Guards DMA transfer for block we've just read
    block->state = BLOCK_DATA;

    if (open->shutdown)
    {
        if (open->end_of_stream)
            complete(&open->isr_done);
    }
    else
        advance_isr_block(open);

    wake_up_interruptible(&open->wait_queue);
    return IRQ_HANDLED;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Open and close. */

/* We only permit a single device instance at a time. */
static atomic_t device_open = ATOMIC_INIT(0);


/* Allocate the block buffers. */
static int allocate_blocks(struct stream_open *open)
{
    struct device *dev = &open->pcap->pdev->dev;
    int rc = 0;
    int blk = 0;
    for (; blk < block_count; blk ++)
    {
        struct block *block = &open->blocks[blk];
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
            dev, open->blocks[blk].dma, BUF_BLOCK_SIZE, DMA_FROM_DEVICE);
no_dma_map:
        free_pages((unsigned long) open->blocks[blk].block, block_shift);
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
        struct block *block = &open->blocks[blk];
        dma_unmap_single(dev, block->dma, BUF_BLOCK_SIZE, DMA_FROM_DEVICE);
        free_pages((unsigned long) block->block, block_shift);
    }
}


static void start_hardware(struct stream_open *open)
{
    void *reg_base = open->pcap->reg_base;
    writel(block_timeout, reg_base + PCAP_TIMEOUT);
    writel(BUF_BLOCK_SIZE, reg_base + PCAP_BLOCK_SIZE);
    assign_buffer(open, 0);
    writel(1, reg_base + PCAP_DMA_START);
    assign_buffer(open, 1);
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
    writel(0, pcap->reg_base + PCAP_DMA_RESET);

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

    /* Ensure we shutdown. */
    open->shutdown = true;      // Really want a reset ISR status instead
    writel(0, pcap->reg_base + PCAP_DMA_RESET);
    if (wait_for_completion_timeout(&open->isr_done, HZ) == 0)
        /* Whoops.  If this happens we're in trouble, because we don't know what
         * blocks are being written to. */
        printk(KERN_EMERG "PandA interrupt routine did not complete\n");

    devm_free_irq(dev, pcap->irq, open);
    free_blocks(open);
    kfree(open);
    atomic_dec(&device_open);
    return 0;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Reading. */


/* Blocks caller until the current block becomes ready. */
static int wait_for_block(struct stream_open *open)
{
    struct block *block = &open->blocks[open->read_block_index];
    int rc = wait_event_interruptible_timeout(open->wait_queue,
        block->state == BLOCK_DATA, HZ);
    if (rc == 0)
        /* Normal timeout.  Tell caller they can try again. */
        return -EAGAIN;
    else
        return rc;
}


/* Consumes as much as possible of the current block. */
static ssize_t read_one_block(
    struct stream_open *open, char __user *buf, size_t count)
{
    struct block *block = &open->blocks[open->read_block_index];
    smp_rmb();  // Guards DMA transfer for new data block
    if (block->length > 0)
    {
        size_t read_offset = open->read_offset;
        size_t copy_count = block->length - read_offset;
        if (copy_count > count)  copy_count = count;
        copy_count -= copy_to_user(buf, block->block + read_offset, copy_count);
        if (copy_count == 0)
            return -EFAULT;
        else
        {
            open->read_offset += copy_count;
            return copy_count;
        }
    }
    else
        return 0;
}


/* Returns fully read block to free pool, advances to next block. */
static struct block *advance_block(struct stream_open *open)
{
    struct block *block = &open->blocks[open->read_block_index];
    open->read_offset = 0;
    open->read_block_index = step_index(open->read_block_index);
    smp_wmb();  // Guards copy_to_user for block we're freeing
    block->state = BLOCK_FREE;
    return &open->blocks[open->read_block_index];
}


static ssize_t panda_stream_read(
    struct file *file, char __user *buf, size_t count, loff_t *f_pos)
{
    struct stream_open *open = file->private_data;

    if (open->end_of_stream)
        return 0;

    /* Wait for data to arrive in the current block or timeout. */
    int rc = wait_for_block(open);
    if (rc < 0)
        return rc;

    /* Copy as much as we can out of each available block, but don't do any more
     * waits.  This means we'll remain caught up with the available data stream
     * as far as buffering allows. */
    ssize_t copied = 0;
    struct block *block = &open->blocks[open->read_block_index];
    while (count > 0  &&  block->state == BLOCK_DATA)
    {
        ssize_t copy_count = read_one_block(open, buf, count);
        if (copy_count < 0)
            /* By not checking copied here we may lose data ... however, in
             * this case we're in trouble anyway so it doesn't matter. */
            return copy_count;
        else
        {
            copied += copy_count;
            count -= copy_count;
            buf += copy_count;
        }

        if (open->read_offset >= block->length)
            block = advance_block(open);
    }

    /* At this point there is no error condition, so just decode the three
     * normal states. */
    if (copied > 0)
        return copied;              // Normal data flow
    else if (open->end_of_stream)
        return 0;                   // End of data stream
    else
        return -EAGAIN;             // No data yet, try again
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Completion ioctl. */


static long stream_completion(
    struct stream_open *open, uint32_t __user *completion)
{
    *completion = open->completion;
    open->end_of_stream = true;
    return 0;
}


static long panda_stream_ioctl(
    struct file *file, unsigned int cmd, unsigned long arg)
{
    struct stream_open *open = file->private_data;
    switch (cmd)
    {
        case PANDA_COMPLETION:
            return stream_completion(open, (void __user *) arg);
        default:
            return -EINVAL;
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct file_operations panda_stream_fops = {
    .owner = THIS_MODULE,
    .open       = panda_stream_open,
    .release    = panda_stream_release,
    .read       = panda_stream_read,
    .unlocked_ioctl = panda_stream_ioctl,
};
