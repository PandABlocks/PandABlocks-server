/* Shared circular buffer for single writer and multiple independent readers. */

/* The buffer. */
struct buffer;
/* A single reader connected to a buffer. */
struct reader_state;

/* Prepares central memory buffer. */
struct buffer *create_buffer(size_t block_size, size_t block_count);
/* Destroys memory buffer. */
void destroy_buffer(struct buffer *buffer);

/* Reserves the next slot in the buffer for writing. An entire contiguous
 * block of block_size bytes is guaranteed to be returned, and
 * release_write_block() must be called when writing is complete. */
void *get_write_block(struct buffer *buffer, size_t *block_size);
/* Releases the write block.  The number of bytes written is specified together
 * with the state of the data stream: at end of capture data_end is set to true.
 * It is valid to write zero bytes. */
void release_write_block(struct buffer *buffer, size_t written, bool data_end);

/* Creates a new reading connection to the buffer. */
struct reader_state *open_reader(struct buffer *buffer);
/* Closes a previously opened reader connection. */
void close_reader(struct reader_state *reader);

/* Blocks until an entire block_size block is available to be read out, returns
 * pointer to data to be read.  At the end of the data stream NULL is returned,
 * and release_write_block() should not be called before calling
 * get_read_block() again. */
const void *get_read_block(struct reader_state *reader, size_t *length);

size_t read_block_data(
    struct reader_state *reader, size_t length, bool *data_end);



/* Releases the write block.  If false is returned then the block was
 * overwritten while locked due to reader underrun.  Only call if non-NULL value
 * returned by get_read_block(). */
bool release_read_block(struct reader_state *reader);
