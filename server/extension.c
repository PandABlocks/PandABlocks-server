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
#include <ctype.h>
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

static bool legacy_mode = false;


error__t initialise_extension_server(unsigned int port, bool _legacy_mode)
{
    legacy_mode = _legacy_mode;

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


/* A server parse exchange is pretty sterotyped: we send a newline terminated
 * request string, and either get a response with the same prefix character and
 * a list of numbers, or an error response. */
static error__t extension_server_exchange(
    const char *message, unsigned int count, unsigned int result[])
{
    char prefix = message[0];
    char result_buffer[256];
    const char *response = result_buffer;
    return
        TEST_OK_(server.file, "Extension server not running")  ?:
        ERROR_WITH_MUTEX(server.mutex,
            TEST_OK_(
                write_string(server.file, message, strlen(message))  &&
                read_line(server.file,
                    result_buffer, sizeof(result_buffer), true),
                "Extension server communication failure"))  ?:
        IF_ELSE(read_char(&response, prefix),
            /* Successful response.  Response should be a list of integers. */
            ERROR_EXTEND(
                parse_uint_array(&response, result, count)  ?:
                parse_eos(&response),
                /* If we have a problem with the response augment the error
                 * message to help with diagnosing extension server errors. */
                "Error at offset %zd in response \"%s\"",
                response - result_buffer + 1, result_buffer),
        //else
            /* The only other valid response is an error message. */
            parse_char(&response, 'E')  ?:
            FAIL_("%s", response));
}


/* Sends parse request to server, parses response, which should either be a
 * parse error message or a successful parse response id. */
static error__t extension_server_parse_block(
    unsigned int count, const char *request, unsigned int *parse_id)
{
    char message[128];
    return
        format_string(message, sizeof(message), "B%d %s\n", count, request)  ?:
        extension_server_exchange(message, 1, parse_id);
}


/* Sends parse request to server, parses response, which should either be a
 * parse error message or a successful parse response id. */
static error__t extension_server_parse_field(
    unsigned int block_id, bool write_not_read, const char *request,
    unsigned int *parse_id)
{
    char message[128];
    return
        format_string(message, sizeof(message),
            "P%c%d %s\n", write_not_read ? 'W' : 'R', block_id, request)  ?:
        extension_server_exchange(message, 1, parse_id);
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

#define UNUSED_REGISTER     UINT_MAX

struct extension_address {
    unsigned int block_base;
    unsigned int parse_id;
    unsigned int read_count;
    unsigned int write_count;
    unsigned int *read_registers;
    unsigned int *write_registers;
};


/* Counts number of strings that look like integers. */
static unsigned int count_uint_strings(const char *line)
{
    unsigned int count = 0;
    while (true)
    {
        line = skip_whitespace(line);
        if (isdigit(*line))
        {
            count += 1;
            do {
                line += 1;
            } while (isdigit(*line));
        }
        else
            return count;
    }
}


/* Parses array of strings into an array of integers. */
static error__t parse_register_array(
    const char **line, unsigned int *count, unsigned int **array)
{
    *count = count_uint_strings(*line);
    if (*count > 0)
    {
        *array = malloc(sizeof(unsigned int) * *count);
        error__t error = parse_uint_array(line, *array, *count);
        for (unsigned int i = 0; !error  &&  i < *count; i ++)
            error = TEST_OK_((*array)[i] < BLOCK_REGISTER_COUNT,
                "Register value too large");
        *line = skip_whitespace(*line);
        return error;
    }
    else
    {
        *array = NULL;
        return ERROR_OK;
    }
}


static error__t parse_extension_name(
    const char **line, unsigned int block_id,
    bool write_not_read, unsigned int *parse_id)
{
    const char *request;
    return
        parse_whitespace(line)  ?:
        parse_utf8_string(line, &request)  ?:
        extension_server_parse_field(
            block_id, write_not_read, request, parse_id);
}


/* In legacy mode we only support a single register in write mode, and we have
 * to switch this register into the write position. */
static error__t check_legacy_mode(
    struct extension_address *extension, bool write_not_read)
{
    error__t error =
        TEST_OK_(extension->write_count == 0,
            "Explicit W registers not supported in legacy mode")  ?:
        TEST_OK_(write_not_read  ||  extension->read_count == 0,
            "No registers supported in read mode")  ?:
        TEST_OK_(extension->read_count <= 1,
            "Cannot write to more than one register");
    if (!error)
    {
        /* Move the one read register into the write position. */
        extension->write_count = extension->read_count;
        extension->write_registers = extension->read_registers;
        extension->read_count = 0;
        extension->read_registers = NULL;
    }
    return error;
}


/* The extension register syntax is:
 *
 *      [register]* X extension-name
 *
 * for read registers and
 *
 *      [register]* [W [register]*] X extension-name
 *
 * for write registers. */
error__t parse_extension_register(
    const char **line, struct extension_block *block,
    unsigned int block_base,
    bool write_not_read, struct extension_address **address)
{
    struct extension_address extension = {
        .block_base = block_base,
    };

    error__t error =
        TEST_OK_(block, "No extensions defined for this block")  ?:
        parse_register_array(
            line, &extension.read_count, &extension.read_registers)  ?:
        IF(read_char(line, 'W'),
            TEST_OK_(write_not_read,
                "Cannot specify write registers for read type")  ?:
            parse_register_array(
                line, &extension.write_count, &extension.write_registers))  ?:
        parse_char(line, 'X')  ?:
        parse_extension_name(
            line, block->block_id, write_not_read, &extension.parse_id)  ?:
        IF(legacy_mode, check_legacy_mode(&extension, write_not_read));

    if (error)
    {
        free(extension.read_registers);
        free(extension.write_registers);
    }
    else
    {
        *address = malloc(sizeof(struct extension_address));
        **address = extension;
    }

    return error;
}


void destroy_extension_address(struct extension_address *address)
{
    free(address->read_registers);
    free(address->write_registers);
    free(address);
}


/* Reads the specified hardware registers and formats onto the end of the given
 * format string. */
static error__t __attribute__((format(printf, 5, 6))) read_hardware_registers(
    const struct extension_address *address,
    char *buffer, size_t length, unsigned int number, const char *format, ...)
{
    /* First format the prefix.  We know this won't fail. */
    va_list args;
    va_start(args, format);
    int written = vsnprintf(buffer, length, format, args);
    va_end(args);

    /* Consume buffer space just written. */
    ASSERT_OK(written > 0  &&  (size_t) written < length);
    buffer += written;
    length -= (size_t) written;

    /* Now read and format the hardware registers. */
    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < address->read_count; i ++)
    {
        uint32_t value = hw_read_register(
            address->block_base, number, address->read_registers[i]);
        written = snprintf(buffer, length, " %u", value);
        error = TEST_OK_(written > 0  &&  (size_t) written < length,
            "Buffer overflow reading registers");
        buffer += written;
        length -= (size_t) written;
    }

    /* Put \n on the end, if there's room. */
    return
        error  ?:
        TEST_OK_(length >= 2, "Buffer overflow reading registers")  ?:
        DO(strcpy(buffer, "\n"));
}


/* Returns current value of the given extension register. */
error__t extension_read_register(
    const struct extension_address *address, unsigned int number,
    uint32_t *result)
{
    char message[256];
    return
        read_hardware_registers(
            address, message, sizeof(message), number,
            "R%u %u", address->parse_id, number)  ?:
        extension_server_exchange(message, 1, result);
}


/* Writes the given value to the given extension register. */
error__t extension_write_register(
    const struct extension_address *address,
    unsigned int number, uint32_t value)
{
    char message[256];
    unsigned int results[address->write_count];
    unsigned int write_count = legacy_mode ? 0 : address->write_count;

    if (legacy_mode  &&  address->write_count > 0)
        hw_write_register(
            address->block_base, number,
            address->write_registers[0], value);

    error__t error =
        read_hardware_registers(
            address, message, sizeof(message), number,
            "W%u %u %u", address->parse_id, number, value)  ?:
        extension_server_exchange(message, write_count, results);

    /* If writing was successful write the registers. */
    if (!error  &&  !legacy_mode)
    {
        for (unsigned int i = 0; i < address->write_count; i ++)
            hw_write_register(
                address->block_base, number,
                address->write_registers[i], results[i]);
    }
    return error;
}
