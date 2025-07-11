/* Support for memory mapped contiguous blocks of memory. */

#include <linux/module.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/semaphore.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <asm/atomic.h>

#include "error.h"
#include "panda.h"

#include "panda_device.h"
#include "panda_drv.h"


/* Information associated with an open file. */
struct block_open {
    struct mutex lock;
    size_t block_size;              // Bytes in block
    struct panda_pcap *pcap;        // Device capabilites
    struct panda_block block;       // Block specification
    struct block_channel *channel;  // Channel used
    bool configured;                // Whether this was configured successfully
};


struct buffer_info {
    struct list_head list;
    struct block_channel *channel; // Convenient access to channel
    void *addr;                    // Kernel address of allocated block
    dma_addr_t dma;                // DMA address of block
    size_t length;                 // Bytes to transfer
    bool more;                     // Whether there will be more blocks after
};


void block_channel_init(struct block_channel *channel)
{
    memset(channel, 0, sizeof(struct block_channel));
    spin_lock_init(&channel->lock);
    INIT_LIST_HEAD(&channel->queue);
    INIT_LIST_HEAD(&channel->free);
}


static void free_buffer_locked(struct buffer_info *buffer)
{
    list_add(&buffer->list, &buffer->channel->free);
}


static void block_channel_free_buffers_locked(struct block_channel *channel)
{
    while (!list_empty(&channel->queue))
    {
        struct buffer_info *buffer =
            list_first_entry(&channel->queue, struct buffer_info, list);
        list_del(&buffer->list);
        free_buffer_locked(buffer);
    }
    if (channel->current_buffer)
    {
        free_buffer_locked(channel->current_buffer);
        channel->current_buffer = NULL;
    }
    if (channel->next_buffer)
    {
        free_buffer_locked(channel->next_buffer);
        channel->next_buffer = NULL;
    }
    WRITE_ONCE(channel->nwords, 0);
}


/* This assumes the block has been configured */
static void reset_dma_locked(struct block_channel *channel)
{
    writel(0, channel->length_reg);
    block_channel_free_buffers_locked(channel);
    channel->completed = false;
}


static int allocate_channel_buffers(
    struct block_channel *channel, unsigned int order, unsigned long nbuffers,
    struct device *dev)
{
    int rc = 0;
    unsigned long flags;
    LIST_HEAD(free_list);
    size_t block_size = 1U << (order + PAGE_SHIFT);
    for (int i=0; i < nbuffers; i++)
    {
        struct buffer_info *buffer = kmalloc(
            sizeof(struct buffer_info), GFP_KERNEL);
        TEST_OK(buffer, rc = -ENOMEM, no_buffer,
            "Unable to allocate buffer info structure");
        buffer->channel = channel;
        /* Ok, all in order.  Allocate the requested block and map it for DMA.
         * Flag GFP_DMA32 is set to avoid using bounce buffer. */
        buffer->addr =
            (void *) __get_free_pages(GFP_KERNEL | GFP_DMA32, order);
        if (!buffer->addr)
        {
            kfree(buffer);
            rc = -ENOMEM;
            dev_err(dev, "Unable to allocate buffer\n");
            goto no_buffer;
        }
        buffer->dma = dma_map_single(
            dev, buffer->addr, block_size, DMA_TO_DEVICE);
        if (dma_mapping_error(dev, buffer->dma))
        {
            free_pages((unsigned long) buffer->addr, order);
            kfree(buffer);
            rc = -ENOMEM;
            dev_err(dev, "Unable to map buffer\n");
            goto no_buffer;
        }
        list_add(&buffer->list, &free_list);
    }
    spin_lock_irqsave(&channel->lock, flags);
    list_splice_init(&free_list, &channel->free);
    spin_unlock_irqrestore(&channel->lock, flags);
    return 0;
no_buffer:
    while (!list_empty(&free_list))
    {
        struct buffer_info *buffer =
            list_first_entry(&free_list, struct buffer_info, list);
        list_del(&buffer->list);
        dma_unmap_single(dev, buffer->dma, block_size, DMA_TO_DEVICE);
        free_pages((unsigned long) buffer->addr, order);
        kfree(buffer);
    }
    return rc;
}


static struct  buffer_info *get_free_buffer(struct block_channel *channel)
{
    unsigned long flags;
    struct buffer_info *buffer = NULL;
    spin_lock_irqsave(&channel->lock, flags);
    if (!list_empty(&channel->free))
    {
        buffer = list_first_entry(&channel->free, struct buffer_info, list);
        list_del(&buffer->list);
    }
    spin_unlock_irqrestore(&channel->lock, flags);
    return buffer;
}


/* Prepares the backing memory as fixed blocks. */
static long config_block(
    struct block_open *open, const struct panda_block __user *block)
{
    struct panda_pcap *pcap = open->pcap;
    struct device *dev = &pcap->pdev->dev;
    int rc = 0;
    mutex_lock(&open->lock);
    /* Block can be configured only once per open */
    TEST_OK(
        !open->configured, rc = -EBUSY, was_configured,
        "Block was already configured");
    /* Try to retrieve the ioctl arguments and validate them. */
    TEST_OK(!copy_from_user(&open->block, block, sizeof(struct panda_block)),
        rc = -EFAULT, bad_request, "Error copying block");
    TEST_OK(
        open->block.block_base <= pcap->length - 4  &&
        open->block.block_length <= pcap->length - 4,
        rc = -EINVAL, bad_request, "Invalid register argument for block");
    TEST_OK(
        open->block.dma_channel < BLOCK_CHANNEL_COUNT,
        rc = -EINVAL, bad_request, "Invalid DMA channel");
    TEST_OK(
        open->block.nbuffers > 0,
        rc = -EINVAL, bad_request, "Invalid number of buffers");
    open->block_size = 1U << (open->block.order + PAGE_SHIFT);
    dev_dbg(dev,
        "Configuring block: dma_channel=%u, block_size=%zu, nbuffers=%u\n",
        open->block.dma_channel, open->block_size, open->block.nbuffers);
    struct block_channel *channel =
        &pcap->block_channels[open->block.dma_channel];
    open->channel = channel;
    unsigned long flags;
    bool had_user;
    spin_lock_irqsave(&channel->lock, flags);
    channel->addr_reg = pcap->reg_base + open->block.block_base;
    channel->length_reg = pcap->reg_base + open->block.block_length;
    had_user = channel->has_user;
    if (!had_user)
    {
        /* Make sure driver and FPGA are on the same page */
        reset_dma_locked(channel);
        channel->has_user = true;
    }
    spin_unlock_irqrestore(&channel->lock, flags);
    /* A channel can have only one user at a time */
    TEST_OK(
        !had_user, rc = -EBUSY, channel_busy, "Channel already in used");
    TEST_RC(rc=allocate_channel_buffers(
        channel, open->block.order, open->block.nbuffers, dev),
        no_buffer, "Unable to allocate buffers for channel");
    open->configured = true;
    mutex_unlock(&open->lock);
    return open->block_size;

no_buffer:
channel_busy:
bad_request:
was_configured:
    mutex_unlock(&open->lock);
    return rc;
}


/* This should be called with the channel lock taken, it returns true if
 * pushing was successful */
static bool push_buffer_locked(struct buffer_info *buffer)
{
    struct block_channel *channel = buffer->channel;
    if (channel->current_buffer == NULL)
        /* This buffer will be used immediately. */
        channel->current_buffer = buffer;
    else if (channel->next_buffer == NULL)
        /* This buffer will be used immediately after current_buffer */
        channel->next_buffer = buffer;
    else
        return false;

    /* Inform the hardware via the register we were given. */
    writel(buffer->dma, channel->addr_reg);
    writel(
        buffer->more ? buffer->length | (1<<31) : buffer->length,
        channel->length_reg);
    return true;
}


static long send_block(
    struct block_open *open, const struct panda_block_send_request __user *arg)
{
    struct panda_pcap *pcap = open->pcap;
    struct device *dev = &pcap->pdev->dev;
    struct block_channel *channel = open->channel;
    struct panda_block_send_request sreq;
    unsigned long flags;
    int rc = 0;
    TEST_OK(
        open->configured, rc = -EINVAL, bad_request, "Block was not configured");
    TEST_OK(
        !copy_from_user(&sreq, arg, sizeof(struct panda_block_send_request)),
        rc = -EFAULT, bad_request, "Error copying send request");
    /* Check the requested length is valid. */
    TEST_OK(sreq.length * sizeof(uint32_t) <= open->block_size,
        rc = -EFBIG, bad_request, "Write segment too long");
    if (!sreq.length)
    {
        dev_dbg(dev, "Block request with length=0, channel will be reset\n");
        spin_lock_irqsave(&channel->lock, flags);
        reset_dma_locked(channel);
        spin_unlock_irqrestore(&channel->lock, flags);
        return 0;
    }
    struct buffer_info *buffer;
    TEST_OK(
        buffer=get_free_buffer(channel), rc = -ENOMEM, no_buffer,
       "Unable to get free buffer");
    buffer->length = sreq.length;
    buffer->more = sreq.more;
    dma_sync_single_for_cpu(dev, buffer->dma, open->block_size, DMA_TO_DEVICE);
    TEST_OK(
        !copy_from_user(
            buffer->addr, (const char __user *) sreq.data,
            sreq.length * sizeof(uint32_t)),
        rc = -EFAULT, bad_copy, "Fault copying data from user");
    dma_sync_single_for_device(
        dev, buffer->dma, open->block_size, DMA_TO_DEVICE);
    spin_lock_irqsave(&channel->lock, flags);
    TEST_OK(!channel->completed,
        rc = -EPIPE, was_completed, "Block channel was completed");
    WRITE_ONCE(channel->nwords, READ_ONCE(channel->nwords) + buffer->length);
    if (!buffer->more)
        channel->completed = true;
    if(push_buffer_locked(buffer))
        dev_dbg(dev,
            "Block buffer described by 0x%pK (more=%d, length=%zu) was pushed\n",
            buffer, buffer->more, buffer->length);
    else
    {
        dev_dbg(dev, "Adding block buffer described by 0x%pK to queue\n", buffer);
        list_add(&buffer->list, &channel->queue);
    }
    spin_unlock_irqrestore(&channel->lock, flags);
    return 0;

was_completed:
    free_buffer_locked(buffer);
    spin_unlock_irqrestore(&channel->lock, flags);
bad_copy:
no_buffer:
bad_request:
    return rc;
}


static long get_nwords(struct block_open *open, const size_t __user *nwords)
{
    struct block_channel *channel = open->channel;
    if (!channel)
        return -EINVAL;
    return put_user(READ_ONCE(channel->nwords), nwords);
}


static long panda_block_ioctl(
    struct file *file, unsigned int cmd, unsigned long arg)
{
    struct block_open *open = file->private_data;
    switch (cmd)
    {
        case PANDA_BLOCK_CONFIG:
            return config_block(open, (const void __user *) arg);
        case PANDA_BLOCK_SEND:
            return send_block(open, (const void __user *) arg);
        case PANDA_BLOCK_NWORDS:
            return get_nwords(open, (const void __user *) arg);
        default:
            return -EINVAL;
    }
}


irqreturn_t block_isr(int irq, void *dev_id)
{
    struct panda_pcap *pcap = dev_id;
    struct device *dev = &pcap->pdev->dev;
    void *reg_base = pcap->reg_base;
    uint32_t status = readl(reg_base + REG_TABLE_IRQ_STATUS);
    dev_dbg(dev, "Block ISR status: %08x\n", status);
    uint16_t completed = status >> 16;
    uint16_t ready = status & 0xFFFF;
    for (int i=0; i < BLOCK_CHANNEL_COUNT; i++)
    {
        struct block_channel *channel = &pcap->block_channels[i];
        unsigned long flags;
        /* Completion takes priority over becoming ready */
        if (completed & (1 << i))
        {
            spin_lock_irqsave(&channel->lock, flags);
            channel->completed = true;
            block_channel_free_buffers_locked(channel);
            spin_unlock_irqrestore(&channel->lock, flags);
        }
        else if (ready & (1 << i))
        {
            spin_lock_irqsave(&channel->lock, flags);
            /* The channel is ready for a new block. */
            if (channel->current_buffer && channel->next_buffer)
            {
                /* The current block has been used and can now be released */
                struct buffer_info *buffer = channel->current_buffer;
                channel->current_buffer = channel->next_buffer;
                channel->next_buffer = NULL;
                WRITE_ONCE(
                    channel->nwords,
                    READ_ONCE(channel->nwords) - buffer->length);
                free_buffer_locked(buffer);
            }
            if (!list_empty(&channel->queue))
            {
                struct buffer_info *buffer =
                    list_last_entry(&channel->queue, struct buffer_info, list);
                list_del(&buffer->list);
                if(push_buffer_locked(buffer))
                    dev_dbg(dev,
                        "Block buffer described by 0x%pK was pushed\n", buffer);
                else
                    /* this should not happen, but if it does, we
                     * just free the buffer to avoid leaks */
                    free_buffer_locked(buffer);
            }
            spin_unlock_irqrestore(&channel->lock, flags);
        }
    }

    return IRQ_HANDLED;
}


static int panda_block_open(struct inode *inode, struct file *file)
{
    int rc = 0;
    struct block_open *open = kmalloc(sizeof(*open), GFP_KERNEL);
    TEST_PTR(open, rc, no_open, "Unable to allocate open structure");

    *open = (struct block_open) {
        .pcap = container_of(inode->i_cdev, struct panda_pcap, cdev),
    };
    mutex_init(&open->lock);
    file->private_data = open;

no_open:
    return rc;
}


static int panda_block_release(struct inode *inode, struct file *file)
{
    struct block_open *open = file->private_data;
    struct block_channel *channel = open->channel;
    struct device *dev = &open->pcap->pdev->dev;
    mutex_lock(&open->lock);
    if (open->configured)
    {
        unsigned long flags;
        LIST_HEAD(free_list);
        spin_lock_irqsave(&channel->lock, flags);
        reset_dma_locked(channel);
        channel->has_user = false;
        list_splice_init(&channel->free, &free_list);
        spin_unlock_irqrestore(&channel->lock, flags);
        unsigned int count = 0;
        while (!list_empty(&free_list))
        {
            struct buffer_info *buffer =
                list_first_entry(&free_list, struct buffer_info, list);
            list_del(&buffer->list);
            dma_unmap_single(dev, buffer->dma, open->block_size, DMA_TO_DEVICE);
            free_pages((unsigned long) buffer->addr, open->block.order);
            kfree(buffer);
            count++;
        }
        if (count != open->block.nbuffers)
        {
            dev_err(dev, "LEAK: Released %d buffers, expected %u\n", count,
                open->block.nbuffers);
        }
    }
    mutex_unlock(&open->lock);
    kfree(open);
    return 0;
}


struct file_operations panda_block_fops = {
    .owner = THIS_MODULE,
    .open = panda_block_open,
    .release = panda_block_release,
    .unlocked_ioctl = panda_block_ioctl,
};
