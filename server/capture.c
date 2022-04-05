/* Data capture control. */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "error.h"
#include "parse.h"
#include "buffered_file.h"
#include "config_server.h"
#include "data_server.h"
#include "output.h"
#include "hardware.h"
#include "prepare.h"
#include "std_dev.h"

#include "capture.h"



struct field_group {
    size_t index;       // Index of first field in this group
    size_t count;       // Number of (output) fields in this group
    size_t scaling;     // Index of first scaling entry for this group
};

struct scaling {
    double scale;
    double offset;
};

/* This structure defines the process for generating data capture. */
struct data_capture {
    /* Number words in a single sample. */
    size_t raw_sample_words;

    size_t sample_count_index;      // Offset of sample count if required
    bool sample_count_anonymous;    // Sample count present but not in a group

    /* Counts, data indexes, and scaling indexes for fields with differing
     * process requirements.  For the 64-bit fields the index is in 32-bit words
     * but the count is in 64-bit words. */
    struct field_group unscaled;    // 32-bit fields with no processing
    struct field_group scaled32;    // 32-bit fields with scaling and offset
    struct field_group scaled64;    // 64-bit fields with scaling and offset
    struct field_group averaged;    // 64-bit accumulated sums
    struct field_group std_dev;     // Fields required for standard deviation

    /* Arrays of constants for scaling. */
    struct scaling scaling[MAX_CAPTURE_COUNT];
};


/* Need to take a little care: the 64-bit fields aren't aligned, so make sure
 * the compiler doesn't assume they are. */
typedef struct __attribute__((packed)) {
    int64_t value;
} unaligned_int64_t;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data transformation. */

/* Raw data is laid out thus:
 *
 *  +-----------+-----------+-----------+-----------+-----------+
 *  | hidden    | unscaled  | scaled    | scaled    | averaged  |
 *  |           | uint-32   | int-32    | int-64    | int-64    |
 *  +-----------+-----------+-----------+-----------+-----------+
 *   ^           ^           ^           ^           ^
 *  uncaptured  unscaled    scaled32   scaled64   averaged
 *  _count      _count      _count      _count      _count
 *
 * The hidden field includes the sample capture count when this is not
 * explicitly captured but is needed for averaging.
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


/* Copies given fields unprocessed to output. */
static size_t copy_unscaled_fields(
    const struct field_group *fields, const uint32_t input[],
    void *output, size_t field_size)
{
    memcpy(output, &input[fields->index], field_size * fields->count);
    return field_size * fields->count;
}



/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/* Scaled conversion. */

static size_t convert_scaled32(
    const struct data_capture *capture, const uint32_t input[], double output[])
{
    const int32_t *input_32 = (const void *) &input[capture->scaled32.index];
    const struct scaling *scaling =
        &capture->scaling[capture->scaled32.scaling];
    for (size_t i = 0; i < capture->scaled32.count; i ++)
        output[i] = scaling[i].scale * input_32[i] + scaling[i].offset;
    return sizeof(double) * capture->scaled32.count;
}


static size_t convert_scaled64(
    const struct data_capture *capture, const uint32_t input[], double output[])
{
    const unaligned_int64_t *input_64 =
        (const void *) &input[capture->scaled64.index];
    const struct scaling *scaling =
        &capture->scaling[capture->scaled64.scaling];
    for (size_t i = 0; i < capture->scaled64.count; i ++)
        output[i] =
            scaling[i].scale * (double) input_64[i].value + scaling[i].offset;
    return sizeof(double) * capture->scaled64.count;
}


static size_t average_scaled_data(
    const struct data_capture *capture, const uint32_t input[], double output[])
{
    const unaligned_int64_t *input_64 =
        (const void *) &input[capture->averaged.index];
    const struct scaling *scaling =
        &capture->scaling[capture->averaged.scaling];
    uint32_t sample_count = input[capture->sample_count_index];
    if (sample_count == 0)
        sample_count = 1;
    for (size_t i = 0; i < capture->averaged.count; i ++)
        output[i] =
            scaling[i].scale * (double) input_64[i].value / sample_count +
            scaling[i].offset;
    return sizeof(double) * capture->averaged.count;
}


static size_t convert_standard_deviation(
    const struct data_capture *capture, const uint32_t input[], double output[])
{
    uint32_t sample_count = input[capture->sample_count_index];
    const struct scaling *scaling = &capture->scaling[capture->std_dev.scaling];
    const unaligned_int64_t *raw_sums =
        (const void *) &input[capture->averaged.index];
    const unaligned_uint96_t *sum_squares =
        (const void *) &input[capture->std_dev.index];

    for (size_t i = 0; i < capture->std_dev.count; i ++)
        output[i] = scaling[i].scale * compute_standard_deviation(
            sample_count, raw_sums[i].value, &sum_squares[i]);
    return sizeof(double) * capture->std_dev.count;
}


static void convert_scaled_data(
    const struct data_capture *capture,
    unsigned int sample_count, const uint32_t input[], void *output)
{
    for (unsigned int i = 0; i < sample_count; i ++)
    {
        /* Capture the unscaled values. */
        output += copy_unscaled_fields(
            &capture->unscaled, input, output, sizeof(uint32_t));

        /* Perform all the scaling. */
        output += convert_scaled32(capture, input, output);
        output += convert_scaled64(capture, input, output);
        output += average_scaled_data(capture, input, output);
        output += convert_standard_deviation(capture, input, output);

        input += capture->raw_sample_words;
    }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/* Conversion. */


size_t get_raw_sample_length(const struct data_capture *capture)
{
    ASSERT_OK(capture->raw_sample_words > 0);
    return sizeof(uint32_t) * capture->raw_sample_words;
}


size_t get_binary_sample_length(
    const struct data_capture *capture, const struct data_options *options)
{
    size_t length = 0;
    switch (options->data_process)
    {
        case DATA_PROCESS_RAW:
            length = sizeof(uint32_t) * capture->raw_sample_words;
            break;
        case DATA_PROCESS_SCALED:
            length =
                sizeof(uint32_t) * capture->unscaled.count +
                sizeof(double) * (
                    capture->scaled32.count + capture->scaled64.count +
                    capture->averaged.count);
            break;
    }
    return length;
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
    /* Note: unscaled.index here acts as a counter for the "hidden" fields. */
    data += FORMAT_ASCII(
        capture->unscaled.index + capture->unscaled.count, data,
        " %"PRIu32, uint32_t);
    data += FORMAT_ASCII(
        capture->scaled32.count, data,
        " %"PRIi32, int32_t);
    data += FORMAT_ASCII(
        capture->scaled64.count + capture->averaged.count, data,
        " %"PRIi64, int64_t);
    return data;
}


static const void *send_scaled_as_ascii(
    const struct data_capture *capture,
    struct buffered_file *file, const void *data)
{
    data += FORMAT_ASCII(
        capture->unscaled.count, data, " %"PRIu32, uint32_t);
    data += FORMAT_ASCII(
        capture->scaled32.count + capture->scaled64.count +
        capture->averaged.count, data, PRIdouble, double);
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


/* Used to gather the final capture state. */
struct gather {
    struct data_capture *capture;   // Associated data capture area
    unsigned int scaling_count;     // Number of entries written to scaling
    unsigned int capture_count;     // Number of entries to be captured
    unsigned int *capture_array;    // List of capture indices
};



/* Emits a single output, returns index of captured value. */
static unsigned int emit_capture(
    struct gather *gather, const struct capture_info *field,
    unsigned int index_count, bool scaled)
{
    struct data_capture *capture = gather->capture;
    unsigned int capture_index = gather->capture_count;
    for (unsigned int i = 0; i < index_count; i ++)
        gather->capture_array[capture_index + i] =
            field->capture_index.index[i];
    gather->capture_count += index_count;

    if (scaled)
    {
        capture->scaling[gather->scaling_count] = (struct scaling) {
            .scale = field->scale,
            .offset = field->offset,
        };
        gather->scaling_count += 1;
    }

    return capture_index;
}


/* Ensure that the sample count is available.  If it is already being captured
 * then work out where it will appear in the unscaled list, otherwise add it to
 * the anonymous capture group at the start. */
static bool ensure_sample_count(
    const struct captured_fields *fields, struct gather *gather)
{
    struct data_capture *capture = gather->capture;

    /* Search the unscaled captures for a sample matching the sample count. */
    unsigned int sample_count_capture =
        fields->sample_count->capture_index.index[0];
    for (unsigned int i = 0; i < fields->unscaled.count; i ++)
    {
        struct capture_info *field = fields->unscaled.outputs[i];
        if (field->capture_index.index[0] == sample_count_capture)
        {
            /* Already being captured.  Because the unscaled group goes first
             * and we're the only entry, this is the right index. */
            capture->sample_count_index = i;
            /* Sample count is in a group, not anonymous */
            return false;
        }
    }

    /* Not being captured.  We need to emit a capture entry right now.  Because
     * we're first and only, we go into the anonymous group. */
    capture->sample_count_index =
        emit_capture(gather, fields->sample_count, 1, false);
    return true;
}


static void prepare_output_group(
    struct gather *gather, const struct capture_group *group,
    struct field_group *fields, unsigned int index_count, bool scaled)
{
    *fields  = (struct field_group) {
        .index = gather->capture_count,
        .scaling = gather->scaling_count,
        .count = group->count,
    };

    for (unsigned int i = 0; i < group->count; i ++)
        emit_capture(gather, group->outputs[i], index_count, scaled);
}


static struct data_capture data_capture_state;


static void gather_data_capture(
    const struct captured_fields *fields,
    const struct data_capture **capture_out,
    unsigned int *capture_count, unsigned int capture_array[])
{
    struct gather gather = {
        .capture = &data_capture_state,
        .capture_array = capture_array,
    };

    /* Work through the fields. */
    if (fields->averaged.count > 0)
        data_capture_state.sample_count_anonymous =
            ensure_sample_count(fields, &gather);
    else
        data_capture_state.sample_count_anonymous = false;

    prepare_output_group(
        &gather, &fields->unscaled, &data_capture_state.unscaled, 1, false);
    prepare_output_group(
        &gather, &fields->scaled32, &data_capture_state.scaled32, 1, true);
    prepare_output_group(
        &gather, &fields->scaled64, &data_capture_state.scaled64, 2, true);
    prepare_output_group(
        &gather, &fields->averaged, &data_capture_state.averaged, 2, true);
    data_capture_state.raw_sample_words = gather.capture_count;

    *capture_out = &data_capture_state;
    *capture_count = gather.capture_count;
}


error__t prepare_data_capture(
    const struct captured_fields *fields,
    const struct data_capture **capture)
{
    /* Capture information to send to hardware. */
    unsigned int capture_count;     // Number of entries to be captured
    unsigned int capture_array[MAX_CAPTURE_COUNT];   // List of capture indices

    gather_data_capture(fields, capture, &capture_count, capture_array);
    return
        TEST_OK_(capture_count > 0, "Nothing configured for capture")  ?:
        TEST_OK_(capture_count < MAX_PCAP_WRITE_COUNT,
            "Too many captures for PCAP")  ?:
        /* Now we can let the hardware know. */
        DO(hw_write_capture_set(capture_array, capture_count));
}


bool sample_count_is_anonymous(const struct data_capture *capture)
{
    return capture->sample_count_anonymous;
}
