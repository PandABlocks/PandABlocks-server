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


error__t arm_capture(void)
{
    LOCK(data_thread_mutex);
    unsigned int readers, active;
    error__t error =
        TEST_OK_(!data_capture_enabled, "Data capture already in progress")  ?:
        /* If data capture is not enabled then we can safely expect the buffer
         * status to be idle. */
        TEST_OK(!read_buffer_status(data_buffer, &readers, &active))  ?:
        TEST_OK_(active == 0, "Data clients still taking data");
    if (!error)
    {
        prepare_data_capture();
        hw_write_arm(true);

        data_capture_enabled = true;
        SIGNAL(data_thread_event);
    }
    UNLOCK(data_thread_mutex);
    return error;
}


error__t disarm_capture(void)
{
    hw_write_arm(false);
    return ERROR_OK;
}


error__t reset_capture(void)
{
    LOCK(data_thread_mutex);
    error__t error = FAIL_("Not implemented");
    UNLOCK(data_thread_mutex);
    return error;
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
};


static error__t parse_data_request(
    struct data_connection *connection, const char *line)
{
    printf("parsing line: \"%s\"\n", line);
    return ERROR_OK;
}


/* Every data request must start with a newline terminated format request. */
static bool process_data_request(struct data_connection *connection)
{
    char line[MAX_LINE_LENGTH];
    if (read_line(connection->file, line, sizeof(line), false))
    {
        error__t error = parse_data_request(connection, line);
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
    return (rx == -1  &&  errno == EAGAIN)  ||  rx > 0;
}


/* Block until capture begins or the socket is closed. */
static bool wait_for_capture(
    struct data_connection *connection, size_t *lost_bytes)
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


static void send_data_header(struct data_connection *connection)
{
    write_string(connection->file, "header\n", 7);
    flush_out_buf(connection->file);
}


static size_t fill_out_buf(
    const void *in_buf, size_t in_length, size_t *consumed, char *out_buf)
{
    const uint32_t *buf = in_buf;
    /* Lazy, about to redo.  For the moment the out buf is long enough. */
    *consumed = in_length;
    return (size_t) sprintf(out_buf, "%u..%u(%zu)\n",
        buf[0], buf[in_length/4 - 1], in_length);
}


static void send_data_stream(struct data_connection *connection)
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
            size_t out_length =
                fill_out_buf(buffer, in_length, &consumed, out_buf);
            in_length -= consumed;
            buffer += consumed;

            if (check_read_block(connection->reader))
                write_block(connection->file, out_buf, out_length);
        }
    }
}


static void send_data_completion(
    struct data_connection *connection, enum reader_status status)
{
    static const char *completions[] = {
        "OK\n", "Closed\n", "Overrun\n", "Reset\n", };
    const char *message = completions[status];
    write_string(connection->file, message, strlen(message));
    flush_out_buf(connection->file);
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
        size_t lost_bytes;
        while (wait_for_capture(&connection, &lost_bytes))
        {
            send_data_header(&connection);
            send_data_stream(&connection);
            enum reader_status status = close_reader(connection.reader);
            send_data_completion(&connection, status);
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
