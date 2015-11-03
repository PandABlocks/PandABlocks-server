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
#include "config_server.h"

#include "types.h"



struct type_attr_context;

struct attr {
    const char *name;

    error__t (*get)(
        const struct type_attr_context *context,
        const struct connection_result *result);

    error__t (*put)(const struct type_attr_context *context, const char *value);
};


struct type {
    const char *name;
    bool tied;          // Set for types exclusive to specific classes

    /* This creates and initialises any type specific data needed. */
    error__t (*init)(const char **string, unsigned int count, void **type_data);
    /* By default type_data will be freed on destruction.  This optional method
     * implements any more complex destruction process needed. */
    void (*destroy)(void *type_data);

    /* This converts a string to a writeable integer. */
    error__t (*parse)(
        const struct type_context *context,
        const char *string, unsigned int *value);

    /* This formats the value into a string according to the type rules. */
    error__t (*format)(
        const struct type_context *context,
        unsigned int value, char string[], size_t length);

    const struct attr *attrs;
    unsigned int attr_count;
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
/* Unsigned integer type.
 *
 * This is created with an optional maximum value: if specified, valid values
 * are restricted to the specified range.  The maximum value can be read as an
 * attribute of this type. */

static error__t uint_init(
    const char **string, unsigned int count, void **type_data)
{
    unsigned int *max_value = malloc(sizeof(unsigned int));
    *max_value = 0xffffffffU;
    *type_data = max_value;
    return
        IF(read_char(string, ' '),
            parse_uint(string, max_value));
}


static error__t uint_parse(
    const struct type_context *context, const char *string, unsigned int *value)
{
    unsigned int *max_value = context->type_data;
    return
        parse_uint(&string, value)  ?:
        parse_eos(&string)  ?:
        TEST_OK_(*value <= *max_value, "Number out of range");
}


static error__t uint_format(
    const struct type_context *context,
    unsigned int value, char string[], size_t length)
{
    snprintf(string, length, "%u", value);
    return ERROR_OK;
}


static error__t uint_max_get(
    const struct type_attr_context *context,
    const struct connection_result *result)
{
    unsigned int *max_value = context->type_data;
    char string[MAX_RESULT_LENGTH];
    snprintf(string, sizeof(string), "%u", *max_value);
    result->write_one(context->connection, string);
    return ERROR_OK;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Dummy type. */

static error__t dummy_parse(
    const struct type_context *context, const char *string, unsigned int *value)
{
    *value = 0;
    return ERROR_OK;
}


static error__t dummy_format(
    const struct type_context *context,
    unsigned int value, char string[], size_t length)
{
    snprintf(string, length, "dummy");
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


error__t type_attr_list_get(
    const struct type *type,
    struct config_connection *connection,
    const struct connection_result *result)
{
    if (type->attrs)
        for (unsigned int i = 0; i < type->attr_count; i ++)
            result->write_many(connection, type->attrs[i].name);
    result->write_many_end(connection);
    return ERROR_OK;
}


error__t type_attr_get(
    const struct type_attr_context *context,
    const struct connection_result *result)
{
    return
        TEST_OK_(context->attr->get, "Attribute not readable")  ?:
        context->attr->get(context, result);
}


error__t type_attr_put(
    const struct type_attr_context *context, const char *value)
{
    return
        TEST_OK_(context->attr->put, "Attribute not writeable")  ?:
        context->attr->put(context, value);
}


error__t type_lookup_attr(
    const struct type *type, const char *name,
    const struct attr **attr)
{
printf("type_lookup_attr %s:%s\n", type->name, name);
    if (type->attrs)
        for (unsigned int i = 0; i < type->attr_count; i ++)
            if (strcmp(name, type->attrs[i].name) == 0)
            {
                *attr = &type->attrs[i];
                return ERROR_OK;
            }
    return FAIL_("No such attribute");
}


const char *type_get_type_name(const struct type *type)
{
    return type->name;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


static const struct type field_type_table[] = {
    { "uint",
        .init = uint_init,
        .parse = uint_parse, .format = uint_format,
        .attrs = (struct attr[]) { { "MAX", .get = uint_max_get, }, },
        .attr_count = 1,
    },
    { "int", .parse = dummy_parse, .format = dummy_format },
    { "bit", .parse = dummy_parse, .format = dummy_format },
    { "scaled_time", .parse = dummy_parse, .format = dummy_format },
    { "position", .parse = dummy_parse, .format = dummy_format },

    /* 5-input lookup table with special parsing. */
    { "lut", .parse = dummy_parse, .format = dummy_format },

    /* The mux types are only valid for bit_in and pos_in classes. */
    { "bit_mux", .tied = true,
      .parse = bit_mux_parse, .format = bit_mux_format },
    { "pos_mux", .tied = true,
      .parse = pos_mux_parse, .format = pos_mux_format },

    /* A type for fields where the data is never read and only the action of
     * writing is important: no data allowed. */
    { "action", .parse = action_parse, },

    /* The table type is probably just a placeholder, only suitable for the
     * table class. */
    { "table", .tied = true, },

    { "enum", .parse = dummy_parse, .format = dummy_format },
//     { "enum",   .parse = enum_parse,   .format = enum_format },
};

static struct hash_table *field_type_map;


error__t create_type(
    const char *string, bool forced, unsigned int count,
    const struct type **type, void **type_data)
{
    *type_data = NULL;
    char type_name[MAX_NAME_LENGTH];
    return
        parse_name(&string, type_name, sizeof(type_name))  ?:
        TEST_OK_(
            *type = hash_table_lookup(field_type_map, type_name),
            "Unknown field type %s", type_name)  ?:
        TEST_OK_(!(*type)->tied  ||  forced,
            "Cannot use this type with this class")  ?:
        IF((*type)->init,
            (*type)->init(&string, count, type_data))  ?:
        parse_eos(&string);
}


void destroy_type(const struct type *type, void *type_data)
{
    if (type->destroy)
        type->destroy(type_data);
    else
        free(type_data);
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
