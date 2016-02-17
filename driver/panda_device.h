/* This header file is designed to be shared with userspace. */

/* Returns size of register area as an unsigned 32-bit integer. */
#define PANDA_MAP_SIZE      _IOR('P', 0, uint32_t)

/* Allocates area of memory with physical and virtual addresses. */
struct panda_block {
    unsigned int order;         // log2 number of pages requested in block
    /* The following fields are filled in. */
    size_t block_size;          // Number of bytes in allocated block
    void *block;                // Virtual address of page
    uint32_t phy_address;       // Physical address for DMA assignment
    unsigned int block_id;      // Block id used for release and flush calls
};

/* This ioctl is used to allocate a block of memory.  The physical and virtual
 * address of the created block are written into the given structure. */
#define PANDA_BLOCK_CREATE  _IOWR('P', 1, struct panda_block)
#define PANDA_BLOCK_RELEASE _IOW('P', 2, unsigned int)
#define PANDA_BLOCK_FLUSH   _IOW('P', 3, unsigned int)
