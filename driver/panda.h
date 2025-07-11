#include <linux/irq.h>

/* Common files and structures for PandA driver. */
#define BLOCK_CHANNEL_COUNT 16

extern struct file_operations panda_map_fops;
extern struct file_operations panda_block_fops;
extern struct file_operations panda_stream_fops;
struct block_entry;

/* This structure keeps track of queued buffers, free buffers and the
 * information needed to push a buffer to a dma block instance */
struct block_channel {
    spinlock_t lock;                     // Protect access to this structure
    struct list_head queue;              // Queue of buffers to be sent
    struct list_head free;               // Buffers free to use
    struct buffer_info *current_buffer;  // Block currently being transferred
    struct buffer_info *next_buffer;     // Block in FPGA possibly waiting
    void __iomem *addr_reg;              // Address to address register
    void __iomem *length_reg;            // Address to length register
    size_t nwords;                       // Number of 4-byte words scheduled
    bool completed;                      // Did channel stream finish?
    bool has_user;                       // Is anyone using this channel?
};

/* PandA Platform Capabilities. */
struct panda_pcap {
    struct platform_device *pdev;   // Platform device
    struct cdev cdev;               // Associated character device

    unsigned long base_page;        // Base page of register area from resource
    void __iomem *reg_base;         // Register area mapped into kernel memory
    unsigned int length;            // Length of register area

    unsigned int stream_irq;
    unsigned int block_irq;

    struct block_channel block_channels[BLOCK_CHANNEL_COUNT];
};

irqreturn_t block_isr(int irq, void *dev_id);
void block_channel_init(struct block_channel *channel);
