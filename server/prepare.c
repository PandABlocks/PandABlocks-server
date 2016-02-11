/* Data capture preparation. */

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
#include "capture.h"

#include "prepare.h"




/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Output fields registration. */

/* This structure is used to record a single registered output field. */
struct output_field {
    /* The field is identified by the output and index number. */
    struct output *output;
    unsigned int number;

    /* The field name is computed at output registration. */
    char *field_name;
    /* The two capture index values for this field. */
    unsigned int capture_index[2];

    /* The following fields are updated during capture preparation. */
    struct capture_info info;
};


/* All registered outputs. */
static struct output_field *output_fields[CAPTURE_BUS_COUNT];
static unsigned int output_field_count;

/* Offsets into outputs of the three special fields. */
static struct output_field *timestamp_output;
static struct output_field *offset_output;
static struct output_field *adc_count_output;


/* The three special fields need to be remembered separately. */
static error__t process_special_field(
    enum prepare_class prepare_class, struct output_field *field)
{
    error__t error = ERROR_OK;
    switch (prepare_class)
    {
        case PREPARE_CLASS_NORMAL:
            break;
        case PREPARE_CLASS_TIMESTAMP:
            error = TEST_OK_(timestamp_output == NULL,
                "Timestamp already specified");
            timestamp_output = field;
            break;
        case PREPARE_CLASS_TS_OFFSET:
            error = TEST_OK_(offset_output == NULL,
                "Timestamp already specified");
            offset_output = field;
            break;
        case PREPARE_CLASS_ADC_COUNT:
            error = TEST_OK_(adc_count_output == NULL,
                "Timestamp already specified");
            adc_count_output = field;
            break;
    }
    return error;
}


static struct output_field *create_output_field(
    struct output *output, unsigned int number, const char field_name[],
    const unsigned int capture_index[2])
{
    struct output_field *field = malloc(sizeof(struct output_field));
    *field = (struct output_field) {
        .output = output,
        .number = number,
        .field_name = strdup(field_name),
        .capture_index = { capture_index[0], capture_index[1], },
    };
    output_fields[output_field_count++] = field;
    return field;
}


error__t register_output(
    struct output *output, unsigned int number, const char field_name[],
    enum prepare_class prepare_class, const unsigned int capture_index[2])
{
    struct output_field *field;
    return
        TEST_OK_(output_field_count < CAPTURE_BUS_COUNT,
            "Too many capture fields specified!")  ?:
        DO(field = create_output_field(
            output, number, field_name, capture_index))  ?:
        process_special_field(prepare_class, field);
}


/* Makes a best effor stab at returning a list of fields currently configured
 * for capture. */
void report_capture_list(struct connection_result *result)
{
    for (unsigned int i = 0; i < output_field_count; i ++)
    {
        struct output_field *field = output_fields[i];
        const char *capture;
        if (get_capture_enabled(field->output, field->number, &capture))
            format_many_result(result, "%s %s", field->field_name, capture);
    }
    result->response = RESPONSE_MANY;
}


void reset_capture_list(void)
{
    for (unsigned int i = 0; i < output_field_count; i ++)
        reset_output_capture(
            output_fields[i]->output, output_fields[i]->number);
}


void report_capture_labels(struct connection_result *result)
{
    for (unsigned int i = 0; i < output_field_count; i ++)
        result->write_many(result->write_context, output_fields[i]->field_name);
    result->response = RESPONSE_MANY;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data capture request parsing. */

static error__t parse_one_option(
    const char *option, struct data_options *options)
{
    /* Data formatting options. */
    if (strcmp(option, "UNFRAMED") == 0)
        options->data_format = DATA_FORMAT_UNFRAMED;
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
            .data_format = DATA_FORMAT_UNFRAMED,
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
/* Header formatting. */


static void send_capture_info(
    struct buffered_file *file,
    const struct data_capture *capture, const struct data_options *options,
    uint64_t missed_samples)
{
    if (missed_samples)
        write_formatted_string(
            file, "Missed samples: %"PRIu64"\n", missed_samples);

    static const char *data_format_strings[] = {
        "Unframed", "Framed", "Base64", "ASCII", };
    static const char *data_process_strings[] = {
        "Raw", "Unscaled", "Scaled", };
    const char *data_format = data_format_strings[options->data_format];
    const char *data_process = data_process_strings[options->data_process];

    write_formatted_string(file, "Process: %s\n", data_process);
    write_formatted_string(file, "Transport: %s\n", data_format);
    if (options->data_format != DATA_FORMAT_ASCII)
        write_formatted_string(file, "Sample size (bytes): %zu\n",
            get_binary_sample_length(capture, options));
}


static void send_field_info(
    struct buffered_file *file, const struct output_field *field)
{
    write_formatted_string(file,
        " %s %s", field->field_name, field->info.capture_string);
    if (field->info.scaled)
        write_formatted_string(file,
            " Scaled: %.12g %.12g Units: %s",
            field->info.scaling.scale, field->info.scaling.offset,
            field->info.units);
    write_char(file, '\n');
}


static void send_group_info(
    struct buffered_file *file, const struct capture_group *group)
{
    for (unsigned int i = 0; i < group->count; i ++)
        send_field_info(file, group->outputs[i]);
}


bool send_data_header(
    const struct captured_fields *fields,
    const struct data_capture *capture,
    const struct data_options *options,
    struct buffered_file *file, uint64_t missed_samples)
{
    send_capture_info(file, capture, options, missed_samples);

    /* Format the field capture descriptions. */
    write_formatted_string(file, "Fields:\n");
    if (fields->ts_capture != TS_IGNORE)
        send_field_info(file, fields->timestamp);
    send_group_info(file, &fields->unscaled);
    send_group_info(file, &fields->scaled32);
    send_group_info(file, &fields->scaled64);
    send_group_info(file, &fields->adc_mean);

    write_char(file, '\n');
    return flush_out_buf(file);
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Output preparation. */


/* Called by data capture as part of initial preparation. */
enum framing_mode get_output_info(
    const struct output_field *output,
    unsigned int capture_index[2], struct scaling *scaling)
{
    capture_index[0] = output->capture_index[0];
    capture_index[1] = output->capture_index[1];
    *scaling = output->info.scaling;
    return output->info.framing_mode;
}


/* This structure is updated by calling prepare_captured_fields. */
static struct captured_fields captured_fields;


const struct captured_fields *prepare_captured_fields(void)
{
    captured_fields.ts_capture = TS_IGNORE;
    captured_fields.timestamp = timestamp_output;
    captured_fields.offset = offset_output;
    captured_fields.adc_count = adc_count_output;

    captured_fields.unscaled.count = 0;
    captured_fields.scaled32.count = 0;
    captured_fields.scaled64.count = 0;
    captured_fields.adc_mean.count = 0;

    /* Walk the list of outputs and gather them into their groups. */
    for (unsigned int i = 0; i < output_field_count; i ++)
    {
        struct output_field *output = output_fields[i];

        /* Fetch and store the current capture settings for this field. */
        enum capture_mode capture_mode = get_capture_info(
            output->output, output->number, &output->info);

        /* Dispatch output into the appropriate group for processing. */
        struct capture_group *capture = NULL;
        switch (capture_mode)
        {
            case CAPTURE_OFF:       break;
            case CAPTURE_UNSCALED:  capture = &captured_fields.unscaled; break;
            case CAPTURE_SCALED32:  capture = &captured_fields.scaled32; break;
            case CAPTURE_SCALED64:  capture = &captured_fields.scaled64; break;
            case CAPTURE_ADC_MEAN:  capture = &captured_fields.adc_mean; break;

            case CAPTURE_TS_NORMAL:
                captured_fields.ts_capture = TS_CAPTURE;
                break;
            case CAPTURE_TS_OFFSET:
                captured_fields.ts_capture = TS_OFFSET;
                break;
        }
        if (capture)
            capture->outputs[capture->count++] = output;
    }

    return &captured_fields;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation and shutdown. */

error__t initialise_prepare(void)
{
    /* Initialise the four capture groups with enough workspace. */
    size_t output_size = sizeof(struct output_field *) * CAPTURE_BUS_COUNT;
    captured_fields.unscaled.outputs = malloc(output_size);
    captured_fields.scaled32.outputs = malloc(output_size);
    captured_fields.scaled64.outputs = malloc(output_size);
    captured_fields.adc_mean.outputs = malloc(output_size);

    return
        /* Check that all of the fixed fields have been specified. */
        TEST_OK_(timestamp_output, "Timestamp field not specified")  ?:
        TEST_OK_(offset_output, "Timestamp offset field not specified")  ?:
        TEST_OK_(adc_count_output, "ADC count field not specified");
}


void terminate_prepare(void)
{
    free(captured_fields.unscaled.outputs);
    free(captured_fields.scaled32.outputs);
    free(captured_fields.scaled64.outputs);
    free(captured_fields.adc_mean.outputs);
    for (unsigned int i = 0; i < output_field_count; i ++)
    {
        free(output_fields[i]->field_name);
        free(output_fields[i]);
    }
}
