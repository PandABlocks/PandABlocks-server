/* Support for types. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <errno.h>

#include "error.h"
#include "hashtable.h"
#include "parse.h"
#include "mux_lookup.h"
#include "config_server.h"
#include "fields.h"

#include "types.h"



struct attr {
    /* Name of this attribute. */
    const char *name;

    /* Reads attribute value. */
    error__t (*get)(
        const struct type_attr_context *context,
        const struct connection_result *result);

    /* Writes attribute value. */
    error__t (*put)(const struct type_attr_context *context, const char *value);

    /* Context for shared attribute data. */
    void *context;
};


struct type {
    const char *name;
    bool tied;          // Set for types exclusive to specific classes

    /* This creates and initialises any type specific data needed. */
    error__t (*init)(const char **string, unsigned int count, void **type_data);
    /* By default type_data will be freed on destruction.  This optional method
     * implements any more complex destruction process needed. */
    void (*destroy)(void *type_data, unsigned int count);

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



/* Some type operations need to be done under a lock, alas. */
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()      ASSERT_PTHREAD(pthread_mutex_lock(&session_lock))
#define UNLOCK()    ASSERT_PTHREAD(pthread_mutex_unlock(&session_lock))


static error__t copy_string(char result[], size_t length, const char *value)
{
    return
        TEST_OK_(strlen(value) < length, "Result too long")  ?:
        DO(strcpy(result, value));
}


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
        copy_string(result, length, string);
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
/* Bit type: input can only be 0 or 1. */

static error__t bit_parse(
    const struct type_context *context, const char *string, unsigned int *value)
{
    return
        TEST_OK_(strchr("01", *string), "Invalid bit value")  ?:
        DO(*value = *string++ == '1')  ?:
        parse_eos(&string);
}


static error__t bit_format(
    const struct type_context *context,
    unsigned int value, char string[], size_t length)
{
    snprintf(string, length, "%s", value ? "1" : "0");
    return ERROR_OK;
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
/* Position and Scaled Time. */

struct position_state {
    double scale;
    double offset;
    char *units;
};


static error__t position_init(
    const char **string, unsigned int count, void **type_data)
{
    struct position_state *state = calloc(count, sizeof(struct position_state));
    for (unsigned int i = 0; i < count; i ++)
        state[i].scale = 1.0;
    *type_data = state;
    return ERROR_OK;
}

static void position_destroy(void *type_data, unsigned int count)
{
    struct position_state *state = type_data;
    for (unsigned int i = 0; i < count; i ++)
        free(state[i].units);
    free(type_data);
}


static error__t position_parse(
    const struct type_context *context, const char *string, unsigned int *value)
{
    struct position_state *state = context->type_data;
    state = &state[context->number];

    double position;
    double converted;
    return
        parse_double(&string, &position)  ?:
        parse_eos(&string)  ?:
        DO(converted = (position - state->offset) / state->scale)  ?:
        TEST_OK_(INT_MIN <= converted  &&  converted <= INT_MAX,
            "Position out of range")  ?:
        DO(*value = (unsigned int) lround(converted));
}


/* Alas the double formatting rules are ill mannered, in particular I don't want
 * to allow leading spaces.  I'd also love to prune trailing zeros, but we'll
 * see. */
static const char *format_double(char *buffer, double value)
{
    sprintf(buffer, "%12g", value);
    return skip_whitespace(buffer);
}


static error__t position_format(
    const struct type_context *context,
    unsigned int value, char string[], size_t length)
{
    struct position_state *state = context->type_data;
    state = &state[context->number];

    char buffer[MAX_RESULT_LENGTH];
    const char *output = format_double(buffer,
        (int) value * state->scale + state->offset);
    return copy_string(string, length, output);
}


static error__t position_raw_get(
    const struct type_attr_context *context,
    const struct connection_result *result)
{
    char string[MAX_RESULT_LENGTH];
    sprintf(string, "%d",
        (int) read_field_register(context->field, context->number));
    result->write_one(context->connection, string);
    return ERROR_OK;
}


static error__t position_scale_get(
    const struct type_attr_context *context,
    const struct connection_result *result)
{
    struct position_state *state = context->type_data;
    state = &state[context->number];
    char string[MAX_RESULT_LENGTH];
    result->write_one(context->connection, format_double(string, state->scale));
    return ERROR_OK;
}

static error__t position_scale_put(
    const struct type_attr_context *context, const char *value)
{
    struct position_state *state = context->type_data;
    state = &state[context->number];
    return
        parse_double(&value, &state->scale)  ?:
        parse_eos(&value);
}

static error__t position_offset_get(
    const struct type_attr_context *context,
    const struct connection_result *result)
{
    struct position_state *state = context->type_data;
    state = &state[context->number];
    char string[MAX_RESULT_LENGTH];
    result->write_one(
        context->connection, format_double(string, state->offset));
    return ERROR_OK;
}

static error__t position_offset_put(
    const struct type_attr_context *context, const char *value)
{
    struct position_state *state = context->type_data;
    state = &state[context->number];
    return
        parse_double(&value, &state->offset)  ?:
        parse_eos(&value);
}


static error__t position_units_get(
    const struct type_attr_context *context,
    const struct connection_result *result)
{
    struct position_state *state = context->type_data;
    state = &state[context->number];

    LOCK();
    char string[MAX_RESULT_LENGTH];
    error__t error = copy_string(string, sizeof(string), state->units ?: "");
    UNLOCK();

    return
        error  ?:
        DO(result->write_one(context->connection, string));
}

static error__t position_units_put(
    const struct type_attr_context *context, const char *value)
{
    struct position_state *state = context->type_data;
    state = &state[context->number];

    LOCK();
    free(state->units);
    state->units = strdup(value);
    UNLOCK();

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
    /* Unsigned integer with optional maximum limit. */
    { "uint",
        .init = uint_init,
        .parse = uint_parse, .format = uint_format,
        .attrs = (struct attr[]) { { "MAX", .get = uint_max_get, }, },
        .attr_count = 1,
    },

    /* Bits are simple: 0 or 1. */
    { "bit", .parse = bit_parse, .format = bit_format },

    /* Scaled time and position are very similar, both convert between a
     * floating point representation and a digital hardware value. */
    { "position",
        .init = position_init, .destroy = position_destroy,
        .parse = position_parse, .format = position_format,
        .attrs = (struct attr[]) {
            { "RAW", .get = position_raw_get, },
            { "SCALE",
                .get = position_scale_get, .put = position_scale_put },
            { "OFFSET",
                .get = position_offset_get, .put = position_offset_put },
            { "UNITS",
                .get = position_units_get, .put = position_units_put }, },
        .attr_count = 4 },
    { "scaled_time",
        .init = position_init, .destroy = position_destroy,
        .parse = position_parse, .format = position_format,
        .attrs = (struct attr[]) {
            { "RAW", .get = position_raw_get, },
            { "SCALE", .get = position_scale_get, .put = position_scale_put },
            { "UNITS",
                .get = position_units_get, .put = position_units_put }, },
        .attr_count = 3 },

    /* The mux types are only valid for bit_in and pos_in classes. */
    { "bit_mux", .tied = true,
      .parse = bit_mux_parse, .format = bit_mux_format },
    { "pos_mux", .tied = true,
      .parse = pos_mux_parse, .format = pos_mux_format },

    /* A type for fields where the data is never read and only the action of
     * writing is important: no data allowed. */
    { "action", .parse = action_parse, },

    /* 5-input lookup table with special parsing. */
    { "lut", .parse = dummy_parse, .format = dummy_format },

    /* Enumerations. */
    { "enum", .parse = dummy_parse, .format = dummy_format },

    /* The table type is probably just a placeholder, only suitable for the
     * table class. */
    { "table", .tied = true, },
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


void destroy_type(const struct type *type, void *type_data, unsigned int count)
{
    if (type->destroy)
        type->destroy(type_data, count);
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
