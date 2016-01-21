/* Simulation hardware interface. */

#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <pthread.h>

#include "error.h"
#include "socket_server.h"
#include "hardware.h"

#include "sim_hardware.h"


#define SERVER_NAME     "localhost"
#define SERVER_PORT     9999


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Support functions. */

static int sock = -1;
static bool socket_ok = true;

/* Need to make sure our communications are atomic. */
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()      ASSERT_PTHREAD(pthread_mutex_lock(&session_lock))
#define UNLOCK()    ASSERT_PTHREAD(pthread_mutex_unlock(&session_lock))


static error__t write_all(const void *data, size_t length)
{
    error__t error = TEST_OK_(socket_ok, "Simulation connection failed");
    ssize_t written;
    while (!error  &&  length > 0)
        error =
            TEST_IO(written = write(sock, data, length))  ?:
            DO(
                data   += (size_t) written;
                length -= (size_t) written;
            );
    return error;
}


static error__t read_all(void *data, size_t length)
{
    error__t error = TEST_OK_(socket_ok, "Simulation connection failed");
    ssize_t received;
    while (!error  &&  length > 0)
        error =
            TEST_IO(received = read(sock, data, length))  ?:
            TEST_OK_(received, "Unexpected EOF")  ?:
            DO(
                data   += (size_t) received;
                length -= (size_t) received;
            );
    return error;
}


static error__t write_command(
    char command,
    unsigned int block_base, unsigned int block_number, unsigned int reg)
{
    unsigned char string[4] = {
        (unsigned char) command,
        (unsigned char) block_base,
        (unsigned char) block_number,
        (unsigned char) reg
    };
    return write_all(string, sizeof(string));
}


static error__t write_command_int(
    char command,
    unsigned int block_base, unsigned int block_number, unsigned int reg,
    uint32_t arg)
{
    unsigned char string[8] = {
        (unsigned char) command,
        (unsigned char) block_base,
        (unsigned char) block_number,
        (unsigned char) reg
    };
    *CAST_FROM_TO(unsigned char *, uint32_t *, &string[4]) = (uint32_t) arg;
    return write_all(string, sizeof(string));
}


static void handle_error(error__t error)
{
    /* This flag will be reset on the first error, which will cause all
     * subsequent access attempts to silently fail. */
    static bool running = true;
    if (error)
    {
        socket_ok = false;
        if (running)
        {
            ERROR_REPORT(error, "Error in simulation connection");
            kill_socket_server();
            running = false;    // Suppress subsequent error messages
        }
        else
            /* Quietly discard subsequent error messages. */
            error_discard(error);
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Hardware simulation methods. */


void hw_write_register(
    unsigned int block_base, unsigned int block_number, unsigned int reg,
    uint32_t value)
{
    LOCK();
    handle_error(
        write_command_int('W', block_base, block_number, reg, value));
    UNLOCK();
}


uint32_t hw_read_register(
    unsigned int block_base, unsigned int block_number, unsigned int reg)
{
    LOCK();
    uint32_t result = 0;
    handle_error(
        write_command('R', block_base, block_number, reg)  ?:
        read_all(&result, 4));
    UNLOCK();

    return result;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Long table support. */


error__t hw_long_table_allocate(
    unsigned int order,
    uint32_t **data, uint32_t *physical_addr, void **table_id)
{
    size_t length = 1U << order;
    *data = malloc(length * sizeof(uint32_t));
    *physical_addr = 0;
    *table_id = *data;
    return ERROR_OK;
}


void hw_long_table_release(void *table_id)
{
    free(table_id);
}


void hw_long_table_flush(
    void *table_id, size_t length,
    unsigned int block_base, unsigned int number)
{
    LOCK();
    handle_error(
        write_command_int('T', block_base, number, 0, (uint32_t) length)  ?:
        write_all(table_id, length * sizeof(uint32_t)));
    UNLOCK();
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation. */

error__t initialise_hardware(void)
{
    struct sockaddr_in s_in = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(SERVER_PORT)
    };
    struct hostent *hostent;
    int one = 1;
    return
        TEST_OK_IO(hostent = gethostbyname(SERVER_NAME))  ?:
        DO(memcpy(
            &s_in.sin_addr.s_addr, hostent->h_addr,
            (size_t) hostent->h_length))  ?:
        TEST_IO(sock = socket(AF_INET, SOCK_STREAM, 0))  ?:
        TEST_IO_(connect(sock, (struct sockaddr *) &s_in, sizeof(s_in)),
            "Unable to connect to simulation server")  ?:
        set_timeout(sock, SO_SNDTIMEO, 1)  ?:
        set_timeout(sock, SO_RCVTIMEO, 1)  ?:
        TEST_IO(setsockopt(sock, SOL_TCP, TCP_NODELAY, &one, sizeof(one)));
}


void terminate_hardware(void)
{
    if (sock >= 0)
        close(sock);
}
