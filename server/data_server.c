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


#define DATA_BLOCK_SIZE     (1U << 18)
#define DATA_BLOCK_COUNT    16

#define IN_BUF_SIZE         4096
#define OUT_BUF_SIZE        4096

/* Connection poll interval. */
#define CONNECTION_POLL_SECS    0
#define CONNECTION_POLL_NSECS   ((unsigned long) (0.1 * NSECS))  // 100 ms


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data retrieval from hardware. */

static pthread_mutex_t data_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t data_thread_event;

static pthread_t data_thread_id;
static bool data_thread_started = false;
static volatile bool data_thread_running = true;

/* Set during data capture, reset when capture complete. */
static bool data_capture_enabled = false;

static struct buffer *data_buffer;


/* Number of currently connected data clients. */
static unsigned int connected_client_count;
/* Number of data clients current taking data. */
static unsigned int active_client_count;


static void capture_experiment(void)
{
    /* Wait for data capture to start. */
    LOCK(data_thread_mutex);
    while (data_thread_running  &&  !data_capture_enabled)
        WAIT(data_thread_mutex, data_thread_event);
    UNLOCK(data_thread_mutex);

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
printf("data_thread: %d %zu %d\n", data_thread_running, count, at_eof);
    }

    end_write(data_buffer);
}


static void *data_thread(void *context)
{
    while (data_thread_running)
    {
        capture_experiment();

        LOCK(data_thread_mutex);
        data_capture_enabled = false;
        if (data_thread_running)
        {
            data_capture_complete();
            if (active_client_count == 0)
                data_clients_complete();
        }
        UNLOCK(data_thread_mutex);
    }
    return NULL;
}


void start_data_capture(void)
{
printf("start_data_capture\n");
    LOCK(data_thread_mutex);
    data_capture_enabled = true;
    BROADCAST(data_thread_event);
    UNLOCK(data_thread_mutex);
}


void reset_data_capture(void)
{
printf("reset_data_capture\n");
    data_clients_complete();
}


static void stop_data_thread(void)
{
    LOCK(data_thread_mutex);
    data_thread_running = false;
    BROADCAST(data_thread_event);
    UNLOCK(data_thread_mutex);
}


void get_data_capture_counts(unsigned int *connected, unsigned int *reading)
{
    /* We lock this so that we get a consistent pair of values. */
    LOCK(data_thread_mutex);
    *connected = connected_client_count;
    *reading = active_client_count;
    UNLOCK(data_thread_mutex);
}


/* Keeps count of the connected clients. */
static void count_connection(bool connected)
{
    LOCK(data_thread_mutex);
    if (connected)
        connected_client_count += 1;
    else
        connected_client_count -= 1;
    UNLOCK(data_thread_mutex);
}


/* When a client completes its data processing we can mark it as inactive.  In
 * this case we may need to let the capture layer know about this as well. */
static void count_active_client(bool active)
{
    LOCK(data_thread_mutex);
    if (active)
        active_client_count += 1;
    else
    {
        active_client_count -= 1;
        if (active_client_count == 0)
            data_clients_complete();
    }
    UNLOCK(data_thread_mutex);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data delivery to client. */


struct data_connection {
    int scon;
    struct buffered_file *file;
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
printf("rx = %zd, %d\n", rx, errno);
    return (rx == -1  &&  errno == EAGAIN)  ||  rx > 0;
}


/* Block until capture begins or the socket is closed. */
static bool wait_for_capture(struct data_connection *connection)
{
    /* Block here waiting for data capture to begin or for the client to
     * disconnect.  Alas, detecting disconnection is a bit of a pain: we either
     * have to poll or use file handles (eventfd(2)) for wakeup, and using a
     * file handle is a pain because we'd need one per client.  So we poll. */
    LOCK(data_thread_mutex);
    while (check_connection(connection)  &&  !data_capture_enabled)
        pwait_timeout(
            &data_thread_mutex, &data_thread_event,
            CONNECTION_POLL_SECS, CONNECTION_POLL_NSECS);
    UNLOCK(data_thread_mutex);

    return true;
}


static void send_data_header(struct data_connection *connection)
{
    write_string(connection->file, "header\n", 7);
    flush_out_buf(connection->file);
}


static void send_data_stream(struct data_connection *connection)
{
    /* Wrong test, really needs to consume buffer. */
    while (data_capture_enabled)
    {
        write_string(connection->file, "data\n", 5);
        flush_out_buf(connection->file);
        sleep(1);
    }

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

    count_connection(true);
    if (process_data_request(&connection))
    {
        while (wait_for_capture(&connection))
        {
            count_active_client(true);
            send_data_header(&connection);
            send_data_stream(&connection);
            count_active_client(false);
        }
    }
    count_connection(false);

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


void terminate_data_server(void)
{
    stop_data_thread();
    if (data_thread_started)
        error_report(
            TEST_PTHREAD(pthread_join(data_thread_id, NULL)));
    if (data_buffer)
        destroy_buffer(data_buffer);
}
