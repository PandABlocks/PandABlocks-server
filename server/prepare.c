/* Data capture preparation. */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include "error.h"
#include "parse.h"
#include "buffered_file.h"
#include "config_server.h"
#include "data_server.h"
#include "output.h"
#include "hardware.h"
#include "pos_out.h"
#include "ext_out.h"
#include "capture.h"

#include "prepare.h"



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data capture request parsing. */

static const struct data_options default_data_options = {
    .data_format = DATA_FORMAT_ASCII,
    .data_process = DATA_PROCESS_SCALED,
};

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
            .data_process = DATA_PROCESS_RAW,
            .omit_header = true,
            .omit_status = true,
            .one_shot = true,
        };
    else if (strcmp(option, "DEFAULT") == 0)
        *options = default_data_options;

    else
        return FAIL_("Invalid data capture option");
    return ERROR_OK;
}


error__t parse_data_options(const char *line, struct data_options *options)
{
    *options = default_data_options;

    char option[MAX_NAME_LENGTH];
    error__t error = ERROR_OK;
    while (!error  &&  *line)
        error =
            DO(line = skip_whitespace(line))  ?:
            TEST_OK_(*line != '\r', "Cannot process CR character")  ?:
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
    const struct capture_info *field, const struct data_options *options)
{
    static const char *field_type_names[][CAPTURE_MODE_COUNT] = {
        [DATA_PROCESS_RAW] = {
            [CAPTURE_MODE_SCALED32] = "int32",
            [CAPTURE_MODE_AVERAGE]  = "int64",
            [CAPTURE_MODE_STDDEV]   = "int96",
            [CAPTURE_MODE_SCALED64] = "int64",
            [CAPTURE_MODE_UNSCALED] = "uint32",
        },
        [DATA_PROCESS_SCALED] = {
            [CAPTURE_MODE_SCALED32] = "double",
            [CAPTURE_MODE_AVERAGE]  = "double",
            [CAPTURE_MODE_STDDEV]   = "double",
            [CAPTURE_MODE_SCALED64] = "double",
            [CAPTURE_MODE_UNSCALED] = "uint32",
        },
    };
    enum data_process process = options->data_process;
    enum capture_mode capture = field->capture_mode;
    return field_type_names[process][capture];
}


static void send_capture_info(
    struct buffered_file *file,
    const struct data_capture *capture, const struct data_options *options,
    uint64_t missed_samples, struct timespec *pcap_arm_tsp)
{
    static const char *data_format_strings[] = {
        [DATA_FORMAT_UNFRAMED] = "Unframed",
        [DATA_FORMAT_FRAMED]   = "Framed",
        [DATA_FORMAT_BASE64]   = "Base64",
        [DATA_FORMAT_ASCII]    = "ASCII",
    };
    static const char *data_process_strings[] = {
        [DATA_PROCESS_RAW]      = "Raw",
        [DATA_PROCESS_SCALED]   = "Scaled",
    };
    const char *data_format = data_format_strings[options->data_format];
    const char *data_process = data_process_strings[options->data_process];

    struct xml_element element =
        start_element(file, "data", options->xml_header, false, true);

    struct tm tm;
    char timestamp_message[MAX_RESULT_LENGTH];
    gmtime_r(&(pcap_arm_tsp->tv_sec), &tm);
    snprintf(timestamp_message, sizeof(timestamp_message),
        "%4d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec, pcap_arm_tsp->tv_nsec / 1000000);

    format_attribute(&element, "arm_time", "%s", timestamp_message);
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
    const struct capture_info *field)
{
    struct xml_element element =
        start_element(file, "field", options->xml_header, false, false);

    format_attribute_opt(&element, false,
        "name", "%s", field->field_name);
    format_attribute_opt(&element, false,
        "type", "%s", field_type_name(field, options));
    format_attribute_opt(&element, false,
        "capture", "%s", field->capture_string);

    if (field->capture_mode != CAPTURE_MODE_UNSCALED)
    {
        format_attribute(&element,
            "scale", "%.12g", field->scale);
        format_attribute(&element,
            "offset", "%.12g", field->offset);
        format_attribute_string(&element,
            "units", field->units);
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


static void send_stddev_group_info(
    struct buffered_file *file, const struct data_options *options,
    const struct capture_group *group)
{
    for (unsigned int i = 0; i < group->count; i ++)
    {
        /* Quick and dirty hack, fake up a new field with the properties we
         * need. */
        struct capture_info mean_info = *group->outputs[i];
        mean_info.capture_mode = CAPTURE_MODE_AVERAGE;
        mean_info.capture_string = "Mean";
        send_field_info(file, options, &mean_info);
        send_field_info(file, options, group->outputs[i]);
    }
}


bool send_data_header(
    const struct captured_fields *fields,
    const struct data_capture *capture,
    const struct data_options *options,
    struct buffered_file *file, uint64_t missed_samples,
    struct timespec *pcap_arm_tsp)
{
    struct xml_element header =
        start_element(file, "header", options->xml_header, true, true);

    send_capture_info(file, capture, options, missed_samples, pcap_arm_tsp);

    /* Format the field capture descriptions. */
    struct xml_element field_group =
        start_element(file, "fields", options->xml_header, true, false);

    /* In RAW mode we might have an anonymous sample count field to publish */
    bool raw_process = options->data_process == DATA_PROCESS_RAW;
    if (raw_process  &&  sample_count_is_anonymous(capture))
        send_field_info(file, options, fields->sample_count);

    send_group_info(file, options, &fields->unscaled);
    send_group_info(file, options, &fields->scaled32);
    send_group_info(file, options, &fields->scaled64);
    send_group_info(file, options, &fields->averaged);
    if (raw_process)
        /* Special header for std dev. */
        send_stddev_group_info(file, options, &fields->std_dev);
    else
        send_group_info(file, options, &fields->std_dev);

    end_element(&field_group);
    end_element(&header);

    write_char(file, '\n');
    return flush_out_buf(file);
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Output preparation. */


/* This structure is updated by calling prepare_captured_fields and is consumed
 * by prepare_data_capture() in capture.c.  We pre-allocate enough space for all
 * fields to be full, which cannot happen, but the numbers involved are not
 * extravagant. */
static struct captured_fields captured_fields = {
    .sample_count = (struct capture_info[1]) { },
    .scaled32 = { .outputs = (struct capture_info *[MAX_CAPTURE_COUNT]) { } },
    .averaged = { .outputs = (struct capture_info *[MAX_CAPTURE_COUNT]) { } },
    .std_dev  = { .outputs = (struct capture_info *[MAX_CAPTURE_COUNT]) { } },
    .scaled64 = { .outputs = (struct capture_info *[MAX_CAPTURE_COUNT]) { } },
    .unscaled = { .outputs = (struct capture_info *[MAX_CAPTURE_COUNT]) { } },
};

/* Capture info for each field is held in this area. */
static struct capture_info capture_info_buffer[MAX_CAPTURE_COUNT];


static struct capture_group *get_capture_group(enum capture_mode capture_mode)
{
    switch (capture_mode)
    {
        case CAPTURE_MODE_SCALED32:
             return &captured_fields.scaled32;
        case CAPTURE_MODE_AVERAGE:
             return &captured_fields.averaged;
        case CAPTURE_MODE_STDDEV:
             return &captured_fields.std_dev;
        case CAPTURE_MODE_SCALED64:
             return &captured_fields.scaled64;
        case CAPTURE_MODE_UNSCALED:
             return &captured_fields.unscaled;
        default:
            ASSERT_FAIL();
    }
}


const struct captured_fields *prepare_captured_fields(void)
{
    captured_fields.scaled32.count = 0;
    captured_fields.averaged.count = 0;
    captured_fields.std_dev.count = 0;
    captured_fields.scaled64.count = 0;
    captured_fields.unscaled.count = 0;

    /* Populate the sample_count extension bus field.  This is treated specially
     * as it is needed for averaging operations. */
    get_samples_capture_info(captured_fields.sample_count);

    /* Walk the list of outputs and gather them into their groups. */
    unsigned int ix = 0;
    unsigned int captured;
    struct capture_info *capture_info = capture_info_buffer;
    while (iterate_captured_values(&ix, &captured, capture_info))
    {
        for (unsigned int i = 0; i < captured; i ++)
        {
            struct capture_group *capture =
                get_capture_group(capture_info->capture_mode);
            capture->outputs[capture->count++] = capture_info;
            capture_info += 1;
        }
    }

    return &captured_fields;
}
