/* Socket server for data streaming interface. */

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "error.h"
#include "hardware.h"

#include "data_server.h"


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data retrieval from hardware. */

static pthread_t data_thread_id;
static bool data_thread_started = false;
static volatile bool data_thread_running = true;


static void *data_thread(void *context)
{
    char buffer[4096];
    bool at_eof;
    size_t count;
    while (data_thread_running)
        count = hw_read_streamed_data(buffer, sizeof(buffer), &at_eof);
    return NULL;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data delivery to client. */


error__t process_data_socket(int scon)
{
    const char *message = "Data connection not yet implemented\n";
    return TEST_IO(write(scon, message, strlen(message)));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation and shutdown. */

error__t start_data_server(void)
{
    return
        TEST_PTHREAD(pthread_create(
            &data_thread_id, NULL, data_thread, NULL))  ?:
        DO(data_thread_started = true);
}

void terminate_data_server(void)
{
    data_thread_running = false;
    if (data_thread_started)
        error_report(
            TEST_PTHREAD(pthread_cancel(data_thread_id))  ?:
            TEST_PTHREAD(pthread_join(data_thread_id, NULL)));
}
