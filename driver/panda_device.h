/* This header file is designed to be shared with userspace. */

/* Returns size of register area as an unsigned 32-bit integer. */
#define PANDA_MAP_SIZE      _IO('P', 0)

/* Before a panda.block file can be used it must be initialised by configuring
 * the register set and block size. */
struct panda_block {
    unsigned int order;         // log2 of number of pages in block
    /* Both the register addresses are byte offsets into the PandA register
     * area. */
    unsigned int block_base;    // Register for setting block base address
    unsigned int block_length;  // Register for setting current block length
    unsigned int dma_channel;   // Which DMA channel we are pushing the block to
    unsigned int nbuffers;      // Number of buffers that will be allocated
};

struct panda_block_send_request {
    /* Pointer to a buffer with the data to be sent */
    const void *data;
    /* Length of the data in the buffer in 32-bit words */
    size_t length;
    /* Whether there will be more block send requests */
    bool more;
};

/* Each open panda.block file must have its block size set by calling the
 * PANDA_BLOCK_CONFIG ioctl to set the block order.  The physical address is
 * returned if successful. */
#define PANDA_BLOCK_CONFIG  _IOR('P', 1, struct panda_block)

/* The DMA engine must be armed before each experiment. */
#define PANDA_DMA_ARM       _IO('P', 2)

/* After the stream device has returned end of stream the completion code must
 * be read before restarting. */
#define PANDA_COMPLETION    _IOW('P', 3, uint32_t *)
/* One of the following completions can be expected: */
#define PANDA_COMPLETION_OK         0
#define PANDA_COMPLETION_DISARM     1
#define PANDA_COMPLETION_FRAMING    2
#define PANDA_COMPLETION_DMA        4
#define PANDA_COMPLETION_OVERRUN    8

/* Returns timestamp associated with the start of capture, created on rising
 * edge of capture enable.  To ensure a non-zero timestamp this should not be
 * called until data has been returned from the data stream. */
#define PANDA_GET_START_TS  _IOR('P', 4, struct timespec64)

/* Sends a block */
#define PANDA_BLOCK_SEND    _IOR('P', 5, struct panda_block_send_request)

/* Get the number of words scheduled */
#define PANDA_BLOCK_NWORDS  _IOW('P', 6, size_t *)
