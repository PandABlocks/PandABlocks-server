/* Data capture control. */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
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


/* This structure defines the process for generating data capture. */
struct data_capture {
    /* Number words in a single sample. */
    size_t raw_sample_words;

    /* Timestamp capture and offset control. */
    enum ts_capture { TS_IGNORE, TS_CAPTURE, TS_OFFSET } ts_capture;

    /* Offsets of special fields into sample. */
    size_t timestamp_index;     // Raw 64-bit timestamp
    size_t ts_offset_index;     // Timestamp offset correct, if required
    size_t adc_count_index;     // ADC capture count

    /* Counts, data indexes, and scaling indexes for fields with differing
     * process requirements.  For the 64-bit fields the index is in 32-bit words
     * but the count is in 64-bit words. */
    size_t unscaled_index;      // 32-bit fields which have no processing
    size_t unscaled_count;
    size_t scaled_32_index;     // 32-bit fields with scaling and offset
    size_t scaled_32_count;
    size_t scaled_32_scaling;
    size_t scaled_64_index;     // 64-bit fields with scaling and offset
    size_t scaled_64_count;
    size_t scaled_64_scaling;
    size_t adc_sum_index;       // 64-bit accumulated ADC fields for averaging
    size_t adc_sum_count;
    size_t adc_sum_scaling;

    /* Scaling for timestamp. */
    double timestamp_scale;

    /* Arrays of constants for scaling. */
    struct scaling {
        double scale;
        double offset;
    } *scaling;
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data capture request parsing. */

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


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data transformation. */

/* Raw data is laid out thus:
 *
 *  +-----------+-----------+-----------+-----------+-----------+
 *  | hidden    | unscaled  | scaled    | scaled    | adc mean  |
 *  |           | uint-32   | int-32    | int-64    | int-64    |
 *  +-----------+-----------+-----------+-----------+-----------+
 *   ^           ^           ^           ^           ^
 *  uncaptured  unscaled    scaled_32   scaled_64   adc_sum
 *  _count      _count      _count      _count      _count
 *
 * The hidden fields include the timestamp (handled separately), and the
 * timestamp offset and adc capture count when these are not explicitly captured
 * but are needed for computations.
 *
 * The effect of the conversion depends on the selected conversion, with the
 * following corresponding results:
 *
 * RAW: The entire raw data buffer is transmitted including the "hidden" fields.
 *
 *  +-----------+-----------+-----------+-----------+-----------+
 *  | uint-32   | uint-32   | int-32    | int-64    | int-64    |
 *  +-----------+-----------+-----------+-----------+-----------+
 *
 * UNSCALED: The timestamp is generated at the start followed by the original
 * data, except that the ADC data is averaged (and scaled by 8 bits):
 *      TS
 *     +--------+-----------+-----------+-----------+-----------+
 *     |uint-64 | uint-32   | int-32    | int-64    | int-32    |
 *     +--------+-----------+-----------+-----------+-----------+
 *
 * SCALED: The timestamp is generated at the start followed by the data scaled
 * and averaged as appropriate:
 *      TS
 *     +--------+-----------+-----------+-----------+-----------+
 *     | double | uint-32   | double    | double    | double    |
 *     +--------+-----------+-----------+-----------+-----------+
 * */


static bool capture_raw_timestamp(
    const struct data_capture *capture, const uint32_t *input, uint64_t *output)
{
    const uint64_t *timestamp = (const void *) &input[capture->timestamp_index];
    switch (capture->ts_capture)
    {
        case TS_IGNORE: default:
            return false;
        case TS_CAPTURE:
            *output = *timestamp;
            return true;
        case TS_OFFSET:
            *output = *timestamp - input[capture->ts_offset_index];
            return true;
    }
}


#define COPY_UNSCALED_FIELDS(capture, input, output, field, type) \
    memcpy(output, &input[capture->field##_index], \
        sizeof(type) * capture->field##_count); \
    output += sizeof(type) * capture->field##_count


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/* Unscaled conversion. */


/* Averaged ADC samples are scaled by 2^8 to avoid losing precision.  This will
 * work safely for up to 24 bit ADCs. */
static size_t average_unscaled_adc(
    const struct data_capture *capture, const uint32_t *input, uint32_t *output)
{
    uint32_t adc_sample_count = input[capture->adc_count_index];
    const int64_t *adc_sums = (const void *) &input[capture->adc_sum_index];
    for (size_t i = 0; i < capture->adc_sum_count; i ++)
        output[i] = (uint32_t) ((adc_sums[i] << 8) / adc_sample_count);
    return sizeof(uint32_t) * capture->adc_sum_count;
}


/* For unscaled data we do two conversions to the data: apply the timestamp
 * offset if requried, and perform the averaging of ADC data.  The remaining
 * values are copied unchanged. */
static void convert_unscaled_data(
    const struct data_capture *capture,
    unsigned int sample_count, const uint32_t *input, void *output)
{
    for (unsigned int i = 0; i < sample_count; i ++)
    {
        /* Timestamp if requested with appropriate adjustment. */
        if (capture_raw_timestamp(capture, input, output))
            output += sizeof(uint64_t);

        /* Copy over the unscaled and scaled values. */
        COPY_UNSCALED_FIELDS(capture, input, output, unscaled,  uint32_t);
        COPY_UNSCALED_FIELDS(capture, input, output, scaled_32, uint32_t);
        COPY_UNSCALED_FIELDS(capture, input, output, scaled_64, uint64_t);

        /* Process the ADC values. */
        output += average_unscaled_adc(capture, input, output);

        input += capture->raw_sample_words;
    }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/* Scaled conversion. */

static bool capture_scaled_timestamp(
    const struct data_capture *capture, const uint32_t *input, double *output)
{
    uint64_t timestamp;
    bool ts_present = capture_raw_timestamp(capture, input, &timestamp);
    if (ts_present)
        *output = capture->timestamp_scale * (double) timestamp;
    return ts_present;
}


static size_t convert_scaled_32(
    const struct data_capture *capture, const uint32_t *input, double *output)
{
    const int32_t *input_32 = (const void *) &input[capture->scaled_32_index];
    struct scaling *scaling = &capture->scaling[capture->scaled_32_scaling];
    for (size_t i = 0; i < capture->scaled_32_count; i ++)
        output[i] = scaling[i].scale * input_32[i] + scaling[i].offset;
    return sizeof(double) * capture->scaled_32_count;
}


static size_t convert_scaled_64(
    const struct data_capture *capture, const uint32_t *input, double *output)
{
    const int64_t *input_64 = (const void *) &input[capture->scaled_64_index];
    struct scaling *scaling = &capture->scaling[capture->scaled_64_scaling];
    for (size_t i = 0; i < capture->scaled_64_count; i ++)
        output[i] = scaling[i].scale * (double) input_64[i] + scaling[i].offset;
    return sizeof(double) * capture->scaled_64_count;
}


static size_t average_scaled_adc(
    const struct data_capture *capture, const uint32_t *input, double *output)
{
    const int64_t *input_64 = (const void *) &input[capture->adc_sum_index];
    struct scaling *scaling = &capture->scaling[capture->adc_sum_scaling];
    uint32_t adc_sample_count = input[capture->adc_count_index];
    for (size_t i = 0; i < capture->adc_sum_count; i ++)
        output[i] =
            scaling[i].scale * (double) input_64[i] / adc_sample_count +
            scaling[i].offset;
    return sizeof(double) * capture->adc_sum_count;
}


static void convert_scaled_data(
    const struct data_capture *capture,
    unsigned int sample_count, const uint32_t *input, void *output)
{
    for (unsigned int i = 0; i < sample_count; i ++)
    {
        /* Capture timestamp if requested. */
        if (capture_scaled_timestamp(capture, input, output))
            output += sizeof(double);

        /* Capture the unscaled values. */
        COPY_UNSCALED_FIELDS(capture, input, output, unscaled, uint32_t);

        /* Perform all the scaling. */
        output += convert_scaled_32(capture, input, output);
        output += convert_scaled_64(capture, input, output);
        output += average_scaled_adc(capture, input, output);

        input += capture->raw_sample_words;
    }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/* Conversion. */


size_t get_raw_sample_length(const struct data_capture *capture)
{
    return sizeof(uint32_t) * capture->raw_sample_words;
}


size_t get_binary_sample_length(
    const struct data_capture *capture, struct data_options *options)
{
    size_t length = 0;
    switch (options->data_process)
    {
        case DATA_PROCESS_RAW:
            length = sizeof(uint32_t) * capture->raw_sample_words;
            break;
        case DATA_PROCESS_UNSCALED:
            length =
                sizeof(uint32_t) * (
                    capture->unscaled_count + capture->scaled_32_count +
                    capture->adc_sum_count) +
                sizeof(uint64_t) * capture->scaled_64_count;
            if (capture->ts_capture != TS_IGNORE)
                length += sizeof(uint64_t);
            break;
        case DATA_PROCESS_SCALED:
            length =
                sizeof(uint32_t) * capture->unscaled_count +
                sizeof(uint64_t) * (
                    capture->scaled_32_count + capture->scaled_64_count +
                    capture->adc_sum_count);
            if (capture->ts_capture != TS_IGNORE)
                length += sizeof(double);
            break;
    }
    return length;
}


bool send_data_header(
    const struct data_capture *capture, struct data_options *options,
    struct buffered_file *file, uint64_t lost_samples)
{
    write_formatted_string(
        file, "header: lost %"PRIu64" samples\n", lost_samples);
    write_string(file, "header\n", 7);
    return flush_out_buf(file);
}


void convert_raw_data_to_binary(
    const struct data_capture *capture, struct data_options *options,
    unsigned int sample_count, const void *input, void *output)
{
    switch (options->data_process)
    {
        case DATA_PROCESS_RAW:
            memcpy(output, input,
                sample_count * get_raw_sample_length(capture));
            break;
        case DATA_PROCESS_UNSCALED:
            convert_unscaled_data(capture, sample_count, input, output);
            break;
        case DATA_PROCESS_SCALED:
            convert_scaled_data(capture, sample_count, input, output);
            break;
    }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/* Output in ASCII. */


/* Helper routine for printing in ASCII format. */
#define FORMAT_ASCII(count, data, format, type) \
    ( { \
        const type *values = data; \
        for (size_t i = 0; i < (count); i ++) \
            write_formatted_string(file, format, values[i]); \
        (count) * sizeof(type); \
    } )

#define PRIdouble   " %.10g"


/* Sends a single row of raw data formatted in ASCII. */
static const void *send_raw_as_ascii(
    const struct data_capture *capture,
    struct buffered_file *file, const void *data)
{
    data += FORMAT_ASCII(
        capture->unscaled_index + capture->unscaled_count, data,
        " %"PRIu32, uint32_t);
    data += FORMAT_ASCII(
        capture->scaled_32_count, data,
        " %"PRIi32, int32_t);
    data += FORMAT_ASCII(
        capture->scaled_64_count + capture->adc_sum_count, data,
        " %"PRIi64, int64_t);
    return data;
}


static const void *send_unscaled_as_ascii(
    const struct data_capture *capture,
    struct buffered_file *file, const void *data)
{
    if (capture->ts_capture != TS_IGNORE)
        data += FORMAT_ASCII(1, data, " %"PRIu64, uint64_t);
    data += FORMAT_ASCII(
        capture->unscaled_count, data, " %"PRIu32, uint32_t);
    data += FORMAT_ASCII(
        capture->scaled_32_count, data, " %"PRIi32, int32_t);
    data += FORMAT_ASCII(
        capture->scaled_64_count, data, " %"PRIi64, int64_t);
    data += FORMAT_ASCII(
        capture->adc_sum_count, data, " %"PRIi32, int32_t);
    return data;
}


static const void *send_scaled_as_ascii(
    const struct data_capture *capture,
    struct buffered_file *file, const void *data)
{
    if (capture->ts_capture != TS_IGNORE)
        data += FORMAT_ASCII(1, data, PRIdouble, double);
    data += FORMAT_ASCII(
        capture->unscaled_count, data, " %"PRIu32, uint32_t);
    data += FORMAT_ASCII(
        capture->scaled_32_count + capture->scaled_64_count +
        capture->adc_sum_count, data, PRIdouble, double);
    return data;
}


/* We need to take the conversion into account to understand the data layout
 * when converting to ASCII numbers. */
bool send_binary_as_ascii(
    const struct data_capture *capture, struct data_options *options,
    struct buffered_file *file, unsigned int sample_count, const void *data)
{
    for (unsigned int i = 0; i < sample_count; i ++)
    {
        switch (options->data_process)
        {
            case DATA_PROCESS_RAW:
                data = send_raw_as_ascii(capture, file, data);
                break;
            case DATA_PROCESS_UNSCALED:
                data = send_unscaled_as_ascii(capture, file, data);
                break;
            case DATA_PROCESS_SCALED:
                data = send_scaled_as_ascii(capture, file, data);
                break;
        }
        write_char(file, '\n');
    }
    return check_buffered_file(file);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data capture preparation. */

static struct data_capture data_capture_state = {
    .raw_sample_words = 11,
    .ts_capture = TS_OFFSET,
    .timestamp_index = 0,
    .ts_offset_index = 2,
    .adc_count_index = 3,

    .unscaled_index = 4,
    .unscaled_count = 1,
    .scaled_32_index = 5,
    .scaled_32_count = 2,
    .scaled_32_scaling = 0,     // Always zero
    .scaled_64_index = 7,
    .scaled_64_count = 1,
    .scaled_64_scaling = 2,     // Always scaled_32_count
    .adc_sum_index = 9,
    .adc_sum_count = 1,
    .adc_sum_scaling = 3,
    .timestamp_scale = 8e-9,    // Always
    .scaling = (struct scaling[]) {
        { 0.1, 1 }, { 0.2, 2 }, { 0.3, 3 }, { 0.4, 4 }, },
};


const struct data_capture *prepare_data_capture(void)
{
    printf("Write capture masks\n");
    printf("Write capture set\n");
    return &data_capture_state;
}
