/* This header file is designed to be shared with userspace. */

/* Returns size of register area as an unsigned 32-bit integer. */
#define PANDA_MAP_SIZE      _IO('P', 0)

/* Each open panda.block file must have its block size set by calling the
 * PANDA_BLOCK_CREATE ioctl to set the block order.  The physical address is
 * returned if successful. */
#define PANDA_BLOCK_CREATE  _IOW('P', 1, unsigned int)

/* This invalidates the cache and flushes the block to memory. */
#define PANDA_BLOCK_FLUSH   _IOW('P', 2, unsigned int)
