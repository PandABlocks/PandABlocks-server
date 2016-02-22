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
};

/* Each open panda.block file must have its block size set by calling the
 * PANDA_BLOCK_CREATE ioctl to set the block order.  The physical address is
 * returned if successful. */
#define PANDA_BLOCK_CREATE  _IOR('P', 1, struct panda_block)

/* After the stream device has returned end of stream the completion code must
 * be read before restarting. */
#define PANDA_COMPLETION    _IOW('P', 2, uint32_t *)
