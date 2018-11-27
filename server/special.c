/* Special field types. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>

#include "error.h"
#include "parse.h"
#include "hardware.h"
#include "config_server.h"
#include "fields.h"
#include "locking.h"

#include "special.h"


struct xadc_state {
    int offset;                 // Offset to apply to raw value
    double scale;               // Scaling factor for formatted result
    char *raw_file;             // File name to read value from

    pthread_mutex_t mutex;
    int raw_value;              // Last value read
    uint64_t update_index;      // Timestamp of last update
};


static error__t xadc_class_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data,
    struct indent_parser *parser)
{
    struct xadc_state *xadc = malloc(sizeof(struct xadc_state));
    *xadc = (struct xadc_state) {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .update_index = 1,
    };
    *class_data = xadc;
    return TEST_OK_(count == 1, "Cannot have multiple instances of xadc field");
}


/* Reads formatted value from named file. */
static error__t read_file_value(
    const char *filename, const char *format, void *result)
{
    FILE *file;
    return
        TEST_OK_(file = fopen(filename, "r"),
            "Unable to open node \"%s\"", filename)  ?:
        DO_FINALLY(
            TEST_OK(fscanf(file, format, result) == 1),
        // finally
            fclose(file));
}


#define FORMAT_FILENAME(filename, base_path, suffix) \
    DO(snprintf(filename, sizeof(filename), "%s_%s", base_path, suffix))


static error__t xadc_parse_register(
    void *class_data, struct field *field, unsigned int block_base,
    const char **line)
{
    /* Treat entire line as path to sysfs node to read.  We'll add the _offset,
     * _scale, and _raw suffixes to find our nodes. */
    const char *base_path = skip_whitespace(*line);
    *line = strchr(base_path, '\0');

    struct xadc_state *xadc = class_data;
    char filename[PATH_MAX];
    return
        /* Read offset if present, otherwise defaults to 0. */
        FORMAT_FILENAME(filename, base_path, "offset")  ?:
        IF(access(filename, R_OK) == 0,
            read_file_value(filename, "%d", &xadc->offset))  ?:
        /* Read scale, must be present. */
        FORMAT_FILENAME(filename, base_path, "scale")  ?:
        read_file_value(filename, "%lg", &xadc->scale)  ?:
        /* Finally save copy of the full raw file name to read for value. */
        FORMAT_FILENAME(filename, base_path, "raw")  ?:
        DO(xadc->raw_file = strdup(filename));
}


static void xadc_destroy(void *class_data)
{
    struct xadc_state *xadc = class_data;
    free(xadc->raw_file);
    free(class_data);
}


static error__t xadc_raw_get(struct xadc_state *xadc)
{
    int raw_value;
    return
        read_file_value(xadc->raw_file, "%d", &raw_value)  ?:
        DO(
            if (raw_value != xadc->raw_value)
            {
                xadc->raw_value = raw_value;
                xadc->update_index = get_change_index();
            }
        );
}


static error__t xadc_get(
    void *class_data, unsigned int number, char result[], size_t length)
{
    struct xadc_state *xadc = class_data;
    return
        WITH_LOCK(xadc->mutex, xadc_raw_get(xadc))  ?:
        format_double(result, length,
            1e-3 * xadc->scale * (xadc->raw_value + xadc->offset));
}


static void xadc_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct xadc_state *xadc = class_data;
    LOCK(xadc->mutex);
    ERROR_REPORT(xadc_raw_get(xadc),
        "Unexpected error reading XADC value");
    changes[0] = xadc->update_index > report_index;
    UNLOCK(xadc->mutex);
}


const struct class_methods xadc_class_methods = {
    "xadc",
    .init = xadc_class_init,
    .parse_register = xadc_parse_register,
    .destroy = xadc_destroy,
    .get = xadc_get,
    .change_set = xadc_change_set,
    .change_set_index = CHANGE_IX_READ,
};


/******************************************************************************/


static error__t parse_nibble(const char **string, uint8_t *nibble)
{
    char ch = *(*string)++;
    if ('0' <= ch  &&  ch <= '9')
        *nibble = (uint8_t) (ch - '0');
    else if ('A' <= ch  &&  ch <= 'F')
        *nibble = (uint8_t) (ch - 'A' + 10);
    else if ('a' <= ch  &&  ch <= 'f')
        *nibble = (uint8_t) (ch - 'a' + 10);
    else
        return FAIL_("Invalid character in octet");
    return ERROR_OK;
}

static error__t parse_octet(const char **string, uint8_t *octet)
{
    uint8_t high_nibble = 0, low_nibble = 0;
    return
        parse_nibble(string, &high_nibble)  ?:
        parse_nibble(string, &low_nibble)  ?:
        DO(*octet = (uint8_t) (high_nibble << 4 | low_nibble));
}


/* Very rigid parsing: a MAC address line is six hexadecimal octet specifiers
 * separated by colons and ending with a newline character. */
static error__t parse_mac_address(const char **line, uint64_t *mac_address)
{
    *mac_address = 0;
    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < 6; i ++)
    {
        uint8_t octet;
        error =
            IF(i > 0, parse_char(line, ':'))  ?:
            parse_octet(line, &octet)  ?:
            DO(*mac_address = *mac_address << 8 | octet);
    }
    return error  ?:  parse_char(line, '\n');
}


error__t load_mac_address_file(const char *filename)
{
    FILE *input;
    error__t error =
        TEST_OK_IO_(input = fopen(filename, "r"),
            "Unable to open MAC address file");

    unsigned int offset = 0;
    char line_buffer[82];
    unsigned int line_no = 0;
    while (!error  &&  fgets(line_buffer, sizeof(line_buffer), input))
    {
        const char *line = line_buffer;
        line_no += 1;

        /* Very rigid fixed file format.  Each line is one of three things:
         * 1. A comment starting with #
         * 2. A blank line representing a missing MAC address entry
         * 3. A MAC address in form XX:XX:XX:XX:XX:XX
         * At most 4 blank or MAC address lines may be present. */
        if (line[0] == '#')
            /* Comment line.  Ensure line isn't too long for buffer. */
            error = TEST_OK_(strchr(line, '\n'),
                "Comment line too long or missing newline");
        else if (line[0] == '\n')
            /* Blank line.  Just advance the offset counter. */
            offset += 1;
        else
        {
            uint64_t mac_address;
            /* This had better be a MAC address! */
            error =
                TEST_OK_(offset < MAC_ADDRESS_COUNT,
                    "Too many MAC address entries")  ?:
                parse_mac_address(&line, &mac_address)  ?:
                DO( hw_write_mac_address(offset, mac_address);
                    offset += 1);
        }
        if (error)
            error_extend(error, "Error on line %u offset %zu",
                line_no, line - line_buffer);
    }

    if (input)
        fclose(input);
    return error;
}
