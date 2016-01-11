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


#define SERVER_NAME     "localhost"
#define SERVER_PORT     9999


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Named register support. */

/* For simulation we have dummy implementations for the named registers. */

void hw_set_block_base(unsigned int reg) { }

error__t hw_set_named_register(const char *name, unsigned int reg)
{
    return ERROR_OK;
}

error__t hw_validate(void)
{
    return ERROR_OK;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Support functions. */

static int sock = -1;

/* Need to make sure our communications are atomic. */
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()      ASSERT_PTHREAD(pthread_mutex_lock(&session_lock))
#define UNLOCK()    ASSERT_PTHREAD(pthread_mutex_unlock(&session_lock))


static error__t write_all(const void *data, size_t length)
{
    error__t error = ERROR_OK;
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
    error__t error = ERROR_OK;
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


void hw_read_bits(bool bits[BIT_BUS_COUNT], bool changes[BIT_BUS_COUNT])
{
    LOCK();
    handle_error(
        write_command('C', 0, 0, 0)  ?:
        read_all(bits, BIT_BUS_COUNT)  ?:
        read_all(changes, BIT_BUS_COUNT));
    UNLOCK();
}


void hw_read_positions(
    uint32_t positions[POS_BUS_COUNT], bool changes[POS_BUS_COUNT])
{
    LOCK();
    handle_error(
        write_command('P', 0, 0, 0)  ?:
        read_all(positions, sizeof(uint32_t) * POS_BUS_COUNT)  ?:
        read_all(changes, POS_BUS_COUNT));
    UNLOCK();
}


void hw_write_capture_masks(
    uint32_t bit_capture, uint32_t pos_capture,
    uint32_t framed_mask, uint32_t extended_mask)
{
    uint32_t masks[4] = {
        bit_capture, pos_capture, framed_mask, extended_mask };
    LOCK();
    handle_error(
        write_command('M', 0, 0, 0)  ?:
        write_all(masks, sizeof(masks)));
    UNLOCK();
}


/******************************************************************************/
/* Table API. */

/* The difference between short and long tables is managed here. */

struct hw_table {
    uint32_t **data;
    unsigned int count;
    unsigned int block_base;
};


static uint32_t **create_table_data(unsigned int count, size_t length)
{
    uint32_t **data = malloc(count * sizeof(uint32_t *));
    for (unsigned int i = 0; i < count; i ++)
        data[i] = malloc(length * sizeof(uint32_t));
    return data;
}


static struct hw_table *create_hw_table(
    unsigned int block_base, unsigned int count, size_t max_length)
{
    struct hw_table *table = malloc(sizeof(struct hw_table));
    *table = (struct hw_table) {
        .data = create_table_data(count, max_length),
        .count = count,
        .block_base = block_base,
    };
    return table;
}


error__t hw_open_short_table(
    unsigned int block_base, unsigned int block_count,
    unsigned int reset_reg, unsigned int fill_reg, unsigned int length_reg,
    size_t max_length, struct hw_table **table)
{
    *table = create_hw_table(block_base, block_count, max_length);
    return ERROR_OK;
}


error__t hw_open_long_table(
    unsigned int block_base, unsigned int block_count, unsigned int order,
    struct hw_table **table, size_t *length)
{
    *length = 1U << order;
    *table = create_hw_table(block_base, block_count, *length);
    return ERROR_OK;
}


const uint32_t *hw_read_table_data(struct hw_table *table, unsigned int number)
{
    return table->data[number];
}


void hw_write_table(
    struct hw_table *table, unsigned int number,
    size_t offset, const uint32_t data[], size_t length)
{
    size_t bytes = length * sizeof(uint32_t);
    LOCK();
    memcpy(table->data[number] + offset, data, bytes);
    handle_error(
        write_command_int(
            'T', table->block_base, number, 0, (uint32_t) (length + offset))  ?:
        write_all(table->data[number], (length + offset) * sizeof(uint32_t)));
    UNLOCK();
}


void hw_close_table(struct hw_table *table)
{
    for (unsigned int i = 0; i < table->count; i ++)
        free(table->data[i]);
    free(table->data);
    free(table);
}


/******************************************************************************/

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
