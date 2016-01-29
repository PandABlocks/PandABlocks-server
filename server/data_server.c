/* Socket server for data streaming interface. */

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
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


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data retrieval from hardware. */

static pthread_mutex_t data_thread_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t data_thread_event = PTHREAD_COND_INITIALIZER;

static pthread_t data_thread_id;
static bool data_thread_started = false;
static volatile bool data_thread_running = true;

static bool data_capture_enabled = false;

static struct buffer *data_buffer;


static void *data_thread(void *context)
{
    while (data_thread_running)
    {
        /* Wait for data capture to start. */
        LOCK(data_thread_mutex);
        while (data_thread_running  &&  !data_capture_enabled)
            WAIT(data_thread_mutex, data_thread_event);
        UNLOCK(data_thread_mutex);

        bool at_eof = false;
        while (data_thread_running  &&  !at_eof)
        {
            size_t block_size;
            void *block = get_write_block(data_buffer, &block_size);
            size_t count = hw_read_streamed_data(block, block_size, &at_eof);
            release_write_block(data_buffer, count, at_eof);
        }

        LOCK(data_thread_mutex);
        data_capture_enabled = false;
        if (data_thread_running)
        {
            data_capture_complete();
            data_clients_complete();        // lies!!!
        }
        UNLOCK(data_thread_mutex);
    }
    return NULL;
}


void start_data_capture(void)
{
    LOCK(data_thread_mutex);
    data_capture_enabled = true;
    SIGNAL(data_thread_event);
    UNLOCK(data_thread_mutex);
}


void reset_data_capture(void)
{
    data_clients_complete();
}


static void stop_data_thread(void)
{
    LOCK(data_thread_mutex);
    data_thread_running = false;
    SIGNAL(data_thread_event);
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
    return FAIL_("Not implemented");
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
        return !error;
    }
    else
        return false;
}


static bool check_connection(struct data_connection *connection)
{
    return false;
}


static void wait_for_capture(struct data_connection *connection)
{
}


static void send_data_header(struct data_connection *connection)
{
}


static void send_data_stream(struct data_connection *connection)
{
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
        while (check_connection(&connection))
        {
            wait_for_capture(&connection);
            send_data_header(&connection);
            send_data_stream(&connection);
        }
    }

    return destroy_buffered_file(connection.file);
}


void get_data_capture_counts(unsigned int *reading, unsigned int *waiting)
{
    *reading = 0;
    *waiting = 0;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation and shutdown. */


error__t initialise_data_server(void)
{
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
