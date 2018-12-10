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



struct capture_buffer {
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
    enum buffer_state {
        STATE_IDLE,         // No data, no clients
        STATE_ACTIVE,       // Taking data
        STATE_CLEARING,     // Data capture complete, clients still taking data
    } state;
    unsigned int reader_count;  // Number of connected readers
    unsigned int active_count;  // Number of connected active readers

    size_t in_ptr;          // Index of next block to write
    uint64_t lost_bytes;    // Length of overwritten data so far.

    size_t written[];       // Bytes written into each block
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Buffer writer API. */

void start_write(struct capture_buffer *buffer)
{
    /* ASSERT: buffer->state == STATE_IDLE  &&  !buffer->active_count */

    LOCK(buffer->mutex);
    buffer->buffer_cycle = 0;
    buffer->active_count = buffer->reader_count;
    buffer->state = STATE_ACTIVE;
    buffer->in_ptr = 0;
    buffer->lost_bytes = 0;
    memset(buffer->written, 0, buffer->block_count * sizeof(size_t));
    BROADCAST(buffer->signal);
    UNLOCK(buffer->mutex);
}


static void *get_buffer(struct capture_buffer *buffer, size_t ix)
{
    return buffer->buffer + ix * buffer->block_size;
}


void *get_write_block(struct capture_buffer *buffer)
{
    /* ASSERT: buffer->state == STATE_ACTIVE */
    return get_buffer(buffer, buffer->in_ptr);
}


void release_write_block(struct capture_buffer *buffer, size_t written)
{
    /* ASSERT: buffer->state == STATE_ACTIVE  &&  written */

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


/* Go idle and step on to the next capture cycle.  Can only be called when there
 * are no active clients. */
static void advance_capture(struct capture_buffer *buffer)
{
    /* ASSERT: buffer->active_count == 0 */
    buffer->state = STATE_IDLE;
    buffer->capture_cycle += 1;
}


void end_write(struct capture_buffer *buffer)
{
    /* ASSERT: buffer->state == STATE_ACTIVE */
    LOCK(buffer->mutex);
    /* If there are active readers we need to go into the clearing state, and
     * let them know that we've read the end of the capture. */
    if (buffer->active_count > 0)
    {
        buffer->state = STATE_CLEARING;
        BROADCAST(buffer->signal);
    }
    else
        advance_capture(buffer);
    UNLOCK(buffer->mutex);
}


/* This is called when a reader completes a capture, either through normal
 * closing or by premature destruction. */
static void complete_capture(struct capture_buffer *buffer)
{
    /* ASSERT: buffer->state != STATE_IDLE */
    buffer->active_count -= 1;
    if (buffer->active_count == 0  &&  buffer->state == STATE_CLEARING)
        advance_capture(buffer);
}


/* Adds a new reader.  Because of the way we do connection and state management,
 * we need to treat this reader as active unless we're idle.  Returns the
 * current capture cycle so the reader can get started right away. */
static unsigned int add_reader(struct capture_buffer *buffer)
{
    LOCK(buffer->mutex);
    unsigned int cycle = buffer->capture_cycle;
    buffer->reader_count += 1;
    if (buffer->state != STATE_IDLE)
        buffer->active_count += 1;
    UNLOCK(buffer->mutex);
    return cycle;
}


/* Removes a reader.  Again, state management is a trifle involved.  We pass the
 * capture cycle of the disconnecting reader. */
static void remove_reader(struct capture_buffer *buffer, unsigned int cycle)
{
    LOCK(buffer->mutex);
    buffer->reader_count -= 1;
    /* If we are nominally in a capture cycle, but we haven't actually started
     * it, then we need to count ourself off.  That we haven't started is
     * evident because close_reader() would have advanced our capture cycle. */
    if (buffer->state != STATE_IDLE  &&  buffer->capture_cycle == cycle)
        complete_capture(buffer);
    UNLOCK(buffer->mutex);
}


void shutdown_buffer(struct capture_buffer *buffer)
{
    LOCK(buffer->mutex);
    buffer->shutdown = true;
    BROADCAST(buffer->signal);
    UNLOCK(buffer->mutex);
}


bool read_buffer_status(
    struct capture_buffer *buffer,
    unsigned int *readers, unsigned int *active_readers)
{
    LOCK(buffer->mutex);
    enum buffer_state state = buffer->state;
    *readers = buffer->reader_count;
    *active_readers = buffer->active_count;
    UNLOCK(buffer->mutex);
    return state != STATE_IDLE;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Reader API. */


struct reader_state {
    struct capture_buffer *buffer;      // Buffer we're reading from
    unsigned int capture_cycle;
    unsigned int buffer_cycle;
    size_t out_ptr;             // Index of our current block
    enum reader_status status;  // Return code
};


struct reader_state *create_reader(struct capture_buffer *buffer)
{
    struct reader_state *reader = malloc(sizeof(struct reader_state));
    *reader = (struct reader_state) {
        .buffer = buffer,
        .capture_cycle = add_reader(buffer),
    };
    return reader;
}


void destroy_reader(struct reader_state *reader)
{
    remove_reader(reader->buffer, reader->capture_cycle);
    free(reader);
}


/* The counting of lost bytes interacts closely with the updating of the written
 * and lost counts in release_write_block.  We include the in_ptr block in our
 * count here because it doesn't get added to .lost_bytes until the write is
 * complete. */
static uint64_t count_lost_bytes(struct capture_buffer *buffer, size_t out_ptr)
{
    uint64_t lost_bytes = buffer->lost_bytes;
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
    struct reader_state *reader, unsigned int read_margin, uint64_t *lost_bytes)
{
    struct capture_buffer *buffer = reader->buffer;
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
            reader->buffer_cycle = buffer->buffer_cycle;
            reader->out_ptr = out_ptr - buffer->block_count;
        }
        else
        {
            reader->buffer_cycle = buffer->buffer_cycle - 1;
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

    struct capture_buffer *buffer = reader->buffer;
    /* Wait until the buffer is ready and we have a fresh capture cycle to work
     * with or until the deadline expires. */
    while (true)
    {
        if (buffer->shutdown)
            /* Shutdown forced. */
            return false;
        if (buffer->state != STATE_IDLE  &&
            buffer->capture_cycle == reader->capture_cycle)
            /* New capture cycle ready for us. */
            return true;
        if (!pwait_deadline(&buffer->mutex, &buffer->signal, &deadline))
            /* Timeout detected. */
            return false;
    }
}


bool open_reader(
    struct reader_state *reader, unsigned int read_margin,
    const struct timespec *timeout, uint64_t *lost_bytes)
{
    struct capture_buffer *buffer = reader->buffer;
    LOCK(buffer->mutex);

    /* Wait for buffer to become active with a newer capture. */
    bool active = wait_for_buffer_ready(reader, timeout);
    if (active)
    {
        /* Start taking data. */
        reader->capture_cycle = buffer->capture_cycle;
        compute_reader_start(reader, read_margin, lost_bytes);
        reader->status = READER_STATUS_CLOSED;  // Default, not true yet!
    }

    UNLOCK(buffer->mutex);
    return active;
}


enum reader_status close_reader(struct reader_state *reader)
{
    struct capture_buffer *buffer = reader->buffer;
    LOCK(buffer->mutex);
    complete_capture(buffer);
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
    /* ASSERT: reader->status == READER_STATUS_CLOSED */

    struct capture_buffer *buffer = reader->buffer;
    LOCK(buffer->mutex);
    unsigned int buffer_cycle = buffer->buffer_cycle;
    size_t in_ptr = buffer->in_ptr;
    UNLOCK(buffer->mutex);

    bool ok = check_overrun_ok(
        buffer_cycle, reader_buffer_cycle, in_ptr, out_ptr);
    if (!ok)
        reader->status = READER_STATUS_OVERRUN;
    return ok;
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
    struct capture_buffer *buffer = reader->buffer;
    while (!buffer->shutdown)
    {
        /* ASSERT:
         *  buffer->capture_cycle == reader->capture_cycle  &&
         *  buffer->state != STATE_IDLE */

        bool waiting =
            buffer->buffer_cycle == reader->buffer_cycle  &&
            buffer->in_ptr == reader->out_ptr;
        if (!waiting)
            /* No longer waiting, things have moved on. */
            return;

        if (buffer->state == STATE_CLEARING)
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

    struct capture_buffer *buffer = reader->buffer;
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


struct capture_buffer *create_buffer(size_t block_size, size_t block_count)
{
    struct capture_buffer *buffer = malloc(
        sizeof(struct capture_buffer) + block_count * sizeof(size_t));
    *buffer = (struct capture_buffer) {
        .block_size = block_size,
        .block_count = block_count,
        .mutex = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER,
        .buffer = malloc(block_count * block_size),
    };
    pwait_initialise(&buffer->signal);
    return buffer;
}


void destroy_buffer(struct capture_buffer *buffer)
{
    free(buffer->buffer);
    free(buffer);
}
