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
    struct output *output, unsigned int number, const char *field_name,
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
    struct output *output, unsigned int number, const char *field_name,
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

    else if (strcmp(option, "XML") == 0)
        options->xml_header = true;

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
/* XML formatting support. */

struct xml_element {
    struct buffered_file *file;     // Destination for fields
    const char *name;               // Name of this element
    bool xml;                       // Whether to use XML format
    bool nested;                    // Determines <x></x> vs <x ... /x>
    bool hidden;                    // Makes outer parts hidden in text mode
};


/* Returns the string required to escape the given XML character. */
static const char *escape_xml_character(char ch)
{
    switch (ch)
    {
        case '<':   return "&lt;";
        case '&':   return "&amp;";
        case '>':   return "&gt;";
        case '"':   return "&quot;";
        case '\'':  return "&apos;";
        default:    return "";          // Mistake, don't do this
    }
}


/* Used for writing unconstrained strings whcy may contain any of the above
 * reserved XML characters. */
static void write_escaped_xml_string(
    struct buffered_file *file, const char *string)
{
    write_char(file, '"');
    while (*string)
    {
        /* Find length of segment up to any character which needs to be escaped.
         * We can just write this segment as is. */
        size_t accept = strcspn(string, "<&>\"'");
        if (accept > 0)
            write_string(file, string, accept);
        string += accept;

        /* Process the escape character if necessary. */
        if (*string)
        {
            const char *escape = escape_xml_character(*string);
            write_string(file, escape, strlen(escape));
            string += 1;
        }
    }
    write_char(file, '"');
}


static void __attribute__((format(printf, 4, 5))) format_attribute_opt(
    struct xml_element *element, bool use_name,
    const char *name, const char *format, ...)
{
    char value[MAX_RESULT_LENGTH];
    va_list args;
    va_start(args, format);
    vsnprintf(value, sizeof(value), format, args);
    va_end(args);

    if (element->xml)
        write_formatted_string(element->file, " %s=\"%s\"", name, value);
    else
    {
        if (!element->hidden)
            write_char(element->file, ' ');
        if (use_name)
            write_formatted_string(element->file, "%s: %s", name, value);
        else
            write_formatted_string(element->file, "%s", value);
        if (element->hidden)
            write_char(element->file, '\n');
    }
}

#define format_attribute(element, name, format...) \
    format_attribute_opt(element, true, name, format)


static void format_attribute_string(
    struct xml_element *element, const char *name, const char *value)
{
    if (element->xml)
    {
        write_formatted_string(element->file, " %s=", name);
        write_escaped_xml_string(element->file, value);
    }
    else
        write_formatted_string(element->file, " %s: %s", name, value);
}


static struct xml_element start_element(
    struct buffered_file *file, const char *name,
    bool xml, bool nested, bool hidden)
{
    struct xml_element element = {
        .file = file,
        .name = name,
        .xml = xml,
        .nested = nested,
        .hidden = hidden,
    };
    if (xml)
    {
        write_formatted_string(file, "<%s", name);
        if (nested)
            write_formatted_string(file, ">\n");
    }
    else if (!hidden  &&  nested)
        write_formatted_string(file, "%s:\n", name);
    return element;
}


static void end_element(struct xml_element *element)
{
    if (element->xml)
    {
        if (element->nested)
            write_formatted_string(element->file, "</%s>\n", element->name);
        else
            write_formatted_string(element->file, " />\n");
    }
    else if (!element->hidden  &&  !element->nested)
        write_char(element->file, '\n');
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Header formatting. */


/* Returns string specifying formatting of given field. */
static const char *field_type_name(
    const struct output_field *field, const struct data_options *options)
{
    static const char *field_type_names[][CAPTURE_MODE_COUNT] = {
        [DATA_PROCESS_RAW] = {
            [CAPTURE_UNSCALED] = "uint32",
            [CAPTURE_SCALED32] = "int32",
            [CAPTURE_SCALED64] = "int64",
            [CAPTURE_ADC_MEAN] = "int64",
            [CAPTURE_TS_NORMAL ... CAPTURE_TS_OFFSET] = "uint64",
        },
        [DATA_PROCESS_UNSCALED] = {
            [CAPTURE_UNSCALED] = "uint32",
            [CAPTURE_SCALED32] = "int32",
            [CAPTURE_SCALED64] = "int32",
            [CAPTURE_ADC_MEAN] = "int32",
            [CAPTURE_TS_NORMAL ... CAPTURE_TS_OFFSET] = "uint64",
        },
        [DATA_PROCESS_SCALED] = {
            [CAPTURE_UNSCALED] = "uint32",
            [CAPTURE_SCALED32] = "double",
            [CAPTURE_SCALED64] = "double",
            [CAPTURE_ADC_MEAN] = "double",
            [CAPTURE_TS_NORMAL ... CAPTURE_TS_OFFSET] = "double",
        },
    };
    enum data_process process = options->data_process;
    enum capture_mode capture = field->info.capture_mode;
    return field_type_names[process][capture];
}


static void send_capture_info(
    struct buffered_file *file,
    const struct data_capture *capture, const struct data_options *options,
    uint64_t missed_samples)
{
    static const char *data_format_strings[] = {
        [DATA_FORMAT_UNFRAMED] = "Unframed",
        [DATA_FORMAT_FRAMED]   = "Framed",
        [DATA_FORMAT_BASE64]   = "Base64",
        [DATA_FORMAT_ASCII]    = "ASCII",
    };
    static const char *data_process_strings[] = {
        [DATA_PROCESS_RAW]      = "Raw",
        [DATA_PROCESS_UNSCALED] = "Unscaled",
        [DATA_PROCESS_SCALED]   = "Scaled",
    };
    const char *data_format = data_format_strings[options->data_format];
    const char *data_process = data_process_strings[options->data_process];

    struct xml_element element =
        start_element(file, "data", options->xml_header, false, true);
    format_attribute(&element, "missed", "%"PRIu64, missed_samples);
    format_attribute(&element, "process", "%s", data_process);
    format_attribute(&element, "format", "%s", data_format);
    if (options->data_format != DATA_FORMAT_ASCII)
        format_attribute(&element, "sample_bytes", "%zu",
            get_binary_sample_length(capture, options));
    end_element(&element);
}


static void send_field_info(
    struct buffered_file *file, const struct data_options *options,
    const struct output_field *field)
{
    struct xml_element element =
        start_element(file, "field", options->xml_header, false, false);

    format_attribute_opt(&element, false,
        "name", "%s", field->field_name);
    format_attribute_opt(&element, false,
        "type", "%s", field_type_name(field, options));
    format_attribute_opt(&element, false,
        "capture", "%s", field->info.capture_string);

    if (field->info.scaled)
    {
        format_attribute(&element,
            "scale", "%.12g", field->info.scaling.scale);
        format_attribute(&element,
            "offset", "%.12g", field->info.scaling.offset);
        format_attribute_string(&element,
            "units", field->info.units);
    }

    end_element(&element);
}


static void send_group_info(
    struct buffered_file *file, const struct data_options *options,
    const struct capture_group *group)
{
    for (unsigned int i = 0; i < group->count; i ++)
        send_field_info(file, options, group->outputs[i]);
}


bool send_data_header(
    const struct captured_fields *fields,
    const struct data_capture *capture,
    const struct data_options *options,
    struct buffered_file *file, uint64_t missed_samples)
{
    struct xml_element header =
        start_element(file, "header", options->xml_header, true, true);

    send_capture_info(file, capture, options, missed_samples);

    /* Format the field capture descriptions. */
    struct xml_element field_group =
        start_element(file, "fields", options->xml_header, true, false);

    if (fields->ts_capture != TS_IGNORE)
        send_field_info(file, options, fields->timestamp);
    send_group_info(file, options, &fields->unscaled);
    send_group_info(file, options, &fields->scaled32);
    send_group_info(file, options, &fields->scaled64);
    send_group_info(file, options, &fields->adc_mean);

    end_element(&field_group);
    end_element(&header);

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
        enum capture_mode capture_mode =
            get_capture_info(output->output, output->number, &output->info);

        /* Dispatch output into the appropriate group for processing. */
        struct capture_group *capture = NULL;
        switch (capture_mode)
        {
            default:
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
