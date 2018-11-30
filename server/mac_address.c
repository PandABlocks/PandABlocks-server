/* FPGA MAC address support. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "error.h"
#include "parse.h"
#include "hardware.h"

#include "mac_address.h"


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Load MAC addresses. */


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
