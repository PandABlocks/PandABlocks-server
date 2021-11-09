/* Extension bus class definitions. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "hardware.h"
#include "hashtable.h"
#include "parse.h"
#include "config_server.h"
#include "data_server.h"
#include "attributes.h"
#include "fields.h"
#include "types.h"
#include "enums.h"
#include "bit_out.h"
#include "output.h"

#include "ext_out.h"



enum ext_out_type {
    EXT_OUT_TIMESTAMP,       // ext_out timestamp
    EXT_OUT_SAMPLES,         // ext_out samples
    EXT_OUT_BITS,            // ext_out bits <group>
};


struct ext_out {
    enum ext_out_type ext_type;
    unsigned int bit_group;     // Only for ext_out bits <group>
    unsigned int registers[2];  // Up to two registers for timestamps
    struct attr *capture_attr;
    bool capture;               // Set if data capture requested by user
};


static struct ext_out *samples_ext_out;
static char samples_field_name[MAX_NAME_LENGTH];


static error__t bits_get_many(
    void *owner, void *data, unsigned int number,
    struct connection_result *result)
{
    struct ext_out *ext_out = data;
    report_capture_bits(result, ext_out->bit_group);
    return ERROR_OK;
}


/* BITS attribute, only meaningful for ext_out bits. */
static const struct attr_methods bits_attr_methods = {
    "BITS", "Enumerate bits captured in this word",
    .get_many = bits_get_many,
};



static const char *ext_out_describe(void *class_data)
{
    static const char *description[] = {
        [EXT_OUT_TIMESTAMP] = "timestamp",
        [EXT_OUT_SAMPLES]   = "samples",
        [EXT_OUT_BITS]      = "bits",
    };

    struct ext_out *ext_out = class_data;
    return description[ext_out->ext_type];
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* CAPTURE attribute */

static const struct enum_set ext_out_capture_enum_set = {
    .enums = (struct enum_entry[]) {
        { 0, "No", },
        { 1, "Value", },
    },
    .count = 2,
};

static const struct enumeration *ext_out_capture_enum;


static error__t ext_out_capture_format(
    void *owner, void *data, unsigned int number, char result[], size_t length)
{
    struct ext_out *ext_out = data;
    return format_string(result, length, "%s",
        enum_index_to_name(ext_out_capture_enum, ext_out->capture));
}


static error__t ext_out_capture_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct ext_out *ext_out = data;
    unsigned int capture;
    return
        TEST_OK_(
            enum_name_to_index(ext_out_capture_enum, value, &capture),
            "Invalid capture option")  ?:
        DO(ext_out->capture = capture);
}


static const struct enumeration *ext_out_capture_get_enumeration(void *data)
{
    return ext_out_capture_enum;
}


void reset_ext_out_capture(struct ext_out *ext_out)
{
//     WITH_MUTEX(ext_out->mutex)
    if (ext_out->capture)
    {
        ext_out->capture = false;
        attr_changed(ext_out->capture_attr, 0);
    }
}


void report_ext_out_capture(
    struct ext_out *ext_out, const char *field_name,
    struct connection_result *result)
{
    if (ext_out->capture)
        format_many_result(result, "%s %s", field_name,
            ext_out_capture_enum_set.enums[1].name);
}


/* This attribute needs to be added separately so that we can hang onto the
 * attribute so that we can implement the reset_pos_out_capture method. */
static const struct attr_methods ext_out_capture_attr = {
    "CAPTURE", "Capture options",
    .in_change_set = true,
    .format = ext_out_capture_format, .put = ext_out_capture_put,
    .get_enumeration = ext_out_capture_get_enumeration,
};


/*****************************************************************************/
/* Field info. */


static enum capture_mode get_capture_mode(enum ext_out_type ext_type)
{
    switch (ext_type)
    {
        case EXT_OUT_TIMESTAMP: return CAPTURE_MODE_SCALED64;
        case EXT_OUT_SAMPLES:   return CAPTURE_MODE_UNSCALED;
        case EXT_OUT_BITS:      return CAPTURE_MODE_UNSCALED;
        default:
            ASSERT_FAIL();
    }
}


static void get_capture_info(
    struct ext_out *ext_out, struct capture_info *capture_info)
{
    *capture_info = (struct capture_info) {
        .capture_index = {
            .index = {
                CAPTURE_EXT_BUS(ext_out->registers[0]),
                CAPTURE_EXT_BUS(ext_out->registers[1]) },
        },
        .capture_mode = get_capture_mode(ext_out->ext_type),
        .capture_string = "Value",
        /* Scaling info only used for timestamp fields. */
        .scale = 1.0 / CLOCK_FREQUENCY,
        .offset = 0.0,
        .units = "s",
    };
}


unsigned int get_ext_out_capture_info(
    struct ext_out *ext_out, struct capture_info *capture_info)
{
    if (ext_out->capture)
    {
        get_capture_info(ext_out, capture_info);
        return 1;
    }
    else
        return 0;
}


bool get_samples_capture_info(struct capture_info *capture_info)
{
    ASSERT_OK(samples_ext_out);     // If not assigned, we are dead.
    get_capture_info(samples_ext_out, capture_info);
    capture_info->field_name = samples_field_name;
    return samples_ext_out->capture;
}


bool check_pcap_valid(void)
{
    return samples_ext_out;
}


/*****************************************************************************/
/* Startup and shutdown. */


/* This array of booleans is used to detect overlapping extra bus indexes. */
static bool ext_bus_index_used[EXT_BUS_COUNT];


/* An ext_out type is one of timestamp, samples or bits. */
static error__t parse_ext_out_type(
    const char **line, enum ext_out_type *ext_type, unsigned int *bit_group)
{
    char type_name[MAX_NAME_LENGTH];
    return
        parse_whitespace(line)  ?:
        parse_name(line, type_name, sizeof(type_name))  ?:

        IF_ELSE(strcmp(type_name, "timestamp") == 0,
            DO(*ext_type = EXT_OUT_TIMESTAMP),
        //else
        IF_ELSE(strcmp(type_name, "samples") == 0,
            DO(*ext_type = EXT_OUT_SAMPLES),
        //else
        IF_ELSE(strcmp(type_name, "bits") == 0,
            DO(*ext_type = EXT_OUT_BITS)  ?:
            parse_whitespace(line)  ?:
            parse_uint(line, bit_group),
        //else
        FAIL_("Unknown ext_out type"))));
}


static error__t create_ext_out(
    enum ext_out_type ext_type, unsigned int bit_group,
    struct hash_table *attr_map, void **class_data)
{
    struct ext_out *ext_out = malloc(sizeof(struct ext_out));
    *ext_out = (struct ext_out) {
        .ext_type = ext_type,
        .bit_group = bit_group,
        .capture_attr = add_one_attribute(
            &ext_out_capture_attr, NULL, ext_out, 1, attr_map),
    };
    *class_data = ext_out;

    if (ext_type == EXT_OUT_BITS)
        add_one_attribute(&bits_attr_methods, NULL, ext_out, 1, attr_map);
    return ERROR_OK;
}


static error__t ext_out_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data,
    struct indent_parser *parser)
{
    enum ext_out_type ext_type = 0;
    unsigned int bit_group = 0;

    return
        TEST_OK_(count == 1, "Cannot repeat extension field")  ?:
        parse_ext_out_type(line, &ext_type, &bit_group)  ?:
        create_ext_out(ext_type, bit_group, attr_map, class_data);
}


/* Let bit_out know where this group of bits can be captured. */
static void register_bit_group(struct field *field, struct ext_out *ext_out)
{
    char name[MAX_NAME_LENGTH];
    format_field_name(name, sizeof(name), field, NULL, 0, '\0');
    set_bit_group_name(ext_out->bit_group, name);
}


static error__t parse_register(const char **line, unsigned int *registers)
{
    unsigned int value;
    return
        parse_uint(line, &value)  ?:
        TEST_OK_(value < EXT_BUS_COUNT, "Extra index out of range")  ?:
        TEST_OK_(!ext_bus_index_used[value],
            "Extra index %u already used", value)  ?:
        DO( *registers = value;
            ext_bus_index_used[value] = true);
}


static error__t ext_out_parse_register(
    void *class_data, struct field *field, unsigned int block_base,
    const char **line)
{
    struct ext_out *ext_out = class_data;
    return
        parse_register(line, &ext_out->registers[0])  ?:
        IF(ext_out->ext_type == EXT_OUT_TIMESTAMP,
            parse_whitespace(line)  ?:
            parse_register(line, &ext_out->registers[1]))  ?:

        register_ext_out(ext_out, field)  ?:
        IF(ext_out->ext_type == EXT_OUT_SAMPLES,
            TEST_OK_(samples_ext_out == NULL,
                "Duplicate samples field assigned")  ?:
            DO(
                samples_ext_out = ext_out;
                format_field_name(
                    samples_field_name, sizeof(samples_field_name),
                    field, NULL, 0, '\0')
            ))  ?:
        IF(ext_out->ext_type == EXT_OUT_BITS,
            DO(register_bit_group(field, ext_out)));
}


error__t initialise_ext_out(void)
{
    ext_out_capture_enum = create_static_enumeration(&ext_out_capture_enum_set);
    return ERROR_OK;
}


void terminate_ext_out(void)
{
    if (ext_out_capture_enum)
        destroy_enumeration(ext_out_capture_enum);
}


/*****************************************************************************/

const struct class_methods ext_out_class_methods = {
    "ext_out",
    .init = ext_out_init,
    .parse_register = ext_out_parse_register,
    .describe = ext_out_describe,
    // "CAPTURE" attribute initialised separately
};
