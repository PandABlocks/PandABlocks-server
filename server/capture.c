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

#include "capture.h"



struct field_group {
    size_t index;       // Index of first field in this group
    size_t count;       // Number of (output) fields in this group
    size_t scaling;     // Index of first scaling entry for this group
};


/* This structure defines the process for generating data capture. */
struct data_capture {
    /* Number words in a single sample. */
    size_t raw_sample_words;

    /* Timestamp capture and offset control. */
    enum ts_capture ts_capture;

    /* Offsets of special fields into sample. */
    size_t timestamp_index;     // Raw 64-bit timestamp
    size_t ts_offset_index;     // Timestamp offset correct, if required
    size_t adc_count_index;     // ADC capture count

    /* Counts, data indexes, and scaling indexes for fields with differing
     * process requirements.  For the 64-bit fields the index is in 32-bit words
     * but the count is in 64-bit words. */
    struct field_group unscaled;    // 32-bit fields with no processing
    struct field_group scaled32;    // 32-bit fields with scaling and offset
    struct field_group scaled64;    // 64-bit fields with scaling and offset
    struct field_group adc_mean;    // 64-bit accumulated ADC sums

    /* Scaling for timestamp. */
    double timestamp_scale;

    /* Arrays of constants for scaling. */
    struct scaling scaling[CAPTURE_BUS_COUNT];
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data transformation. */

/* Raw data is laid out thus:
 *
 *  +-----------+-----------+-----------+-----------+-----------+
 *  | hidden    | unscaled  | scaled    | scaled    | adc mean  |
 *  |           | uint-32   | int-32    | int-64    | int-64    |
 *  +-----------+-----------+-----------+-----------+-----------+
 *   ^           ^           ^           ^           ^
 *  uncaptured  unscaled    scaled32   scaled64   adc_mean
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
    const struct data_capture *capture,
    const uint32_t input[], uint64_t *output)
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


/* Copies given fields unprocessed to output. */
static size_t copy_unscaled_fields(
    const struct field_group *fields, const uint32_t input[],
    void *output, size_t field_size)
{
    memcpy(output, &input[fields->index], field_size * fields->count);
    return field_size * fields->count;
}



/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/* Unscaled conversion. */


/* Averaged ADC samples are scaled by 2^8 to avoid losing precision.  This will
 * work safely for up to 24 bit ADCs. */
static size_t average_unscaled_adc(
    const struct data_capture *capture,
    const uint32_t input[], uint32_t output[])
{
    uint32_t adc_sample_count = input[capture->adc_count_index];
    const int64_t *adc_means = (const void *) &input[capture->adc_mean.index];
    for (size_t i = 0; i < capture->adc_mean.count; i ++)
        output[i] = (uint32_t) ((adc_means[i] << 8) / adc_sample_count);
    return sizeof(uint32_t) * capture->adc_mean.count;
}


/* For unscaled data we do two conversions to the data: apply the timestamp
 * offset if requried, and perform the averaging of ADC data.  The remaining
 * values are copied unchanged. */
static void convert_unscaled_data(
    const struct data_capture *capture,
    unsigned int sample_count, const uint32_t input[], void *output)
{
    for (unsigned int i = 0; i < sample_count; i ++)
    {
        /* Timestamp if requested with appropriate adjustment. */
        if (capture_raw_timestamp(capture, input, output))
            output += sizeof(uint64_t);

        /* Copy over the unscaled and scaled values. */
        output += copy_unscaled_fields(
            &capture->unscaled, input, output, sizeof(uint32_t));
        output += copy_unscaled_fields(
            &capture->scaled32, input, output, sizeof(uint32_t));
        output += copy_unscaled_fields(
            &capture->scaled64, input, output, sizeof(uint64_t));

        /* Process the ADC values. */
        output += average_unscaled_adc(capture, input, output);

        input += capture->raw_sample_words;
    }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
/* Scaled conversion. */

static bool capture_scaled_timestamp(
    const struct data_capture *capture, const uint32_t input[], double *output)
{
    uint64_t timestamp;
    bool ts_present = capture_raw_timestamp(capture, input, &timestamp);
    if (ts_present)
        *output = capture->timestamp_scale * (double) timestamp;
    return ts_present;
}


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
    const int64_t *input_64 = (const void *) &input[capture->scaled64.index];
    const struct scaling *scaling =
        &capture->scaling[capture->scaled64.scaling];
    for (size_t i = 0; i < capture->scaled64.count; i ++)
        output[i] = scaling[i].scale * (double) input_64[i] + scaling[i].offset;
    return sizeof(double) * capture->scaled64.count;
}


static size_t average_scaled_adc(
    const struct data_capture *capture, const uint32_t input[], double output[])
{
    const int64_t *input_64 = (const void *) &input[capture->adc_mean.index];
    const struct scaling *scaling =
        &capture->scaling[capture->adc_mean.scaling];
    uint32_t adc_sample_count = input[capture->adc_count_index];
    for (size_t i = 0; i < capture->adc_mean.count; i ++)
        output[i] =
            scaling[i].scale * (double) input_64[i] / adc_sample_count +
            scaling[i].offset;
    return sizeof(double) * capture->adc_mean.count;
}


static void convert_scaled_data(
    const struct data_capture *capture,
    unsigned int sample_count, const uint32_t input[], void *output)
{
    for (unsigned int i = 0; i < sample_count; i ++)
    {
        /* Capture timestamp if requested. */
        if (capture_scaled_timestamp(capture, input, output))
            output += sizeof(double);

        /* Capture the unscaled values. */
        output += copy_unscaled_fields(
            &capture->unscaled, input, output, sizeof(uint32_t));

        /* Perform all the scaling. */
        output += convert_scaled32(capture, input, output);
        output += convert_scaled64(capture, input, output);
        output += average_scaled_adc(capture, input, output);

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
        case DATA_PROCESS_UNSCALED:
            length =
                sizeof(uint32_t) * (
                    capture->unscaled.count + capture->scaled32.count +
                    capture->adc_mean.count) +
                sizeof(uint64_t) * capture->scaled64.count;
            if (capture->ts_capture != TS_IGNORE)
                length += sizeof(uint64_t);
            break;
        case DATA_PROCESS_SCALED:
            length =
                sizeof(uint32_t) * capture->unscaled.count +
                sizeof(uint64_t) * (
                    capture->scaled32.count + capture->scaled64.count +
                    capture->adc_mean.count);
            if (capture->ts_capture != TS_IGNORE)
                length += sizeof(double);
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
    /* Note: unscaled.index here acts as a counter for the "hidden" fields. */
    data += FORMAT_ASCII(
        capture->unscaled.index + capture->unscaled.count, data,
        " %"PRIu32, uint32_t);
    data += FORMAT_ASCII(
        capture->scaled32.count, data,
        " %"PRIi32, int32_t);
    data += FORMAT_ASCII(
        capture->scaled64.count + capture->adc_mean.count, data,
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
        capture->unscaled.count, data, " %"PRIu32, uint32_t);
    data += FORMAT_ASCII(
        capture->scaled32.count, data, " %"PRIi32, int32_t);
    data += FORMAT_ASCII(
        capture->scaled64.count, data, " %"PRIi64, int64_t);
    data += FORMAT_ASCII(
        capture->adc_mean.count, data, " %"PRIi32, int32_t);
    return data;
}


static const void *send_scaled_as_ascii(
    const struct data_capture *capture,
    struct buffered_file *file, const void *data)
{
    if (capture->ts_capture != TS_IGNORE)
        data += FORMAT_ASCII(1, data, PRIdouble, double);
    data += FORMAT_ASCII(
        capture->unscaled.count, data, " %"PRIu32, uint32_t);
    data += FORMAT_ASCII(
        capture->scaled32.count + capture->scaled64.count +
        capture->adc_mean.count, data, PRIdouble, double);
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


/* Used to gather the final capture state. */
struct gather {
    uint32_t framing_mask;          // Framing setup as request by output
    uint32_t framing_mode;

    unsigned int scaling_count;     // Number of entries written to scaling
    struct data_capture *capture;   // Associated data capture area

    unsigned int capture_count;     // Number of entries to be captured
    unsigned int capture_index[CAPTURE_BUS_COUNT];   // List of capture indices
};



/* Emits a single output, returns index of captured value. */
static unsigned int emit_capture(
    struct gather *gather, const struct output_field *output,
    unsigned int index_count, bool scaled)
{
    unsigned int capture_index = gather->capture_count;
    enum framing_mode framing_mode = get_output_info(
        output,
        &gather->capture_index[capture_index],
        &gather->capture->scaling[gather->scaling_count]);
    /* Pick up frame identification bit if required from first capture index. */
    uint32_t frame_bit = 1U << gather->capture_index[capture_index];

    /* Emit the capture index and any scaling if required. */
    gather->capture_count += index_count;
    if (scaled)
        gather->scaling_count += 1;

    /* Set the framing masks as appropriate. */
    switch (framing_mode)
    {
        case FRAMING_TRIGGER:   break;
        case FRAMING_FRAME:
            gather->framing_mask |= frame_bit;
            break;
        case FRAMING_SPECIAL:
            gather->framing_mask |= frame_bit;
            gather->framing_mode |= frame_bit;
            break;
    }

    return capture_index;
}


/* Ensures that the given capture group is being captured and records its index,
 * adds to the list of captured fields as necessary.  If the output was found
 * then true is returned to indicate that the computed offset will need to be
 * corrected afterwards. */
static bool ensure_output_captured(
    struct gather *gather,
    const struct capture_group *group, const struct output_field *output,
    size_t *ix)
{
    /* Search capture group for output. */
    for (unsigned int i = 0; i < group->count; i ++)
        if (group->outputs[i] == output)
        {
            *ix = i;
            return true;
        }

    /* Not present, so add to capture list. */
    *ix = emit_capture(gather, output, 1, false);
    return false;
}


/* The timestamp and adc capture counts need special treatment.  We need to
 * ensure that they're captured if required and place their capture indexes. */
static void prepare_fixed_outputs(
    struct gather *gather, const struct captured_fields *fields)
{
    struct data_capture *capture = gather->capture;
    capture->ts_capture = fields->ts_capture;

    /* Timestamp and offset, if necessary. */
    bool fixup_ts_offset = false;
    if (fields->ts_capture != TS_IGNORE)
    {
        emit_capture(gather, fields->timestamp, 2, false);
        if (fields->ts_capture == TS_OFFSET)
            fixup_ts_offset = ensure_output_captured(
                gather, &fields->unscaled, fields->offset,
                &capture->ts_offset_index);
    }

    /* ADC count if any ADC mean fields. */
    bool fixup_adc_count = false;
    if (fields->adc_mean.count > 0)
        fixup_adc_count = ensure_output_captured(
            gather, &fields->unscaled, fields->adc_count,
            &capture->adc_count_index);

    /* Fixup the ts_offset and adc_count fields if required by adding the offset
     * of the starting area of the unscaled area -- which we only just know now,
     * having fully populated it. */
    if (fixup_ts_offset)
        capture->ts_offset_index += gather->capture_count;
    if (fixup_adc_count)
        capture->adc_count_index += gather->capture_count;
}


static void prepare_output_group(
    struct gather *gather, const struct capture_group *group,
    struct field_group *fields, unsigned int index_count, bool scaled)
{
    fields->index = gather->capture_count;
    fields->scaling = gather->scaling_count;
    fields->count = group->count;

    for (unsigned int i = 0; i < group->count; i ++)
        emit_capture(gather, group->outputs[i], index_count, scaled);
}


static void gather_data_capture(
    const struct captured_fields *fields, struct gather *gather)
{
    /* Work through the fields. */
    struct data_capture *capture = gather->capture;
    prepare_fixed_outputs(gather, fields);
    prepare_output_group(
        gather, &fields->unscaled, &capture->unscaled, 1, false);
    prepare_output_group(
        gather, &fields->scaled32, &capture->scaled32, 1, true);
    prepare_output_group(
        gather, &fields->scaled64, &capture->scaled64, 2, true);
    prepare_output_group(
        gather, &fields->adc_mean, &capture->adc_mean, 2, true);
    capture->raw_sample_words = gather->capture_count;
}


static void dump_data_capture(struct data_capture *capture)
{
    printf("capture: %zu %d %zu %zu %zu\n",
        capture->raw_sample_words, capture->ts_capture,
        capture->timestamp_index, capture->ts_offset_index,
        capture->adc_count_index);
    printf(" %zu %zu / %zu %zu %zu / %zu %zu %zu / %zu %zu %zu\n",
        capture->unscaled.index, capture->unscaled.count,
        capture->scaled32.index, capture->scaled32.count,
        capture->scaled32.scaling,
        capture->scaled64.index, capture->scaled64.count,
        capture->scaled64.scaling,
        capture->adc_mean.index, capture->adc_mean.count,
        capture->adc_mean.scaling);
    size_t scaling_count =
        capture->adc_mean.scaling + capture->adc_mean.count;
    for (unsigned int i = 0; i < scaling_count; i ++)
        printf(" (%g %g)",
            capture->scaling[i].scale, capture->scaling[i].offset);
    printf("\n");
}


static struct data_capture data_capture_state = {
    .timestamp_scale = 1.0 / CLOCK_FREQUENCY,
};


error__t prepare_data_capture(
    const struct captured_fields *fields,
    const struct data_capture **capture)
{
    struct gather gather = {
        .capture = &data_capture_state,
    };

    gather_data_capture(fields, &gather);
    error__t error =
        TEST_OK_(gather.capture_count > 0,
            "Nothing configured for capture");
    if (!error)
    {
        /* Now we can let the hardware know. */
        hw_write_framing_mask(gather.framing_mask, gather.framing_mode);
        hw_write_capture_set(gather.capture_index, gather.capture_count);
        hw_write_framing_enable(
            fields->ts_capture == TS_OFFSET  ||  gather.framing_mask);
        *capture = gather.capture;
    }

    dump_data_capture(gather.capture);
    return error;
}
