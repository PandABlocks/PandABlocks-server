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
#include "fields.h"
#include "attributes.h"
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
    unsigned int bit_group; // Only for ext_out bits <group>
    unsigned int registers[2];       // Up to two registers for timestamps
    bool capture;           // Set if data capture requested by user
};


static const struct enum_set ext_out_capture_enum_set = {
    .enums = (struct enum_entry[]) {
        { 0, "No", },
        { 1, "Capture", },
    },
    .count = 2,
};

static const struct enumeration *ext_out_capture_enum;


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


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

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
    };
    *class_data = ext_out;

    if (ext_type == EXT_OUT_BITS)
        create_attributes(
            &bits_attr_methods, 1, NULL, ext_out, 1, attr_map);
    return ERROR_OK;
}


static error__t ext_out_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data)
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


static error__t ext_out_parse_register(
    void *class_data, struct field *field, unsigned int block_base,
    const char **line)
{
    struct ext_out *ext_out = class_data;
    return
        parse_uint(line, &ext_out->registers[0])  ?:
        IF(ext_out->ext_type == EXT_OUT_TIMESTAMP,
            parse_whitespace(line)  ?:
            parse_uint(line, &ext_out->registers[1]));
        register_ext_out(ext_out, field)  ?:
        IF(ext_out->ext_type == EXT_OUT_BITS,
            DO(register_bit_group(field, ext_out)));
}


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
            "Not a valid capture option")  ?:
        DO(ext_out->capture = capture);
}


/*****************************************************************************/
/* Startup and shutdown. */


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


const struct class_methods ext_out_class_methods = {
    "ext_out",
    .init = ext_out_init,
    .parse_register = ext_out_parse_register,
    .describe = ext_out_describe,
    .attrs = (struct attr_methods[]) {
        { "CAPTURE", "Capture options",
            .in_change_set = true,
            .format = ext_out_capture_format, .put = ext_out_capture_put,
        },
    },
    .attr_count = 1,
};
