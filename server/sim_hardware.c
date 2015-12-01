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
#include <errno.h>

#include "error.h"
#include "socket_server.h"

#include "hardware.h"


#define SERVER_NAME     "localhost"
#define SERVER_PORT     9999


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
    if (error)
    {
        ERROR_REPORT(error, "Error in simulation connection");
        kill_socket_server();
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


void hw_write_short_table(
    unsigned int block_base, unsigned int block_number,
    unsigned int reset_reg, unsigned int fill_reg,
    const uint32_t data[], size_t length)
{
    LOCK();
    handle_error(
        write_command_int(
            'S', block_base, block_number, fill_reg, (uint32_t) length)  ?:
        write_all(data, length * sizeof(uint32_t)));
    UNLOCK();
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


void hw_write_bit_capture(uint32_t capture_mask)
{
    LOCK();
    handle_error(write_command_int('K', 0, 0, 0, capture_mask));
    UNLOCK();
}

void hw_write_position_capture(uint32_t capture_mask)
{
    LOCK();
    handle_error(write_command_int('M', 0, 0, 0, capture_mask));
    UNLOCK();
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Long table support. */

struct hw_long_table {
    unsigned int block_base;
    unsigned int count;
    size_t length;
    uint32_t *data[];
};


error__t hw_open_long_table(
    unsigned int block_base, unsigned int count, unsigned int order,
    struct hw_long_table **table, size_t *length)
{
    *table = malloc(sizeof(struct hw_long_table) + count * sizeof(uint32_t *));
    *length = 1U << order;
    **table = (struct hw_long_table) {
        .block_base = block_base,
        .count = count,
        .length = *length,
    };
    for (unsigned int i = 0; i < count; i ++)
        (*table)->data[i] = malloc(sizeof(uint32_t) * *length);
    return ERROR_OK;
}


void hw_read_long_table_area(
    struct hw_long_table *table, unsigned int number, uint32_t **data)
{
    *data = table->data[number];
}


void hw_write_long_table_length(
    struct hw_long_table *table, unsigned int number, size_t length)
{
    ASSERT_OK(length <= table->length);

    /* Push updated table to simulation server. */
    LOCK();
    write_command_int(
        'L', table->block_base, number, 0, (uint32_t) length);
    write_all(table->data[number], length * sizeof(uint32_t));
    UNLOCK();
}


void hw_close_long_table(struct hw_long_table *table)
{
    for (unsigned int i = 0; i < table->count; i ++)
        free(table->data[i]);
    free(table);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

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
