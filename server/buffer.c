#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>

#include "error.h"
#include "locking.h"

#include "buffer.h"



struct buffer {
    size_t block_size;      // Size of each block in bytes
    size_t block_count;     // Number of blocks in buffer

    pthread_mutex_t mutex;  // Locks all access
    pthread_cond_t signal;  // Used to trigger wakeup events

    void *buffer;           // Base of captured data buffer
    /* Cycle and capture counting are used to manage connections without having
     * to keep track of clients.  If the client and buffer capture_count don't
     * agree then the client has been reset, and the cycle_count is used to
     * check whether the client's buffer has been overwritten. */
    unsigned int capture_count; // Counts data capture cycles
    unsigned int cycle_count;   // Counts buffer cycles, for overrun detection

    bool active;            // True if data currently being captured
    unsigned int reader_count;  // Number of connected readers

    size_t in_ptr;          // Index of next block to write
    size_t lost_bytes;      // Length of overwritten data so far.

    size_t written[];       // Bytes written into each block
};


struct reader_state {
    struct buffer *buffer;  // Buffer we're reading from
    unsigned int capture_count;
    unsigned int cycle_count;
    size_t out_ptr;         // Index of our current block
    enum reader_status status;  // Return code
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Buffer writer API. */

void start_write(struct buffer *buffer)
{
    ASSERT_OK(!buffer->active);
    ASSERT_OK(!buffer->reader_count);

    LOCK(buffer->mutex);
    buffer->cycle_count = 0;
    buffer->active = true;
    buffer->in_ptr = 0;
    buffer->lost_bytes = 0;
    memset(buffer->written, 0, buffer->block_count * sizeof(size_t));
    UNLOCK(buffer->mutex);
}


static void *get_buffer(struct buffer *buffer, size_t ix)
{
    return buffer->buffer + ix * buffer->block_size;
}


void *get_write_block(struct buffer *buffer)
{
    ASSERT_OK(buffer->active);
    return get_buffer(buffer, buffer->in_ptr);
}


void release_write_block(struct buffer *buffer, size_t written)
{
    ASSERT_OK(buffer->active);
    ASSERT_OK(written);

    LOCK(buffer->mutex);
    /* Keep track of the total number of bytes in recycled blocks: we'll need
     * this so that late coming clients get to know how much data they've
     * missed. */
    buffer->lost_bytes += buffer->written[buffer->in_ptr];
    buffer->written[buffer->in_ptr] = written;

    /* Advance buffer and cycle count. */
    buffer->in_ptr += 1;
    if (buffer->in_ptr >= buffer->block_count)
    {
        buffer->in_ptr = 0;
        buffer->cycle_count += 1;
    }

    /* Let all clients know there's data to read. */
    BROADCAST(buffer->signal);
    UNLOCK(buffer->mutex);
}


void end_write(struct buffer *buffer)
{
    ASSERT_OK(buffer->active);

    LOCK(buffer->mutex);
    buffer->active = false;
    /* Only advance the capture count on a normal end when there are no more
     * readers. */
    if (buffer->reader_count == 0)
        buffer->capture_count += 1;
    else
        /* Let clients know there's something to deal with. */
        BROADCAST(buffer->signal);
    UNLOCK(buffer->mutex);
}


/* This is called when a reader disconnects, but ONLY if the capture_count
 * fields agree. */
static void disconnect_client(struct buffer *buffer)
{
    buffer->reader_count -= 1;
    if (buffer->reader_count == 0)
        buffer->capture_count += 1;
}


void reset_buffer(struct buffer *buffer)
{
    ASSERT_OK(!buffer->active);

    LOCK(buffer->mutex);
    if (buffer->reader_count > 0)
    {
        buffer->reader_count = 0;
        buffer->capture_count += 1;
    }
    UNLOCK(buffer->mutex);
}


bool read_buffer_status(struct buffer *buffer, unsigned int *clients)
{
    LOCK(buffer->mutex);
    bool active = buffer->active;
    *clients = buffer->reader_count;
    UNLOCK(buffer->mutex);
    return active;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Reader API. */


/* The counting of lost bytes interacts closely with the updating of the written
 * and lost counts in release_write_block.  We include the in_ptr block in our
 * count here because it doesn't get added to .lost_bytes until the write is
 * complete. */
static size_t count_lost_bytes(struct buffer *buffer, size_t out_ptr)
{
    size_t lost_bytes = buffer->lost_bytes;
    for (size_t ix = buffer->in_ptr; ix != out_ptr; )
    {
        lost_bytes += buffer->written[ix];
        ix += 1;
        if (ix >= buffer->block_count)
            ix = 0;
    }
    return lost_bytes;
}


/* Computes a sensible starting point for the reader and compute the number of
 * missed bytes. */
static void compute_reader_start(
    struct reader_state *reader, unsigned int read_margin, size_t *lost_bytes)
{
    struct buffer *buffer = reader->buffer;
    /* If the buffer is not too full then we don't have to think. */
    if (buffer->cycle_count == 0  &&
        buffer->in_ptr + read_margin + 1 < buffer->block_count)
    {
        reader->cycle_count = 0;
        reader->out_ptr = 0;
        *lost_bytes = 0;
    }
    else
    {
        /* Hum.  Not enough margin.  Compute out_ptr, associated cycle_count,
         * and lost bytes. */
        size_t out_ptr = buffer->in_ptr + read_margin + 1;
        if (out_ptr >= buffer->block_count)
        {
            reader->cycle_count = buffer->cycle_count - 1;
            reader->out_ptr = out_ptr - buffer->block_count;
        }
        else
        {
            reader->cycle_count = buffer->cycle_count;
            reader->out_ptr = out_ptr;
        }
        *lost_bytes = count_lost_bytes(buffer, reader->out_ptr);
    }
}


struct reader_state *open_reader(
    struct buffer *buffer, unsigned int read_margin, size_t *lost_bytes)
{
    struct reader_state *reader = malloc(sizeof(struct reader_state));
    LOCK(buffer->mutex);
    *reader = (struct reader_state) {
        .buffer = buffer,
        .capture_count = buffer->capture_count,
        /* This is the status that will be returned if we close the reader
         * prematurely. */
        .status = READER_STATUS_CLOSED,
    };
    compute_reader_start(reader, read_margin, lost_bytes);
    UNLOCK(buffer->mutex);

    return reader;
}


enum reader_status close_reader(struct reader_state *reader)
{
    struct buffer *buffer = reader->buffer;
    LOCK(buffer->mutex);
    /* If we can, count ourself off. */
    if (reader->capture_count == buffer->capture_count)
        disconnect_client(buffer);
    UNLOCK(buffer->mutex);

    enum reader_status status = reader->status;
    free(reader);
    return status;
}


/* Detect buffer underflow by inspecting the in and out pointers and checking
 * the buffer cycle count.  We can only be deceived if a full 2^32 cycles have
 * ocurred since the last time we looked, but the pacing of reading and writing
 * eliminates that risk. */
static bool check_overrun_ok(
    struct reader_state *reader,
    unsigned int cycle_count, size_t in_ptr, size_t out_ptr)
{
    if (in_ptr == out_ptr)
        /* Unmistakable collision! */
        return false;
    else if (in_ptr > out_ptr)
        /* Out pointer ahead of in pointer.  We're ok if we're both on the same
         * cycle. */
        return cycle_count == reader->cycle_count;
    else
        /* Out pointer behind in pointer.  In this case the buffer should be one
         * step ahead of us. */
        return cycle_count == reader->cycle_count + 1;
}


/* Checks status of indicated out_ptr block and updates the status result
 * accordingly if there's any failure. */
static bool check_block_status(struct reader_state *reader, size_t out_ptr)
{
    ASSERT_OK(reader->status == READER_STATUS_CLOSED);

    struct buffer *buffer = reader->buffer;
    LOCK(buffer->mutex);
    unsigned int capture_count = buffer->capture_count;
    unsigned int cycle_count = buffer->cycle_count;
    size_t in_ptr = buffer->in_ptr;
    UNLOCK(buffer->mutex);

    if (reader->capture_count != capture_count)
    {
        /* We're now in the wrong capture cycle, let's call this a reset. */
        reader->status = READER_STATUS_RESET;
        return false;
    }
    else if (!check_overrun_ok(reader, cycle_count, in_ptr, out_ptr))
    {
        reader->status = READER_STATUS_OVERRUN;
        return false;
    }
    else
        return true;
}


bool check_read_block(struct reader_state *reader)
{
    return check_block_status(reader, reader->out_ptr);
}


/* Returns true while we're waiting for input. */
static bool _pure waiting_for_input(struct reader_state *reader)
{
    struct buffer *buffer = reader->buffer;
    return
        buffer->capture_count == reader->capture_count  &&
        buffer->cycle_count == reader->cycle_count  &&
        buffer->in_ptr == reader->out_ptr;
}


const void *get_read_block(struct reader_state *reader, size_t *length)
{
    ASSERT_OK(reader->status == READER_STATUS_CLOSED);

    struct buffer *buffer = reader->buffer;
    LOCK(buffer->mutex);
    /* While the block we want to read is being written we're blocked. */
    while (buffer->active  &&  waiting_for_input(reader))
        WAIT(buffer->mutex, buffer->signal);
    bool all_read = !buffer->active  &&  waiting_for_input(reader);
    UNLOCK(buffer->mutex);

    if (all_read)
    {
        reader->status = READER_STATUS_ALL_READ;
        return NULL;
    }
    else
    {
        /* Advance to next block and return the current one, if we can. */
        size_t out_ptr = reader->out_ptr;
        reader->out_ptr += 1;
        if (reader->out_ptr >= buffer->block_count)
            reader->out_ptr = 0;

        /* Check the status of the block we're about to return. */
        if (check_block_status(reader, out_ptr))
            return get_buffer(buffer, out_ptr);
        else
            return NULL;
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


struct buffer *create_buffer(size_t block_size, size_t block_count)
{
    struct buffer *buffer = malloc(
        sizeof(struct buffer) + block_count * sizeof(size_t));
    *buffer = (struct buffer) {
        .block_size = block_size,
        .block_count = block_count,
        .mutex = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER,
        .signal = (pthread_cond_t) PTHREAD_COND_INITIALIZER,
        .buffer = malloc(block_count * block_size),
    };
    return buffer;
}


void destroy_buffer(struct buffer *buffer)
{
    free(buffer->buffer);
    free(buffer);
}
