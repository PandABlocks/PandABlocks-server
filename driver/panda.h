/* Common files and structures for PandA driver. */

extern struct file_operations panda_map_fops;
extern struct file_operations panda_block_fops;
extern struct file_operations panda_stream_fops;

/* PandA Platform Capabilities. */
struct panda_pcap {
    struct platform_device *pdev;   // Platform device
    struct cdev cdev;               // Associated character device

    void __iomem *reg_base;        // Register area mapped into kernel memory
    unsigned int length;            // Length of register area

    unsigned int irq;
};
