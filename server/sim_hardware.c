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

#include "error.h"
#include "socket_server.h"

#include "hardware.h"


#define SERVER_NAME     "localhost"
#define SERVER_PORT     9999


static int sock = -1;



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
    ssize_t written;
    error__t error =
        TEST_IO(written = write(sock, command, 8))  ?:
        TEST_OK(written == 8);
    if (error)
    {
        ERROR_REPORT(error, "Failed to write register");
        kill_socket_server();
    }
}

uint32_t hw_read_config(
    unsigned int block_base, unsigned int block_number, unsigned int reg)
{
    printf("hw_read_config %u:%u:%u ", block_base, block_number, reg);
    unsigned char command[4] = {
        'R',
        (unsigned char) block_base,
        (unsigned char) block_number,
        (unsigned char) reg
    };
    uint32_t result = 0;
    ssize_t written, rx;
    error__t error =
        TEST_IO(written = write(sock, command, 4))  ?:
        TEST_OK(written == 4)  ?:
        TEST_IO(rx = read(sock, &result, 4))  ?:
        TEST_OK_IO(rx == 4);
    if (error)
    {
        ERROR_REPORT(error, "Failed to read register");
        kill_socket_server();
    }
    printf("=> %u\n", result);
    return result;
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
