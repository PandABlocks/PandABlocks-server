/* Extension server for non-FPGA registers.
 *
 * These are all implemented by an external server. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>

#include "error.h"
#include "parse.h"
#include "hardware.h"
#include "config_server.h"
#include "socket_server.h"
#include "buffered_file.h"
#include "locking.h"

#include "extension.h"


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Connection to server. */

struct extension_server {
    struct buffered_file *file;
    pthread_mutex_t mutex;
};

static struct extension_server server = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
};


error__t initialise_extension_server(unsigned int port)
{
    struct sockaddr_in s_in = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        .sin_port = htons(port)
    };
    int one = 1;
    int sock;
    return
        TEST_IO(sock = socket(AF_INET, SOCK_STREAM, 0))  ?:
        DO(server.file = create_buffered_file(sock, 4096, 4096))  ?:
        TEST_IO_(connect(sock, (struct sockaddr *) &s_in, sizeof(s_in)),
            "Unable to connect to simulation server")  ?:
        set_timeout(sock, SO_SNDTIMEO, 1)  ?:
        set_timeout(sock, SO_RCVTIMEO, 1)  ?:
        TEST_IO(setsockopt(
            sock, SOL_TCP, TCP_NODELAY, &one, sizeof(one)));
}


void terminate_extension_server(void)
{
    if (server.file)
        ERROR_REPORT(destroy_buffered_file(server.file),
            "Error communicating with extension server");
}


/* Helper macro for communication with the extension server.  Performs all
 * transactions under the server mutex and returns an error code if any part of
 * the transaction fails. */
#define SERVER_EXCHANGE(actions) \
    WITH_LOCK(server.mutex, TEST_OK_(actions, \
        "Extension server communication failure"))


/* Sends parse request to server, parses response, which should either be a
 * parse error message or a successful parse response id. */
static error__t extension_server_parse(
    bool write_not_read, const char *request, unsigned int *parse_id)
{
    char buffer[80];
    const char *response = buffer;
    return
        SERVER_EXCHANGE(
            write_formatted_string(server.file,
                "P%c%s\n", write_not_read ? 'W' : 'R', request)  &&
            read_line(server.file, buffer, sizeof(buffer), true))  ?:
        IF_ELSE(read_char(&response, 'P'),
            // Successful parse.  Response should be a single integer
            parse_uint(&response, parse_id)  ?:
            parse_eos(&response),
        //else
            // The only other valid response is an error message
            parse_char(&response, 'E')  ?:
            FAIL_("%s", response));
}


static void extension_server_write(
    unsigned int parse_id, unsigned int number, uint32_t value)
{
    error_report(SERVER_EXCHANGE(
        write_formatted_string(server.file,
            "W%u %u %u\n", parse_id, number, value)  &&
        flush_out_buf(server.file)));
}


static uint32_t extension_server_read(
    unsigned int parse_id, unsigned int number)
{
    char buffer[80];
    const char *response = buffer;
    uint32_t result = 0;
    error_report(
        SERVER_EXCHANGE(
            write_formatted_string(server.file,
                "R%u %u\n", parse_id, number)  &&
            read_line(server.file, buffer, sizeof(buffer), true))  ?:
        parse_char(&response, 'R')  ?:
        parse_uint32(&response, &result)  ?:
        parse_eos(&response));
    return result;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Register interface. */

struct extension_address {
    unsigned int parse_id;
};


static struct extension_address *create_extension_address(
    unsigned int parse_id)
{
    struct extension_address *address =
        malloc(sizeof(struct extension_address));
    *address = (struct extension_address) {
        .parse_id = parse_id,
    };
    return address;
}


error__t parse_extension_address(
    const char **line, bool write_not_read, struct extension_address **address)
{
    const char *request;
    unsigned int parse_id;
    return
        parse_whitespace(line)  ?:
        parse_utf8_string(line, &request)  ?:
        extension_server_parse(write_not_read, request, &parse_id)  ?:
        DO(*address = create_extension_address(parse_id));
}


void destroy_extension_address(struct extension_address *address)
{
    free(address);
}


/* Writes the given value to the given extension register. */
void extension_write_register(
    const struct extension_address *address, unsigned int number,
    uint32_t value)
{
    extension_server_write(address->parse_id, number, value);
}


/* Returns current value of the given extension register. */
uint32_t extension_read_register(
    const struct extension_address *address, unsigned int number)
{
    return extension_server_read(address->parse_id, number);
}
