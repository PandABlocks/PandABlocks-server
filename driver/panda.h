/* Common files and structures for PandA driver. */

/* Initialises register map support. */
int panda_map_init(struct file_operations **fops);

/* Initialises table block support. */
int panda_block_init(struct file_operations **fops);

/* Initialises stream support. */
int panda_stream_init(struct file_operations **fops);


/* PandA Platform Capabilities. */
struct panda_pcap {
    struct platform_device *pdev;   // Platform device
    struct cdev cdev;               // Associated character device

    unsigned long reg_base;         // Physical base of register area
    void __iomem *base_addr;        // Register area mapped into kernel memory
    unsigned int length;            // Length of register area

    unsigned int irq;
};
