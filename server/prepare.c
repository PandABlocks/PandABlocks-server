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
    enum framing_mode framing_mode;     // Used to configure hardware framing
    struct scaling scaling;             // Scaling for fields with scaling
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


static bool capture_enabled(struct output_field *output)
{
    return get_capture_mode(output->output, output->number) != CAPTURE_OFF;
}


/* Makes a best effor stab at returning a list of fields currently configured
 * for capture. */
void report_capture_list(struct connection_result *result)
{
    for (unsigned int i = 0; i < output_field_count; i ++)
        if (capture_enabled(output_fields[i]))
            result->write_many(
                result->write_context, output_fields[i]->field_name);
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
/* Header formatting. */


bool send_data_header(
    const struct captured_fields *fields,
    const struct data_capture *capture,
    const struct data_options *options,
    struct buffered_file *file, uint64_t lost_samples)
{
    write_formatted_string(
        file, "header: lost %"PRIu64" samples\n", lost_samples);
    write_string(file, "header\n", 7);
    return flush_out_buf(file);
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Output preparation. */

enum framing_mode get_output_info(
    const struct output_field *output,
    unsigned int capture_index[2], struct scaling *scaling)
{
    capture_index[0] = output->capture_index[0];
    capture_index[1] = output->capture_index[1];
    *scaling = output->scaling;
    return output->framing_mode;
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
        enum capture_mode capture_mode =
            get_capture_mode(output->output, output->number);
        if (capture_mode != CAPTURE_OFF)
            output->framing_mode = get_capture_info(
                output->output, output->number, &output->scaling);

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
