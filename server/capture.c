/* Data capture control. */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "error.h"
#include "parse.h"
#include "buffered_file.h"
#include "config_server.h"
#include "data_server.h"
#include "classes.h"
#include "output.h"
#include "locking.h"
#include "hardware.h"

#include "capture.h"


struct data_capture {
    size_t raw_sample_words;
};

static struct data_capture data_capture_state = {
    .raw_sample_words = 11,
};


struct data_capture *prepare_data_capture(void)
{
    printf("Write capture masks\n");
    printf("Write capture set\n");
    return &data_capture_state;
}


static error__t parse_one_option(
    const char *option, struct data_options *options)
{
    /* Data formatting options. */
    if (strcmp(option, "UNFRAMED") == 0)
        options->data_format = DATA_FORMAT_RAW;
    else if (strcmp(option, "FRAMED") == 0)
        options->data_format = DATA_FORMAT_FRAMED;
    else if (strcmp(option, "BASE64") == 0)
        options->data_format = DATA_FORMAT_BASE64;
    else if (strcmp(option, "ASCII") == 0)
        options->data_format = DATA_FORMAT_ASCII;

    /* Data processing options. */
    else if (strcmp(option, "RAW") == 0)
        options->data_process = DATA_PROCESS_RAW;
    else if (strcmp(option, "UNSCALED") == 0)
        options->data_process = DATA_PROCESS_UNSCALED;
    else if (strcmp(option, "SCALED") == 0)
        options->data_process = DATA_PROCESS_SCALED;

    /* Reporting and control options. */
    else if (strcmp(option, "NO_HEADER") == 0)
        options->omit_header = true;
    else if (strcmp(option, "NO_STATUS") == 0)
        options->omit_status = true;
    else if (strcmp(option, "ONE_SHOT") == 0)
        options->one_shot = true;

    /* Some compound options. */
    else if (strcmp(option, "BARE") == 0)
        *options = (struct data_options) {
            .data_format = DATA_FORMAT_RAW,
            .data_process = DATA_PROCESS_UNSCALED,
            .omit_header = true,
            .omit_status = true,
            .one_shot = true,
        };
    else if (strcmp(option, "DEFAULT") == 0)
        *options = (struct data_options) {
            .data_format = DATA_FORMAT_ASCII,
            .data_process = DATA_PROCESS_SCALED,
        };

    else
        return FAIL_("Invalid data capture option");
    return ERROR_OK;
}


error__t parse_data_options(const char *line, struct data_options *options)
{
    *options = (struct data_options) {
        .data_format = DATA_FORMAT_ASCII,
        .data_process = DATA_PROCESS_SCALED,
    };

    char option[MAX_NAME_LENGTH];
    error__t error = ERROR_OK;
    while (!error  &&  *line)
        error =
            DO(line = skip_whitespace(line))  ?:
            parse_alphanum_name(&line, option, sizeof(option))  ?:
            parse_one_option(option, options);
    return
        error  ?:
        parse_eos(&line);
}


size_t get_raw_sample_length(struct data_capture *capture)
{
    return sizeof(uint32_t) * capture->raw_sample_words;
}


size_t get_binary_sample_length(
    struct data_capture *capture, struct data_options *options)
{
    return sizeof(uint32_t) * capture->raw_sample_words;
}


bool send_data_header(
    struct data_capture *capture, struct data_options *options,
    struct buffered_file *file)
{
    write_string(file, "header\n", 7);
    return flush_out_buf(file);
}


void convert_raw_data_to_binary(
    struct data_capture *capture, struct data_options *options,
    unsigned int sample_count, const void *input, void *output)
{
    size_t sample_size = get_raw_sample_length(capture);
    memcpy(output, input, sample_size * sample_count);
}


bool send_binary_as_ascii(
    struct data_capture *capture, struct data_options *options,
    struct buffered_file *file, unsigned int sample_count,
    const void *data, size_t data_length)
{
    unsigned int raw_sample_words = (unsigned int) capture->raw_sample_words;
    /* It is possible for data_length and the computed data length to become
     * uncoupled.  If this happens, log a warning and exit without writing. */
    if (data_length != raw_sample_words * sample_count * sizeof(uint32_t))
    {
        log_error("In send_binary_as_ascii %zu != %u * %u",
            data_length, raw_sample_words, sample_count);
        return false;
    }

    const uint32_t *data_in = data;
    for (unsigned int i = 0; i < sample_count; i ++)
    {
        for (unsigned int j = 0; j < capture->raw_sample_words; j ++)
            write_formatted_string(file, " %d", *data_in++);
        write_char(file, '\n');
    }
    return check_buffered_file(file);
}
