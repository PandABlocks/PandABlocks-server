/* Support for types. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <errno.h>

#include "error.h"
#include "hashtable.h"
#include "parse.h"
#include "mux_lookup.h"
#include "fields.h"
#include "config_server.h"
#include "parse_lut.h"
#include "enums.h"

#include "types.h"



struct attr {
    /* Name of this attribute. */
    const char *name;

    error__t (*format)(
        const struct type_attr_context *context, char result[], size_t length);

    /* Reads attribute value.  Only need to implement this for multi-line
     * results, otherwise just implement format. */
    error__t (*get_many)(
        const struct type_attr_context *context,
        const struct connection_result *result);

    /* Writes attribute value. */
    error__t (*put)(const struct type_attr_context *context, const char *value);

    /* Context for shared attribute data. */
    void *context;
};


struct type_methods {
    const char *name;
    bool tied;          // Set for types exclusive to specific classes

    /* This creates and initialises any type specific data needed. */
    error__t (*init)(const char **string, unsigned int count, void **type_data);
    /* By default type_data will be freed on destruction.  This optional method
     * implements any more complex destruction process needed. */
    void (*destroy)(void *type_data, unsigned int count);

    /* This is called during startup to process an attribute line. */
    error__t (*add_attribute_line)(void *type_data, const char **string);

    /* This converts a string to a writeable integer. */
    error__t (*parse)(
        void *type_data, unsigned int number,
        const char *string, unsigned int *value);

    /* This formats the value into a string according to the type rules. */
    error__t (*format)(
        void *type_data, unsigned int number,
        unsigned int value, char string[], size_t length);

    const struct attr *attrs;
    unsigned int attr_count;
};


struct type {
    const struct type_methods *methods;
    unsigned int count;
    void *type_data;
};


/*****************************************************************************/
/* Some support functions. */

/* Some type operations need to be done under a lock, alas. */
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()      ASSERT_PTHREAD(pthread_mutex_lock(&session_lock))
#define UNLOCK()    ASSERT_PTHREAD(pthread_mutex_unlock(&session_lock))


static error__t __attribute__((format(printf, 3, 4))) format_string(
    char result[], size_t length, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vsnprintf(result, length, format, args);
    va_end(args);

    return TEST_OK_(written >= 0  &&  (size_t) written < length,
        "Result too long");
}



/*****************************************************************************/
/* Individual type implementations. */


/* Raw field implementation for those fields that need it. */

static error__t raw_format_int(
    const struct type_attr_context *context, char result[], size_t length)
{
    return FAIL_("Not implemented");
//     return format_string(result, length, "%d",
//         (int) read_field_register(context->field, context->number));
}

static error__t raw_format_uint(
    const struct type_attr_context *context, char result[], size_t length)
{
    return FAIL_("Not implemented");
//     return format_string(result, length, "%u",
//         read_field_register(context->field, context->number));
}


static error__t raw_put_uint(
    const struct type_attr_context *context, const char *string)
{
    return FAIL_("Not implemented");
//     unsigned int value;
//     return
//         parse_uint(&string, &value)  ?:
//         parse_eos(&string)  ?:
//         DO(write_field_register(context->field, context->number, value));
}

static error__t raw_put_int(
    const struct type_attr_context *context, const char *string)
{
    return FAIL_("Not implemented");
//     int value;
//     return
//         parse_int(&string, &value)  ?:
//         parse_eos(&string)  ?:
//         DO(write_field_register(
//             context->field, context->number, (unsigned int) value));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Multiplexer selector type. */

static error__t bit_mux_format(
    void *type_data, unsigned int number,
    unsigned int value, char result[], size_t length)
{
    return mux_lookup_index(bit_mux_lookup, value, result, length);
}

static error__t pos_mux_format(
    void *type_data, unsigned int number,
    unsigned int value, char result[], size_t length)
{
    return mux_lookup_index(pos_mux_lookup, value, result, length);
}


static error__t bit_mux_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
{
    return mux_lookup_name(bit_mux_lookup, string, value);
}

static error__t pos_mux_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
{
    return mux_lookup_name(pos_mux_lookup, string, value);
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
    *max_value = UINT_MAX;
    *type_data = max_value;
    return
        IF(read_char(string, ' '),
            parse_uint(string, max_value));
}


static error__t uint_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
{
    unsigned int *max_value = type_data;
    return
        parse_uint(&string, value)  ?:
        parse_eos(&string)  ?:
        TEST_OK_(*value <= *max_value, "Number out of range");
}


static error__t uint_format(
    void *type_data, unsigned int number,
    unsigned int value, char string[], size_t length)
{
    snprintf(string, length, "%u", value);
    return ERROR_OK;
}


static error__t uint_max_format(
    const struct type_attr_context *context, char result[], size_t length)
{
    unsigned int *max_value = context->type_data;
    return format_string(result, length, "%u", *max_value);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Bit type: input can only be 0 or 1. */

static error__t bit_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
{
    return
        TEST_OK_(strchr("01", *string), "Invalid bit value")  ?:
        DO(*value = *string++ == '1')  ?:
        parse_eos(&string);
}


static error__t bit_format(
    void *type_data, unsigned int number,
    unsigned int value, char string[], size_t length)
{
    snprintf(string, length, "%s", value ? "1" : "0");
    return ERROR_OK;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Action types must have an empty write, and cannot be read. */

static error__t action_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
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
}


static error__t position_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
{
    struct position_state *state = type_data;
    state = &state[number];

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
static error__t format_double(char result[], size_t length, double value)
{
    snprintf(result, length, "%12g", value);    // Not going to overflow
    const char *skip = skip_whitespace(result);
    if (skip > result)
        memmove(result, skip, strlen(skip) + 1);
    return ERROR_OK;
}


static error__t position_format(
    void *type_data, unsigned int number,
    unsigned int value, char result[], size_t length)
{
    struct position_state *state = type_data;
    state = &state[number];
    return format_double(
        result, length, (int) value * state->scale + state->offset);
}


static error__t position_scale_format(
    const struct type_attr_context *context, char result[], size_t length)
{
    struct position_state *state = context->type_data;
    state += context->number;
    return format_double(result, length, state->scale);
}

static error__t position_scale_put(
    const struct type_attr_context *context, const char *value)
{
    struct position_state *state = context->type_data;
    state += context->number;
    return
        parse_double(&value, &state->scale)  ?:
        parse_eos(&value);
}

static error__t position_offset_format(
    const struct type_attr_context *context, char result[], size_t length)
{
    struct position_state *state = context->type_data;
    state += context->number;
    return format_double(result, length, state->offset);
}

static error__t position_offset_put(
    const struct type_attr_context *context, const char *value)
{
    struct position_state *state = context->type_data;
    state += context->number;
    return
        parse_double(&value, &state->offset)  ?:
        parse_eos(&value);
}


static error__t position_units_format(
    const struct type_attr_context *context, char result[], size_t length)
{
    struct position_state *state = context->type_data;
    state += context->number;
    LOCK();
    error__t error = format_string(result, length, "%s", state->units ?: "");
    UNLOCK();
    return error;
}

static error__t position_units_put(
    const struct type_attr_context *context, const char *value)
{
    struct position_state *state = context->type_data;
    state += context->number;

    LOCK();
    free(state->units);
    state->units = strdup(value);
    UNLOCK();

    return ERROR_OK;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Lookup table type. */

struct lut_state {
    unsigned int value;
    char *string;
};


static error__t lut_init(
    const char **string, unsigned int count, void **type_data)
{
    struct lut_state *state = calloc(count, sizeof(struct lut_state));
    *type_data = state;
    return ERROR_OK;
}

static void lut_destroy(void *type_data, unsigned int count)
{
    struct lut_state *state = type_data;
    for (unsigned int i = 0; i < count; i ++)
        free(state[i].string);
}


static error__t do_parse_lut(const char *string, unsigned int *value)
{
    if (strncmp(string, "0x", 2) == 0)
    {
        /* String *must* be an eight digit hex number. */
        char *end;
        *value = (unsigned int) strtoul(string, &end, 16);
        return TEST_OK_(*end == '\0'  &&  strlen(string) > 2, "Bad LUT number");
    }
    else
    {
        enum parse_lut_status status = parse_lut(string, value);
        return TEST_OK_(status == LUT_PARSE_OK,
            "%s", parse_lut_error_string(status));
    }
}


static error__t lut_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
{
    struct lut_state *state = type_data;
    state += number;

    error__t error = do_parse_lut(string, value);
    if (!error)
    {
        LOCK();
        state->value = *value;
        free(state->string);
        state->string = strdup(string);
        UNLOCK();
    }
    return error;
}


static error__t lut_format(
    void *type_data, unsigned int number,
    unsigned int value, char string[], size_t length)
{
    struct lut_state *state = type_data;
    state += number;

    LOCK();
    error__t error =
        IF_ELSE(value == state->value,
            format_string(string, length, "%s", state->string),
        //else
            format_string(string, length, "0x%08X", value));
    UNLOCK();
    return error;
}


/*****************************************************************************/
/* Type formatting API. */


/* Implements block[n].field? */
error__t type_format(
    struct type *type, unsigned int number,
    unsigned int value, char string[], size_t length)
{
    return
        TEST_OK_(type->methods->parse,
            "Cannot read %s value", type->methods->name)  ?:
        type->methods->format(type->type_data, number, value, string, length);
}


/* Implements block[n].field=value */
error__t type_parse(
    struct type *type, unsigned int number,
    const char *string, unsigned int *value)
{
    return
        TEST_OK_(type->methods->parse,
            "Cannot write %s value", type->methods->name)  ?:
        type->methods->parse(type->type_data, number, string, value);
}


/* Implements block.field.*? */
void type_attr_list_get(
    const struct type *type,
    const struct connection_result *result)
{
    if (type->methods->attrs)
        for (unsigned int i = 0; i < type->methods->attr_count; i ++)
            result->write_many(
                result->connection, type->methods->attrs[i].name);
}


/* Implements block[n].field.attr? */
error__t type_attr_get(
    const struct type_attr_context *context,
    const struct connection_result *result)
{
    /* We have two possible implementations of field get: .format and .get_many.
     * If the .format field is available then we use that by preference. */
    if (context->attr->format)
    {
        char string[MAX_RESULT_LENGTH];
        return
            context->attr->format(context, string, sizeof(string))  ?:
            DO(result->write_one(result->connection, string));
    }
    else if (context->attr->get_many)
        return context->attr->get_many(context, result);
    else
        return FAIL_("Attribute not readable");
}


/* Implements block[n].field.attr=value */
error__t type_attr_put(
    const struct type_attr_context *context, const char *value)
{
    return
        TEST_OK_(context->attr->put, "Attribute not writeable")  ?:
        context->attr->put(context, value);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Type access helpers. */


/* Map from type name to type definition, filled in by initialiser. */
static struct hash_table *field_type_map;


const struct attr *type_lookup_attr(
    const struct type *type, const char *name)
{
    const struct attr *attrs = type->methods->attrs;
    if (attrs)
        for (unsigned int i = 0; i < type->methods->attr_count; i ++)
            if (strcmp(name, attrs[i].name) == 0)
                return &attrs[i];
    return NULL;
}


const char *get_type_name(const struct type *type)
{
    return type->methods->name;
}


static struct type *create_type_block(
    const struct type_methods *methods, unsigned int count, void *type_data)
{
    struct type *type = malloc(sizeof(struct type));
    *type = (struct type) {
        .methods = methods,
        .count = count,
        .type_data = type_data,
    };
    return type;
}

error__t create_type(
    const char **string, bool forced, unsigned int count, struct type **type)
{
    char type_name[MAX_NAME_LENGTH];
    const struct type_methods *methods;
    void *type_data = NULL;
    return
        parse_name(string, type_name, sizeof(type_name))  ?:
        TEST_OK_(
            methods = hash_table_lookup(field_type_map, type_name),
            "Unknown field type %s", type_name)  ?:
        TEST_OK_(!methods->tied  ||  forced,
            "Cannot use this type with this class")  ?:
        IF(methods->init,
            methods->init(string, count, &type_data))  ?:
        DO(*type = create_type_block(methods, count, type_data));
}


error__t type_parse_attribute(struct type *type, const char **line)
{
    return
        TEST_OK_(type->methods->add_attribute_line,
            "Cannot add attribute to type")  ?:
        type->methods->add_attribute_line(type->type_data, line);
}


void destroy_type(struct type *type)
{
    if (type->methods->destroy)
        type->methods->destroy(type->type_data, type->count);
    free(type->type_data);
    free(type);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


static const struct type_methods field_type_table[] = {
    /* Unsigned integer with optional maximum limit. */
    { "uint",
        .init = uint_init,
        .parse = uint_parse, .format = uint_format,
        .attrs = (struct attr[]) { { "MAX", .format = uint_max_format, }, },
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
            { "RAW",
                .format = raw_format_int, .put = raw_put_int, },
            { "SCALE",
                .format = position_scale_format, .put = position_scale_put, },
            { "OFFSET",
                .format = position_offset_format, .put = position_offset_put, },
            { "UNITS",
                .format = position_units_format, .put = position_units_put, },
        },
        .attr_count = 4,
    },
    { "scaled_time",
        .init = position_init, .destroy = position_destroy,
        .parse = position_parse, .format = position_format,
        .attrs = (struct attr[]) {
            { "RAW",
                .format = raw_format_int, .put = raw_put_int, },
            { "SCALE",
                .format = position_scale_format, .put = position_scale_put, },
            { "UNITS",
                .format = position_units_format, .put = position_units_put, },
        },
        .attr_count = 3,
    },

    /* The mux types are only valid for bit_in and pos_in classes. */
    { "bit_mux", .tied = true,
        .parse = bit_mux_parse, .format = bit_mux_format },
    { "pos_mux", .tied = true,
        .parse = pos_mux_parse, .format = pos_mux_format },

    /* A type for fields where the data is never read and only the action of
     * writing is important: no data allowed. */
    { "action", .parse = action_parse, },

    /* 5-input lookup table with special parsing. */
    { "lut",
        .init = lut_init, .destroy = lut_destroy,
        .parse = lut_parse, .format = lut_format,
        .attrs = (struct attr[]) {
            { "RAW", .format = raw_format_uint, .put = raw_put_uint, }, },
        .attr_count = 1,
    },

    /* Enumerations. */
    { "enum",
        .init = enum_init, .destroy = enum_destroy,
        .add_attribute_line = enum_add_label,
        .parse = enum_parse, .format = enum_format,
        .attrs = (struct attr[]) {
            { "RAW", .format = raw_format_uint, .put = raw_put_uint, },
            { "LABELS", .get_many = enum_labels_get, },
        },
        .attr_count = 2,
    },

    /* Implements table access. */
    { "table", .tied = true,
        .attrs = (struct attr[]) {
            { "LENGTH", },
            { "B", },
        },
        .attr_count = 2,
    },
};


error__t initialise_types(void)
{
    field_type_map = hash_table_create(false);
    for (unsigned int i = 0; i < ARRAY_SIZE(field_type_table); i ++)
    {
        const struct type_methods *methods = &field_type_table[i];
        hash_table_insert_const(field_type_map, methods->name, methods);
    }

    return ERROR_OK;
}


void terminate_types(void)
{
    if (field_type_map)
        hash_table_destroy(field_type_map);
}
