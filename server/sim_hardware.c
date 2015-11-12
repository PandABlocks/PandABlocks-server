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


static int sock = -1;

/* Need to make sure our communications are atomic. */
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()      ASSERT_PTHREAD(pthread_mutex_lock(&session_lock))
#define UNLOCK()    ASSERT_PTHREAD(pthread_mutex_unlock(&session_lock))


static error__t write_all(int file, const void *data, size_t length)
{
    error__t error = ERROR_OK;
    ssize_t written;
    while (!error  &&  length > 0)
        error =
            TEST_IO(written = write(file, data, length))  ?:
            DO(
                data   += (size_t) written;
                length -= (size_t) written;
            );
    return error;
}


static error__t read_all(int file, void *data, size_t length)
{
    error__t error = ERROR_OK;
    ssize_t received;
    while (!error  &&  length > 0)
        error =
            TEST_IO(received = read(file, data, length))  ?:
            DO(
                data   += (size_t) received;
                length -= (size_t) received;
            );
    return error;
}


void hw_write_config(
    unsigned int block_base, unsigned int block_number, unsigned int reg,
    uint32_t value)
{
    printf("hw_write_config %u:%u:%u <= %u\n",
        block_base, block_number, reg, value);
    unsigned char command[8] = {
        'W',
        (unsigned char) block_base,
        (unsigned char) block_number,
        (unsigned char) reg
    };
    *CAST_FROM_TO(unsigned char *, uint32_t *, &command[4]) = value;

    LOCK();
    error__t error = write_all(sock, command, 8);
    UNLOCK();

    if (error)
    {
        ERROR_REPORT(error, "Failed to write register");
        kill_socket_server();
    }
}


uint32_t hw_read_data(
    unsigned int block_base, unsigned int block_number, unsigned int reg)
{
    printf("hw_read_data %u:%u:%u ", block_base, block_number, reg);
    unsigned char command[4] = {
        'R',
        (unsigned char) block_base,
        (unsigned char) block_number,
        (unsigned char) reg
    };

    LOCK();
    uint32_t result = 0;
    error__t error =
        write_all(sock, command, 4)  ?:
        read_all(sock, &result, 4);
    UNLOCK();

    if (error)
    {
        ERROR_REPORT(error, "Failed to read register");
        kill_socket_server();
    }
    printf("=> %u\n", result);
    return result;
}


void hw_write_table_data(
    unsigned int block_base, unsigned int block_number, unsigned int reg,
    bool start, const uint32_t data[], size_t length)
{
    printf("hw_write_table_data %u:%u:%u %d %p %zu",
        block_base, block_number, reg, start, data, length);
    unsigned char command[8] = {
        start ? 'B' : 'A',      // Block or Append
        (unsigned char) block_base,
        (unsigned char) block_number,
        (unsigned char) reg
    };
    *CAST_FROM_TO(unsigned char *, uint32_t *, &command[4]) = (uint32_t) length;

    LOCK();
    error__t error =
        write_all(sock, command, 8)  ?:
        write_all(sock, data, length);
    UNLOCK();

    if (error)
    {
        ERROR_REPORT(error, "Failed to write block");
        kill_socket_server();
    }
}


void hw_read_bits(bool bits[BIT_BUS_COUNT], bool changes[BIT_BUS_COUNT])
{
    printf("hw_read_bits\n");
    unsigned char command[4] = { 'C', 0, 0, 0 };

    LOCK();
    error__t error =
        write_all(sock, command, 4)  ?:
        read_all(sock, bits, BIT_BUS_COUNT)  ?:
        read_all(sock, changes, BIT_BUS_COUNT);
    UNLOCK();

    if (error)
    {
        ERROR_REPORT(error, "Failed to read bits");
        kill_socket_server();
    }
}


void hw_read_positions(
    uint32_t positions[POS_BUS_COUNT], bool changes[POS_BUS_COUNT])
{
    printf("hw_read_positions\n");
    unsigned char command[4] = { 'P', 0, 0, 0 };

    LOCK();
    error__t error =
        write_all(sock, command, 4)  ?:
        read_all(sock, positions, sizeof(uint32_t) * POS_BUS_COUNT)  ?:
        read_all(sock, changes, POS_BUS_COUNT);
    UNLOCK();

    if (error)
    {
        ERROR_REPORT(error, "Failed to read positions");
        kill_socket_server();
    }
}


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
