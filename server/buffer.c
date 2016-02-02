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
    /* Capture and buffer cycle counting are used to manage connections without
     * having to keep track of clients.  If the client and buffer capture_cycle
     * don't agree then the client has been reset, and the buffer_cycle is used
     * to check whether the client's buffer has been overwritten. */
    unsigned int capture_cycle; // Counts data capture cycles
    unsigned int buffer_cycle;  // Counts buffer cycles, for overrun detection

    bool shutdown;          // Set to true to force shutdown
    bool active;            // True if data currently being captured
    unsigned int reader_count;  // Number of connected readers
    unsigned int active_count;  // Number of connected active readers

    size_t in_ptr;          // Index of next block to write
    size_t lost_bytes;      // Length of overwritten data so far.

    size_t written[];       // Bytes written into each block
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Buffer writer API. */

void start_write(struct buffer *buffer)
{
    ASSERT_OK(!buffer->active);
    ASSERT_OK(!buffer->active_count);

    LOCK(buffer->mutex);
    buffer->buffer_cycle = 0;
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
        buffer->buffer_cycle += 1;
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
    if (buffer->active_count == 0)
        buffer->capture_cycle += 1;
    else
        /* Let clients know there's something to deal with. */
        BROADCAST(buffer->signal);
    UNLOCK(buffer->mutex);
}


/* This is called when a reader disconnects, but ONLY if the capture_cycle
 * fields agree. */
static void disconnect_client(struct buffer *buffer)
{
    buffer->active_count -= 1;
    if (!buffer->active  &&  buffer->active_count == 0)
        buffer->capture_cycle += 1;
}


void reset_buffer(struct buffer *buffer)
{
    LOCK(buffer->mutex);
    if (!buffer->active)
    {
        if (buffer->active_count > 0)
        {
            buffer->active_count = 0;
            buffer->capture_cycle += 1;
        }
    }
    BROADCAST(buffer->signal);
    UNLOCK(buffer->mutex);
}


void shutdown_buffer(struct buffer *buffer)
{
    LOCK(buffer->mutex);
    buffer->shutdown = true;
    BROADCAST(buffer->signal);
    UNLOCK(buffer->mutex);
}


bool read_buffer_status(
    struct buffer *buffer, unsigned int *readers, unsigned int *active_readers)
{
    LOCK(buffer->mutex);
    bool active = buffer->active;
    *readers = buffer->reader_count;
    *active_readers = buffer->active_count;
    UNLOCK(buffer->mutex);
    return active;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Reader API. */


struct reader_state {
    struct buffer *buffer;      // Buffer we're reading from
    unsigned int capture_cycle; // Checks whether we've been reset
    unsigned int buffer_cycle;
    size_t out_ptr;             // Index of our current block
    enum reader_status status;  // Return code
};


struct reader_state *create_reader(struct buffer *buffer)
{
    struct reader_state *reader = malloc(sizeof(struct reader_state));
    LOCK(buffer->mutex);
    *reader = (struct reader_state) {
        .buffer = buffer,
        .capture_cycle = buffer->capture_cycle,
    };
    buffer->reader_count += 1;
    UNLOCK(buffer->mutex);
    return reader;
}


void destroy_reader(struct reader_state *reader)
{
    struct buffer *buffer = reader->buffer;
    LOCK(buffer->mutex);
    buffer->reader_count -= 1;
    UNLOCK(buffer->mutex);
    free(reader);
}


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
    if (buffer->buffer_cycle == 0  &&
        buffer->in_ptr + read_margin + 1 < buffer->block_count)
    {
        reader->buffer_cycle = 0;
        reader->out_ptr = 0;
        *lost_bytes = 0;
    }
    else
    {
        /* Hum.  Not enough margin.  Compute out_ptr, associated buffer_cycle,
         * and lost bytes. */
        size_t out_ptr = buffer->in_ptr + read_margin + 1;
        if (out_ptr >= buffer->block_count)
        {
            reader->buffer_cycle = buffer->buffer_cycle - 1;
            reader->out_ptr = out_ptr - buffer->block_count;
        }
        else
        {
            reader->buffer_cycle = buffer->buffer_cycle;
            reader->out_ptr = out_ptr;
        }
        *lost_bytes = count_lost_bytes(buffer, reader->out_ptr);
    }
}


/* Wait a new capture cycle to begin or for the timeout to expire. */
static bool wait_for_buffer_ready(
    struct reader_state *reader, const struct timespec *timeout)
{
    struct timespec deadline;
    compute_deadline(timeout, &deadline);

    struct buffer *buffer = reader->buffer;
    /* Wait until the buffer is ready and we have a fresh capture cycle to work
     * with or until the deadline expires. */
    while (true)
    {
        if (buffer->shutdown)
            /* Shutdown forced. */
            return false;
        if (buffer->active  &&  buffer->capture_cycle >= reader->capture_cycle)
            /* New capture cycle ready for us. */
            return true;
        if (!pwait_deadline(&buffer->mutex, &buffer->signal, &deadline))
            /* Timeout detected. */
            return false;
    }
}


bool open_reader(
    struct reader_state *reader, unsigned int read_margin,
    const struct timespec *timeout, size_t *lost_bytes)
{
    struct buffer *buffer = reader->buffer;
    LOCK(buffer->mutex);

    /* Wait for buffer to become active with a newer capture. */
    bool active = wait_for_buffer_ready(reader, timeout);
    if (active)
    {
        /* Add ourself to the client count and prepare capture.. */
        buffer->active_count += 1;
        reader->capture_cycle = buffer->capture_cycle;
        compute_reader_start(reader, read_margin, lost_bytes);
        reader->status = READER_STATUS_CLOSED;
    }
    UNLOCK(buffer->mutex);

    return active;
}


enum reader_status close_reader(struct reader_state *reader)
{
    struct buffer *buffer = reader->buffer;
    LOCK(buffer->mutex);
    /* If we can, count ourself off. */
    if (reader->capture_cycle == buffer->capture_cycle)
        disconnect_client(buffer);
    UNLOCK(buffer->mutex);

    reader->capture_cycle += 1;
    return reader->status;
}


/* Detect buffer underflow by inspecting the in and out pointers and checking
 * the buffer cycle count.  We can only be deceived if a full 2^32 cycles have
 * ocurred since the last time we looked, but the pacing of reading and writing
 * eliminates that risk. */
static bool check_overrun_ok(
    unsigned int buffer_cycle, unsigned int reader_buffer_cycle,
    size_t in_ptr, size_t out_ptr)
{
    if (in_ptr == out_ptr)
        /* Unmistakable collision! */
        return false;
    else if (in_ptr > out_ptr)
        /* Out pointer ahead of in pointer.  We're ok if we're both on the same
         * cycle. */
        return buffer_cycle == reader_buffer_cycle;
    else
        /* Out pointer behind in pointer.  In this case the buffer should be one
         * step ahead of us. */
        return buffer_cycle == reader_buffer_cycle + 1;
}


/* Checks status of indicated out_ptr block and updates the status result
 * accordingly if there's any failure. */
static bool check_block_status(
    struct reader_state *reader,
    unsigned int reader_buffer_cycle, size_t out_ptr)
{
    ASSERT_OK(reader->status == READER_STATUS_CLOSED);

    struct buffer *buffer = reader->buffer;
    LOCK(buffer->mutex);
    unsigned int capture_cycle = buffer->capture_cycle;
    unsigned int buffer_cycle = buffer->buffer_cycle;
    size_t in_ptr = buffer->in_ptr;
    UNLOCK(buffer->mutex);

    if (reader->capture_cycle != capture_cycle)
    {
        /* We're now in the wrong capture cycle, let's call this a reset. */
        reader->status = READER_STATUS_RESET;
        return false;
    }
    else if (!check_overrun_ok(
        buffer_cycle, reader_buffer_cycle, in_ptr, out_ptr))
    {
        reader->status = READER_STATUS_OVERRUN;
        return false;
    }
    else
        return true;
}


bool check_read_block(struct reader_state *reader)
{
    /* Because out_ptr is the *next* block we're going to read, we need to
     * compute the out_ptr and buffer_cycle of the current block. */
    size_t out_ptr = reader->out_ptr;
    unsigned int buffer_cycle = reader->buffer_cycle;
    if (out_ptr > 0)
        out_ptr -= 1;
    else
    {
        out_ptr = reader->buffer->block_count - 1;
        buffer_cycle -= 1;
    }
    return check_block_status(reader, buffer_cycle, out_ptr);
}


static void wait_for_block_ready(
    struct reader_state *reader, const struct timespec *timeout,
    bool *all_read, bool *timeout_occurred)
{
    struct timespec deadline;
    compute_deadline(timeout, &deadline);

    *all_read = false;
    *timeout_occurred = false;
    struct buffer *buffer = reader->buffer;
    while (!buffer->shutdown)
    {
        bool waiting =
            buffer->capture_cycle == reader->capture_cycle  &&
            buffer->buffer_cycle == reader->buffer_cycle  &&
            buffer->in_ptr == reader->out_ptr;
        if (!waiting)
            /* No longer waiting, things have moved on. */
            return;

        if (!buffer->active)
        {
            /* Still in waiting condition but no longer active.  This is the
             * data completion state. */
            *all_read = true;
            return;
        }

        if (!pwait_deadline(&buffer->mutex, &buffer->signal, &deadline))
        {
            /* Timeout detected. */
            *timeout_occurred = true;
            return;
        }
    }
}


const void *get_read_block(
    struct reader_state *reader,
    const struct timespec *timeout, size_t *length)
{
    /* If the status is not in the default state (confusingly, this is the state
     * which will be returned if we close prematurely) then return nothing. */
    if (reader->status != READER_STATUS_CLOSED)
        return NULL;

    struct buffer *buffer = reader->buffer;
    LOCK(buffer->mutex);
    bool all_read, timeout_occurred;
    wait_for_block_ready(reader, timeout, &all_read, &timeout_occurred);
    UNLOCK(buffer->mutex);

    if (timeout_occurred)
    {
        *length = 0;
        return "";      // Dummy non NULL buffer, won't be read
    }
    else if (all_read)
    {
        reader->status = READER_STATUS_ALL_READ;
        return NULL;
    }
    else
    {
        /* Advance to next block and return the current one, if we can. */
        size_t out_ptr = reader->out_ptr;
        unsigned int buffer_cycle = reader->buffer_cycle;
        reader->out_ptr += 1;
        if (reader->out_ptr >= buffer->block_count)
        {
            reader->out_ptr = 0;
            reader->buffer_cycle += 1;
        }

        /* Check the status of the block we're about to return. */
        if (check_block_status(reader, buffer_cycle, out_ptr))
        {
            *length = buffer->written[out_ptr];
            return get_buffer(buffer, out_ptr);
        }
        else
            return NULL;
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Buffer creation and destruction. */


struct buffer *create_buffer(size_t block_size, size_t block_count)
{
    struct buffer *buffer = malloc(
        sizeof(struct buffer) + block_count * sizeof(size_t));
    *buffer = (struct buffer) {
        .block_size = block_size,
        .block_count = block_count,
        .mutex = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER,
        .buffer = malloc(block_count * block_size),
    };
    pwait_initialise(&buffer->signal);
    return buffer;
}


void destroy_buffer(struct buffer *buffer)
{
    free(buffer->buffer);
    free(buffer);
}
