/* Support for types. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "hashtable.h"
#include "parse.h"
#include "mux_lookup.h"

#include "types.h"


struct type {
    const char *name;

    /* This creates and initialises any type specific data needed. */
    void *(*init_type)(unsigned int count);

    /* This converts a string to a writeable integer. */
    error__t (*parse)(
        const struct type_context *context,
        const char *string, unsigned int *value);

    /* This formats the value into a string according to the type rules. */
    error__t (*format)(
        const struct type_context *context,
        unsigned int value, char string[], size_t length);
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Multiplexer selector type. */

/* Converts register name to multiplexer name, or returns a placeholder if an
 * invalid value is read. */
static error__t mux_format(
    struct mux_lookup *mux_lookup,
    const struct type_context *context,
    unsigned int value, char result[], size_t length)
{
    const char *string;
    return
        mux_lookup_index(mux_lookup, value, &string)  ?:
        DO(strncpy(result, string, length));
}

static error__t bit_mux_format(
    const struct type_context *context,
    unsigned int value, char result[], size_t length)
{
    // we should be able to put bit_mux_lookup in the context info
    return mux_format(bit_mux_lookup, context, value, result, length);
}

static error__t pos_mux_format(
    const struct type_context *context,
    unsigned int value, char result[], size_t length)
{
    return mux_format(pos_mux_lookup, context, value, result, length);
}


/* Converts multiplexer output name to index and writes to register. */
static error__t mux_parse(
    struct mux_lookup *mux_lookup,
    const struct type_context *context,
    const char *string, unsigned int *value)
{
    return mux_lookup_name(mux_lookup, string, value);
}

static error__t bit_mux_parse(
    const struct type_context *context,
    const char *string, unsigned int *value)
{
    return mux_parse(bit_mux_lookup, context, string, value);
}

static error__t pos_mux_parse(
    const struct type_context *context,
    const char *string, unsigned int *value)
{
    return mux_parse(pos_mux_lookup, context, string, value);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Action types must have an empty write, and cannot be read. */

static error__t action_parse(
    const struct type_context *context, const char *string, unsigned int *value)
{
    *value = 0;
    return parse_eos(&string);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Integer type. */

static error__t uint32_parse(
    const struct type_context *context, const char *string, unsigned int *value)
{
    return
        parse_uint(&string, value)  ?:
        parse_eos(&string);
}


static error__t uint32_format(
    const struct type_context *context,
    unsigned int value, char string[], size_t length)
{
    snprintf(string, length, "%u", value);
    return ERROR_OK;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Type formatting API. */


/* This converts a string to a writeable integer. */
error__t type_parse(
    const struct type_context *context,
    const char *string, unsigned int *value)
{
    return
        TEST_OK_(context->type->parse,
            "Cannot write %s value", context->type->name)  ?:
        context->type->parse(context, string, value);
}


/* This formats the value into a string according to the type rules. */
error__t type_format(
    const struct type_context *context,
    unsigned int value, char string[], size_t length)
{
    return
        TEST_OK_(context->type->parse,
            "Cannot read %s value", context->type->name)  ?:
        context->type->format(context, value, string, length);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


static const struct type field_type_table[] = {
    { "uint32", .parse = uint32_parse, .format = uint32_format },
    { "int", .parse = uint32_parse, .format = uint32_format },
    { "bit", .parse = uint32_parse, .format = uint32_format },
    { "scaled_time", .parse = uint32_parse, .format = uint32_format },
    { "position", .parse = uint32_parse, .format = uint32_format },
    { "lut", .parse = uint32_parse, .format = uint32_format },
    { "bit_mux", .parse = bit_mux_parse, .format = bit_mux_format },
    { "pos_mux", .parse = pos_mux_parse, .format = pos_mux_format },
    { "action", .parse = action_parse, },
    { "table", },
    { "enum", .parse = uint32_parse, .format = uint32_format },
//     { "enum",   .parse = type_enum_parse,   .format = type_enum_format },
};

static struct hash_table *field_type_map;



error__t create_type(
    const char *name, const struct type **type, struct type_data **type_data)
{
    *type_data = NULL;
    return TEST_OK_(
        *type = hash_table_lookup(field_type_map, name),
        "Unknown field type %s", name);
}


void destroy_type(const struct type *type, struct type_data *type_data)
{
//     free(type_data);
}


error__t initialise_types(void)
{
    field_type_map = hash_table_create(false);
    for (unsigned int i = 0; i < ARRAY_SIZE(field_type_table); i ++)
    {
        const struct type *type = &field_type_table[i];
        hash_table_insert_const(field_type_map, type->name, type);
    }

    return ERROR_OK;
}


void terminate_types(void)
{
    if (field_type_map)
        hash_table_destroy(field_type_map);
}
