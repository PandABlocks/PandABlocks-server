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


/* This is a safe upper bound on the number of outputs, and is useful for a
 * number of temporary buffers. */
#define MAX_OUTPUT_COUNT    64


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
    size_t scaled32_index;     // 32-bit fields with scaling and offset
    size_t scaled32_count;
    size_t scaled32_scaling;
    size_t scaled64_index;     // 64-bit fields with scaling and offset
    size_t scaled64_count;
    size_t scaled64_scaling;
    size_t adc_mean_index;       // 64-bit accumulated ADC fields for averaging
    size_t adc_mean_count;
    size_t adc_mean_scaling;

    /* Scaling for timestamp. */
    double timestamp_scale;

    /* Array of captured outputs for header reporting. */
    size_t capture_count;
    struct captured_output **captured_output;

    /* Arrays of constants for scaling. */
    struct scaling scaling[MAX_OUTPUT_COUNT];
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
    const int64_t *adc_means = (const void *) &input[capture->adc_mean_index];
    for (size_t i = 0; i < capture->adc_mean_count; i ++)
        output[i] = (uint32_t) ((adc_means[i] << 8) / adc_sample_count);
    return sizeof(uint32_t) * capture->adc_mean_count;
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
        COPY_UNSCALED_FIELDS(capture, input, output, scaled32, uint32_t);
        COPY_UNSCALED_FIELDS(capture, input, output, scaled64, uint64_t);

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


static size_t convert_scaled32(
    const struct data_capture *capture, const uint32_t *input, double *output)
{
    const int32_t *input_32 = (const void *) &input[capture->scaled32_index];
    const struct scaling *scaling =
        &capture->scaling[capture->scaled32_scaling];
    for (size_t i = 0; i < capture->scaled32_count; i ++)
        output[i] = scaling[i].scale * input_32[i] + scaling[i].offset;
    return sizeof(double) * capture->scaled32_count;
}


static size_t convert_scaled64(
    const struct data_capture *capture, const uint32_t *input, double *output)
{
    const int64_t *input_64 = (const void *) &input[capture->scaled64_index];
    const struct scaling *scaling =
        &capture->scaling[capture->scaled64_scaling];
    for (size_t i = 0; i < capture->scaled64_count; i ++)
        output[i] = scaling[i].scale * (double) input_64[i] + scaling[i].offset;
    return sizeof(double) * capture->scaled64_count;
}


static size_t average_scaled_adc(
    const struct data_capture *capture, const uint32_t *input, double *output)
{
    const int64_t *input_64 = (const void *) &input[capture->adc_mean_index];
    const struct scaling *scaling =
        &capture->scaling[capture->adc_mean_scaling];
    uint32_t adc_sample_count = input[capture->adc_count_index];
    for (size_t i = 0; i < capture->adc_mean_count; i ++)
        output[i] =
            scaling[i].scale * (double) input_64[i] / adc_sample_count +
            scaling[i].offset;
    return sizeof(double) * capture->adc_mean_count;
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
                    capture->unscaled_count + capture->scaled32_count +
                    capture->adc_mean_count) +
                sizeof(uint64_t) * capture->scaled64_count;
            if (capture->ts_capture != TS_IGNORE)
                length += sizeof(uint64_t);
            break;
        case DATA_PROCESS_SCALED:
            length =
                sizeof(uint32_t) * capture->unscaled_count +
                sizeof(uint64_t) * (
                    capture->scaled32_count + capture->scaled64_count +
                    capture->adc_mean_count);
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
        capture->scaled32_count, data,
        " %"PRIi32, int32_t);
    data += FORMAT_ASCII(
        capture->scaled64_count + capture->adc_mean_count, data,
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
        capture->scaled32_count, data, " %"PRIi32, int32_t);
    data += FORMAT_ASCII(
        capture->scaled64_count, data, " %"PRIi64, int64_t);
    data += FORMAT_ASCII(
        capture->adc_mean_count, data, " %"PRIi32, int32_t);
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
        capture->scaled32_count + capture->scaled64_count +
        capture->adc_mean_count, data, PRIdouble, double);
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


struct captured_output {
    /* The following fields are initialised at startup. */
    const struct output *output;    // Source of this output
    const char *name;               // Printable field name for header
    enum output_class output_class; // Do we need this?
    unsigned int capture_index[2];  // Lower and upper words for capture

    /* The following fields are set up during output selection. */
    enum capture_mode capture_mode;
    enum framing_mode framing_mode;
    struct scaling scaling;
};


/* Here are our registered outputs, including the special sources. */
static struct captured_output outputs[MAX_OUTPUT_COUNT];
static unsigned int output_count = 0;

/* Offsets into outputs of the three special fields. */
static struct captured_output *timestamp_output;
static struct captured_output *offset_output;
static struct captured_output *adc_count_output;



void register_output(
    const struct output *output, const char *name,
    enum output_class output_class, unsigned int capture_index[2])
{
    ASSERT_OK(output_count < MAX_OUTPUT_COUNT);
    outputs[output_count] = (struct captured_output) {
        .output = output,
        .name = name,
        .output_class = output_class,
        .capture_index = { capture_index[0], capture_index[1], },
    };

    switch (output_class)
    {
        case OUTPUT_CLASS_NORMAL:
            break;
        case OUTPUT_CLASS_TIMESTAMP:
            timestamp_output = &outputs[output_count];
            break;
        case OUTPUT_CLASS_TS_OFFSET:
            offset_output = &outputs[output_count];
            break;
        case OUTPUT_CLASS_ADC_COUNT:
            adc_count_output = &outputs[output_count];
            break;
    }
    output_count += 1;
}


/* This structure is used as we gather our outputs into four groups for the
 * different output processing steps. */
struct capture_group {
    unsigned int count;
    struct captured_output *outputs[MAX_OUTPUT_COUNT];
};


/* Gather the four capture groups and any extra specials required. */
static enum ts_capture gather_capture_groups(
    struct capture_group *unscaled, struct capture_group *scaled32,
    struct capture_group *scaled64, struct capture_group *adc_mean)
{
    unscaled->count = 0;
    scaled32->count = 0;
    scaled64->count = 0;
    adc_mean->count = 0;
    enum ts_capture ts_capture = TS_IGNORE;

    /* Walk the list of outputs and gather them into their groups. */
    for (unsigned int i = 0; i < output_count; i ++)
    {
        outputs[i].capture_mode = get_capture_mode(
            outputs[i].output, &outputs[i].framing_mode, &outputs[i].scaling);

        struct capture_group *capture = NULL;
        switch (outputs[i].capture_mode)
        {
            case CAPTURE_OFF:       break;
            case CAPTURE_UNSCALED:  capture = unscaled; break;
            case CAPTURE_SCALED32:  capture = scaled32; break;
            case CAPTURE_SCALED64:  capture = scaled64; break;
            case CAPTURE_ADC_MEAN:  capture = adc_mean; break;

            case CAPTURE_TS_NORMAL: ts_capture = TS_CAPTURE;    break;
            case CAPTURE_TS_OFFSET: ts_capture = TS_OFFSET;     break;
        }
        if (capture)
            capture->outputs[capture->count++] = &outputs[i];
    }
    return ts_capture;
}


/* Used to gather the final capture state. */
struct gather {
    uint32_t framing_mask;          // Framing setup as request by output
    uint32_t framing_mode;

    unsigned int scaling_count;     // Number of entries written to scaling
    unsigned int capture_count;     // Number of entries to be captured

    struct data_capture *capture;   // Data capture area (for scaling)

    unsigned int capture_index[MAX_OUTPUT_COUNT];   // List of capture indices
};



/* The table below shows how the capture mode controls the number of captured
 * fields and the scaling settings.
 *
 *      capture_mode    capture fields  scaling
 *      ------------    --------------  -------
 *      _OFF            0               -
 *      _UNSCALED       1               -
 *      _SCALED32       1               present
 *      _SCALED64       2               present
 *      _ADC_MEAN       2               present
 *      _TS_NORMAL      2               -
 *      _TS_OFFSET      2 (+offset)     -
 *
 * Note that the capture count for _OFF is fudged here so that emit_capture()
 * will work when adding an uncaptured output. */
static const struct convert_capture_mode {
    unsigned int count;
    bool scaling;
} convert_capture_mode[] = {
    { 1, false, },      // CAPTURE_OFF.  We fudge the capture count here.
    { 1, false, },      // CAPTURE_UNSCALED
    { 1, true,  },      // CAPTURE_SCALED32
    { 2, true,  },      // CAPTURE_SCALED64
    { 2, true,  },      // CAPTURE_ADC_MEAN
    { 2, false, },      // CAPTURE_TS_NORMAL
    { 2, false, },      // CAPTURE_TS_OFFSET
};


/* Emits a single output, returns index of captured value. */
static unsigned int emit_capture(
    struct gather *gather, struct captured_output *output)
{
    /* From the capture mode work out what processing is needed. */
    struct convert_capture_mode convert =
        convert_capture_mode[output->capture_mode];

    /* Emit the capture index and any scaling if required. */
    unsigned int capture_index = gather->capture_count;
    for (unsigned int i = 0; i < convert.count; i ++)
        gather->capture_index[gather->capture_count++] =
            output->capture_index[i];
    struct data_capture *capture = gather->capture;
    if (convert.scaling)
        capture->scaling[gather->scaling_count++] = output->scaling;

    /* Set the framing masks as appropriate. */
    uint32_t frame_bit = 1U << output->capture_index[0];
    switch (output->framing_mode)
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

    /* Unless fields is hidden add it to the set of captured outputs. */
    if (output->capture_mode != CAPTURE_OFF)
        capture->captured_output[capture->capture_count++] = output;

    return capture_index;
}


/* Ensures that the given capture group is being captured and records its index,
 * adds to the list of captured fields as necessary.  If the output was found
 * then true is returned to indicate that the computed offset will need to be
 * corrected afterwards. */
static bool ensure_output_captured(
    struct gather *gather,
    struct capture_group *group, struct captured_output *output,
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
    *ix = emit_capture(gather, output);
    return false;
}


/* The timestamp and adc capture counts need special treatment.  We need to
 * ensure that they're captured if required and place their capture indexes. */
static void prepare_fixed_outputs(
    struct gather *gather, enum ts_capture ts_capture,
    struct capture_group *unscaled, bool adc_count_needed)
{
    struct data_capture *capture = gather->capture;
    capture->ts_capture = ts_capture;

    /* Timestamp and offset, if necessary. */
    bool fixup_ts_offset = false;
    if (ts_capture != TS_IGNORE)
    {
        emit_capture(gather, timestamp_output);
        if (ts_capture == TS_OFFSET)
            fixup_ts_offset = ensure_output_captured(
                gather, unscaled, offset_output, &capture->ts_offset_index);
    }

    /* ADC count if any ADC mean fields. */
    bool fixup_adc_count = false;
    if (adc_count_needed)
        fixup_adc_count = ensure_output_captured(
            gather, unscaled, adc_count_output, &capture->adc_count_index);

    /* Fixup the ts_offset and adc_count fields if required by adding the offset
     * of the starting area of the unscaled area -- which we only just know now,
     * having fully populated it. */
    if (fixup_ts_offset)
        capture->ts_offset_index += gather->capture_count;
    if (fixup_adc_count)
        capture->adc_count_index += gather->capture_count;
}


static void prepare_output_group(
    struct gather *gather, struct capture_group *group,
    size_t *start_index, size_t *count, size_t *scaling_index)
{
    *start_index = gather->capture_count;
    if (scaling_index)
        *scaling_index = gather->scaling_count;
    *count = group->count;

    for (unsigned int i = 0; i < group->count; i ++)
        emit_capture(gather, group->outputs[i]);
}


static void gather_data_capture(struct gather *gather)
{
    /* We need to start by grouping all the outputs into four capture groups. */
    struct capture_group unscaled;
    struct capture_group scaled32;
    struct capture_group scaled64;
    struct capture_group adc_mean;

    /* Walk the output list and gather outputs into the main categories. */
    enum ts_capture ts_capture = gather_capture_groups(
        &unscaled, &scaled32, &scaled64, &adc_mean);

    /* Work through the fields. */
    struct data_capture *capture = gather->capture;
    prepare_fixed_outputs(gather, ts_capture, &unscaled, adc_mean.count > 0);
    prepare_output_group(
        gather, &unscaled,
        &capture->unscaled_index, &capture->unscaled_count, NULL);
    prepare_output_group(
        gather, &scaled32,
        &capture->scaled32_index, &capture->scaled32_count,
        &capture->scaled32_scaling);
    prepare_output_group(
        gather, &scaled64,
        &capture->scaled64_index, &capture->scaled64_count,
        &capture->scaled64_scaling);
    prepare_output_group(
        gather, &adc_mean,
        &capture->adc_mean_index, &capture->adc_mean_count,
        &capture->adc_mean_scaling);
    capture->raw_sample_words = gather->capture_count;
}


static void dump_data_capture(struct data_capture *capture)
{
    printf("capture: %zu %d %zu %zu %zu\n",
        capture->raw_sample_words, capture->ts_capture,
        capture->timestamp_index, capture->ts_offset_index,
        capture->adc_count_index);
    printf(" %zu %zu / %zu %zu %zu / %zu %zu %zu / %zu %zu %zu\n",
        capture->unscaled_index, capture->unscaled_count,
        capture->scaled32_index, capture->scaled32_count,
        capture->scaled32_scaling,
        capture->scaled64_index, capture->scaled64_count,
        capture->scaled64_scaling,
        capture->adc_mean_index, capture->adc_mean_count,
        capture->adc_mean_scaling);
    printf(" %f %zu\n", capture->timestamp_scale, capture->capture_count);
}


static struct captured_output *captured_output[MAX_OUTPUT_COUNT];
static struct data_capture data_capture_state = {
    .captured_output = captured_output,
    .timestamp_scale = 1.0 / CLOCK_FREQUENCY,
};


error__t prepare_data_capture(const struct data_capture **capture)
{
    struct gather gather = {
        .capture = &data_capture_state,
    };
    data_capture_state.capture_count = 0;

    gather_data_capture(&gather);
    error__t error =
        TEST_OK_(data_capture_state.capture_count > 0,
            "Nothing configured for capture");
    if (!error)
    {
        /* Now we can let the hardware know. */
        hw_write_framing_mask(gather.framing_mask, gather.framing_mode);
        hw_write_capture_set(gather.capture_index, gather.capture_count);
        *capture = gather.capture;
    }

    dump_data_capture(gather.capture);
    return error;
}
