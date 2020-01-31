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
        .sin_port = htons((in_port_t) port)
    };
    int one = 1;
    int sock;
    return
        TEST_IO(sock = socket(AF_INET, SOCK_STREAM, 0))  ?:
        DO(server.file = create_buffered_file(sock, 4096, 4096))  ?:
        TEST_IO_(connect(sock, (struct sockaddr *) &s_in, sizeof(s_in)),
            "Unable to connect to extension server")  ?:
        set_timeout(sock, SO_SNDTIMEO, 5)  ?:
        set_timeout(sock, SO_RCVTIMEO, 5)  ?:
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


/* A server parse exchange is pretty sterotyped: we send a newline terminated
 * request string, and either get a response with the same prefix character and
 * a single number, or an error response. */
static error__t extension_server_exchange(
    unsigned int *parse_id, const char *format, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    char prefix = buffer[0];
    const char *response = buffer;
    return
        TEST_OK_(server.file, "Extension server not running")  ?:
        SERVER_EXCHANGE(
            write_string(server.file, buffer, (size_t) length)  &&
            read_line(server.file, buffer, sizeof(buffer), true))  ?:
        IF_ELSE(read_char(&response, prefix),
            // Successful parse.  Response should be a single integer
            parse_uint(&response, parse_id)  ?:
            parse_eos(&response),
        //else
            // The only other valid response is an error message
            parse_char(&response, 'E')  ?:
            FAIL_("%s", response));
}


/* Sends parse request to server, parses response, which should either be a
 * parse error message or a successful parse response id. */
static error__t extension_server_parse_block(
    unsigned int count, const char *request, unsigned int *parse_id)
{
    return extension_server_exchange(parse_id, "B%d %s\n", count, request);
}


/* Sends parse request to server, parses response, which should either be a
 * parse error message or a successful parse response id. */
static error__t extension_server_parse_field(
    unsigned int block_id, bool write_not_read, const char *request,
    unsigned int *parse_id)
{
    return extension_server_exchange(parse_id,
        "P%c%d %s\n", write_not_read ? 'W' : 'R', block_id, request);
}


static uint32_t extension_server_read(
    unsigned int parse_id, unsigned int number)
{
    unsigned int result = 0;
    ERROR_REPORT(
        extension_server_exchange(&result, "R%u %u\n", parse_id, number),
        "Error reading from extension server");
    return result;
}


static void extension_server_write(
    unsigned int parse_id, unsigned int number, uint32_t value)
{
    error_report(SERVER_EXCHANGE(
        write_formatted_string(server.file,
            "W%u %u %u\n", parse_id, number, value)  &&
        flush_out_buf(server.file)));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Block interface. */

struct extension_block {
    unsigned int block_id;
};


static struct extension_block *create_extension_block(unsigned int block_id)
{
    struct extension_block *block = malloc(sizeof(struct extension_block));
    *block = (struct extension_block) {
        .block_id = block_id,
    };
    return block;
}


error__t parse_extension_block(
    const char **line, unsigned int count, struct extension_block **block)
{
    const char *request;
    unsigned int block_id;
    return
        parse_utf8_string(line, &request)  ?:
        extension_server_parse_block(count, request, &block_id)  ?:
        DO(*block = create_extension_block(block_id));
}


void destroy_extension_block(struct extension_block *block)
{
    free(block);
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
    const char **line, struct extension_block *block,
    bool write_not_read, struct extension_address **address)
{
    const char *request;
    unsigned int parse_id;
    return
        TEST_OK_(block, "No extensions defined for this block")  ?:
        parse_whitespace(line)  ?:
        parse_utf8_string(line, &request)  ?:
        extension_server_parse_field(
            block->block_id, write_not_read, request, &parse_id)  ?:
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
