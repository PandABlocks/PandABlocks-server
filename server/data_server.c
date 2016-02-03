/* Socket server for data streaming interface. */

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>

#include "error.h"
#include "hardware.h"
#include "buffered_file.h"
#include "config_server.h"
#include "buffer.h"
#include "capture.h"
#include "locking.h"

#include "data_server.h"


/* Central circular data buffer. */
#define DATA_BLOCK_SIZE         (1U << 18)
#define DATA_BLOCK_COUNT        16

/* File buffers.  These are only used for text buffered text communication on
 * the data channel. */
#define IN_BUF_SIZE             4096
#define OUT_BUF_SIZE            4096
/* Proper network buffer.  All data communication uses this buffer size. */
#define NET_BUF_SIZE            16384


/* Connection and read block polling intervals.  These determine how long it
 * takes for a socket disconnect to be detected. */
#define CONNECTION_POLL_SECS    0
#define CONNECTION_POLL_NSECS   ((unsigned long) (0.1 * NSECS))  // 100 ms

#define READ_BLOCK_POLL_SECS    0
#define READ_BLOCK_POLL_NSECS   ((unsigned long) (0.1 * NSECS))  // 100 ms

/* Allow this many data blocks between the reader and the writer on startup. */
#define BUFFER_READ_MARGIN      2


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data retrieval from hardware. */

static pthread_mutex_t data_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t data_thread_event;

/* Thread id for shutdown synchronisation.  Alas, we need a separate thread to
 * record whether this id is valid. */
static pthread_t data_thread_id;
static bool data_thread_started = false;


/* The data capture thread drives the two core state machines associated with
 * data capture: hardware and the data capture buffer.
 *
 *  Hardware data capture is either Idle or Busy.  The transition to Busy is in
 *  response to *PCAP.ARM= while the transition back to Idle is notified to the
 *  capture thread.  Thus the data_capture_enabled flag effectively encapsulates
 *  the state of the hardware as far as software is concerned.
 *
 *  The data capture buffer is also either Idle or Busy, and this state is also
 *  directly controlled by the data capture thread, but in the Idle state we
 *  also have to check the number of connected busy clients: if a client is busy
 *  we cannot start a new capture.
 *
 * The data_capture_enabled flag is used to distinguish these two states.  The
 * data_thread_running flag is used to trigger an orderly shutdown of the thread
 * on server shutdown. */
static bool data_capture_enabled = false;
static volatile bool data_thread_running = true;

/* Data capture buffer. */
static struct buffer *data_buffer;
/* Structure used to define data capture in progress.  This is valid while data
 * capture is enabled, invalid otherwise. */
static struct data_capture *data_capture;


/* Performs a complete experiment capture: start data buffer, process the data
 * stream until hardware is complete, stop data buffer.. */
static void capture_experiment(void)
{
    start_write(data_buffer);

    bool at_eof = false;
    while (data_thread_running  &&  !at_eof)
    {
        void *block = get_write_block(data_buffer);
        size_t count;
        do
            count = hw_read_streamed_data(block, DATA_BLOCK_SIZE, &at_eof);
        while (data_thread_running  &&  count == 0  &&  !at_eof);
        if (count > 0)
            release_write_block(data_buffer, count);
    }

    end_write(data_buffer);
}


/* Data thread: the responsive half of the data capture state machine.  Captures
 * hardware data to internal buffer in response to triggered experiments. */
static void *data_thread(void *context)
{
    while (data_thread_running)
    {
        /* Wait for data capture to start. */
        LOCK(data_thread_mutex);
        while (data_thread_running  &&  !data_capture_enabled)
            WAIT(data_thread_mutex, data_thread_event);
        UNLOCK(data_thread_mutex);

        if (data_thread_running)
            capture_experiment();

        LOCK(data_thread_mutex);
        data_capture_enabled = false;
        UNLOCK(data_thread_mutex);
    }
    return NULL;
}


/* Forces the data capture thread to exit in an orderly way. */
static void stop_data_thread(void)
{
    LOCK(data_thread_mutex);
    data_thread_running = false;
    SIGNAL(data_thread_event);
    UNLOCK(data_thread_mutex);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* User interface and control. */


/* Note that if this function returns ERROR_OK then the mutex remains held. */
error__t lock_capture_disabled(void)
{
    LOCK(data_thread_mutex);
    error__t error = TEST_OK_(!data_capture_enabled,
        "Data capture in progress");
    if (error)
        UNLOCK(data_thread_mutex);
    return error;
}

void unlock_capture_disabled(void)
{
    UNLOCK(data_thread_mutex);
}


static void start_data_capture(void)
{
    data_capture = prepare_data_capture();
    hw_write_arm(true);

    data_capture_enabled = true;
    SIGNAL(data_thread_event);
}


error__t arm_capture(void)
{
    unsigned int readers, active;
    return WITH_LOCK(data_thread_mutex,
        TEST_OK_(!data_capture_enabled, "Data capture already in progress")  ?:
        /* If data capture is not enabled then we can safely expect the buffer
         * status to be idle. */
        TEST_OK(!read_buffer_status(data_buffer, &readers, &active))  ?:
        TEST_OK_(active == 0, "Data clients still taking data")  ?:
        DO(start_data_capture()));
}


error__t disarm_capture(void)
{
    hw_write_arm(false);
    return ERROR_OK;
}


error__t reset_capture(void)
{
    /* For reset we just make a best effort.  This may silently do nothing if
     * called at the wrong time.  Too bad. */
    hw_write_arm(false);
    reset_buffer(data_buffer);
    return ERROR_OK;
}


error__t capture_status(struct connection_result *result)
{
    unsigned int readers, active;
    bool status = read_buffer_status(data_buffer, &readers, &active);
    return write_one_result(
        result, "%s %u %u", status ? "Busy" : "Idle", readers, active);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data delivery to client. */


struct data_connection {
    int scon;
    struct buffered_file *file;
    struct reader_state *reader;
    struct data_options options;
};


/* Every data request must start with a newline terminated format request. */
static bool process_data_request(struct data_connection *connection)
{
    char line[MAX_LINE_LENGTH];
    if (read_line(connection->file, line, sizeof(line), false))
    {
        error__t error = parse_data_options(line, &connection->options);
        int length = error ?
            snprintf(line, sizeof(line), "ERR %s\n", error_format(error))
        :
            snprintf(line, sizeof(line), "OK\n");
        error_discard(error);
        write_string(connection->file, line, (size_t) length);
        return flush_out_buf(connection->file)  &&  !error;
    }
    else
        return false;
}


/* Checks the connection by attempting a peek on the socket.  Also silently
 * consume any extra data the client sends us at this point. */
static bool check_connection(struct data_connection *connection)
{
    char buffer[4096];
    ssize_t rx = recv(connection->scon, buffer, sizeof(buffer), MSG_DONTWAIT);
if (rx > 0)
printf("Discarding:\"%.*s\"\n", (int) rx, buffer);
    return (rx == -1  &&  errno == EAGAIN)  ||  rx > 0;
}


/* Block until capture begins or the socket is closed. */
static bool wait_for_capture(
    struct data_connection *connection, uint64_t *lost_bytes)
{
    /* Block here waiting for data capture to begin or for the client to
     * disconnect.  Alas, detecting disconnection is a bit of a pain: we either
     * have to poll or use file handles (eventfd(2)) for wakeup, and using a
     * file handle is a pain because we'd need one per client.  So we poll. */
    const struct timespec timeout = {
        .tv_sec  = CONNECTION_POLL_SECS,
        .tv_nsec = CONNECTION_POLL_NSECS, };
    bool opened = false;
    while (check_connection(connection)  &&  !opened)
        opened = open_reader(
            connection->reader, BUFFER_READ_MARGIN, &timeout, lost_bytes);
    return opened;
}


#if 0
/* This processes a single data buffer, returning false if there is a
 * communication problem with the client.
 *    Extra work is needed to deal with the fact that compute_output_data can
 * only work in whole lines, so we need to make sure that any fragments are
 * carried over and processed separately. */
static bool process_data_buffer(
    struct data_connection *connection,
    void *line_buffer, size_t line_length, size_t *line_buffer_count,
    const void *buffer, size_t in_length)
{
    /* Check for anything in the line buffer. */
    if (*line_buffer_count > 0)
    {
        size_t to_copy = MIN(in_length, line_length - *line_buffer_count);
        memcpy(line_buffer, buffer, to_copy);
        buffer += to_copy;
        in_length -= to_copy;
        *line_buffer_count += to_copy;
        if (*line_buffer_count >= line_length)
        {
            emit line buffer;
            *line_buffer_count = 0;
        }
    }

    while (in_length > 0)
    {
    }
}
#endif


/* Sends the data stream until end of stream or there's a problem with the
 * client connection.  Returns false if there's a client connection problem. */
static bool send_data_stream(struct data_connection *connection)
{
    const struct timespec timeout = {
        .tv_sec  = READ_BLOCK_POLL_SECS,
        .tv_nsec = READ_BLOCK_POLL_NSECS, };
    size_t in_length;
    const void *buffer;
    while (buffer = get_read_block(connection->reader, &timeout, &in_length),
           buffer  &&  check_connection(connection))
    {
        while (in_length > 0)
        {
            char out_buf[NET_BUF_SIZE];

            size_t consumed;
            size_t out_length = compute_output_data(
                data_capture, &connection->options,
                buffer, in_length, &consumed,
                out_buf, sizeof(out_buf));
            in_length -= consumed;
            buffer += consumed;

            if (check_read_block(connection->reader))
            {
                if (!write_block(connection->file, out_buf, out_length))
                    return false;
            }
            else
                /* Oops.  Buffer overrun.  Discard this block, the next
                 * get_read_block() call will fail immediately. */
                break;
        }
    }
    return true;
}


static bool send_data_completion(
    struct data_connection *connection, enum reader_status status)
{
    /* This list of completion strings must match the definition of the
     * reader_status enumeration in buffer.h. */
    static const char *completions[] = {
        "OK\n",
        "ERR Early disconnect\n",   // Hard to see how this gets to the user!
        "ERR Data overrun\n",
        "ERR Connection reset\n",
    };
    const char *message = completions[status];
    write_string(connection->file, message, strlen(message));
    return flush_out_buf(connection->file);
}


/* This is the top level handler for a single data client connection.  The
 * connection must open with a format request, after which we will send data
 * capture results while the socket is connected. */
error__t process_data_socket(int scon)
{
    struct data_connection connection = {
        .scon = scon,
        .file = create_buffered_file(scon, IN_BUF_SIZE, OUT_BUF_SIZE),
    };

    if (process_data_request(&connection))
    {
        connection.reader = create_reader(data_buffer);
        uint64_t lost_bytes;
        bool ok = true;
        while (ok  &&  wait_for_capture(&connection, &lost_bytes))
        {
            ok = send_data_header(
                data_capture, &connection.options, connection.file);
            if (ok)
                ok = send_data_stream(&connection);

            enum reader_status status = close_reader(connection.reader);
            if (ok)
                ok = send_data_completion(&connection, status);
        }
        destroy_reader(connection.reader);
    }

    return destroy_buffered_file(connection.file);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation and shutdown. */


error__t initialise_data_server(void)
{
    pwait_initialise(&data_thread_event);
    data_buffer = create_buffer(DATA_BLOCK_SIZE, DATA_BLOCK_COUNT);
    return ERROR_OK;
}


error__t start_data_server(void)
{
    return
        TEST_PTHREAD(pthread_create(
            &data_thread_id, NULL, data_thread, NULL))  ?:
        DO(data_thread_started = true);
}


void terminate_data_server_early(void)
{
    if (data_thread_started)
    {
        stop_data_thread();
        error_report(TEST_PTHREAD(pthread_join(data_thread_id, NULL)));
        shutdown_buffer(data_buffer);
    }
}


void terminate_data_server(void)
{
    /* Can't do this safely until all our clients have gone. */
    if (data_buffer)
        destroy_buffer(data_buffer);
}
