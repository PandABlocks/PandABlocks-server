/* Shared circular buffer for single writer and multiple independent readers. */

/* The buffer. */
struct capture_buffer;


/* Prepares central memory buffer. */
struct capture_buffer *create_buffer(size_t block_size, size_t block_count);

/* Destroys memory buffer. */
void destroy_buffer(struct capture_buffer *buffer);

/* Forces buffer into shutdown mode: all readers will fail immediately. */
void shutdown_buffer(struct capture_buffer *buffer);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Writing to the buffer. */

/* Initiates a write cycle. */
void start_write(struct capture_buffer *buffer);

/* Completes a write cycle. */
void end_write(struct capture_buffer *buffer);

/* Reserves the next slot in the buffer for writing. An entire contiguous
 * block of block_size bytes is guaranteed to be returned, and
 * release_write_block() must be called when writing is complete. */
void *get_write_block(struct capture_buffer *buffer);

/* Releases the write block, specifies number of bytes written. */
void release_write_block(struct capture_buffer *buffer, size_t written);


/* Returns true if buffer is taking data, and count of active clients. */
bool read_buffer_status(
    struct capture_buffer *buffer,
    unsigned int *readers, unsigned int *active_readers);

/* Resets the buffer to empty.  Any active readers will fail. */
void reset_buffer(struct capture_buffer *buffer);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Reading from the buffer */

/* A single reader connected to a buffer. */
struct reader_state;

/* Returned status of reader when reader closed. */
enum reader_status {
    READER_STATUS_ALL_READ, // Valid buffer data
    READER_STATUS_CLOSED,   // Close called early while data still available
    READER_STATUS_OVERRUN,  // Input data overrun
    READER_STATUS_RESET,    // Buffer forcibly reset
};

/* Creates a reader connected to the buffer. */
struct reader_state *create_reader(struct capture_buffer *buffer);

/* Releases resources used by a reader. */
void destroy_reader(struct reader_state *reader);

/* Blocks until the buffer is ready for a new read session or times out,
 * returning false on timeout.  If the connection is too late to receive all
 * data then the number of missed bytes is returned. */
bool open_reader(
    struct reader_state *reader, unsigned int read_margin,
    const struct timespec *timeout, uint64_t *lost_bytes);

/* Closes a previously opened reader connection, returns status.  The reader can
 * now be recycled by calling open_reader() again. */
enum reader_status close_reader(struct reader_state *reader);


/* Blocks until an entire block_size block is available to be read out, returns
 * pointer to data to be read.  Call this repeatedly to advance through the
 * buffer, returns NULL once no more data available. */
const void *get_read_block(
    struct reader_state *reader,
    const struct timespec *timeout, size_t *length);

/* Returns true if the current read block remains valid, returns false if the
 * buffer has been reset or if the current read block has been overwritten.
 * This MUST be called after consuming the contents of the block returned by
 * get_read_block. */
bool check_read_block(struct reader_state *reader);
