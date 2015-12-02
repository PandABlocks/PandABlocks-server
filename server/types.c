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

#include "error.h"
#include "hashtable.h"
#include "parse.h"
#include "config_server.h"
#include "parse_lut.h"
#include "attributes.h"
#include "enums.h"
#include "capture.h"

#include "types.h"



struct type_methods {
    const char *name;

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

    /* Type specific attributes. */
    const struct attr_methods *attrs;
    unsigned int attr_count;
};


struct type {
    const struct type_methods *methods;
    const struct register_methods *reg;
    void *reg_data;
    unsigned int count;
    void *type_data;
};


/*****************************************************************************/
/* Some support functions. */

/* Some type operations need to be done under a lock, alas. */
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()      ASSERT_PTHREAD(pthread_mutex_lock(&session_lock))
#define UNLOCK()    ASSERT_PTHREAD(pthread_mutex_unlock(&session_lock))


static error__t read_register(
    struct type *type, unsigned int number, uint32_t *value)
{
    return
        TEST_OK_(type->reg->read, "Register cannot be read")  ?:
        DO(*value = type->reg->read(type->reg_data, number));
}


static error__t write_register(
    struct type *type, unsigned int number, uint32_t value)
{
    return
        TEST_OK_(type->reg->write, "Register cannot be written")  ?:
        DO(type->reg->write(type->reg_data, number, value));
}


static void changed_register(struct type *type, unsigned int number)
{
    if (type->reg->changed)
        type->reg->changed(type->reg_data, number);
}



/*****************************************************************************/
/* Individual type implementations. */


/* Raw field implementation for those fields that need it. */

static error__t raw_format_int(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct type *type = owner;
    uint32_t value;
    return
        read_register(type, number, &value)  ?:
        format_string(result, length, "%d", value);
}

static error__t raw_format_uint(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct type *type = owner;
    uint32_t value;
    return
        read_register(type, number, &value)  ?:
        format_string(result, length, "%u", value);
}


static error__t raw_put_uint(
    void *owner, void *data, unsigned int number, const char *string)
{
    struct type *type = owner;
    unsigned int value;
    return
        parse_uint(&string, &value)  ?:
        parse_eos(&string)  ?:
        write_register(type, number, value);
}

static error__t raw_put_int(
    void *owner, void *data, unsigned int number, const char *string)
{
    struct type *type = owner;
    int value;
    return
        parse_int(&string, &value)  ?:
        parse_eos(&string)  ?:
        write_register(type, number, (uint32_t) value);
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
    return format_string(string, length, "%u", value);
}


static error__t uint_max_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    unsigned int *max_value = data;
    return format_string(result, length, "%u", *max_value);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Bit type: input can only be 0 or 1. */

static error__t bit_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
{
    bool result;
    return
        parse_bit(&string, &result)  ?:
        parse_eos(&string)  ?:
        DO(*value = result);
}


static error__t bit_format(
    void *type_data, unsigned int number,
    unsigned int value, char string[], size_t length)
{
    return format_string(string, length, "%s", value ? "1" : "0");
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
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct position_state *state = data;
    state = &state[number];
    return format_double(result, length, state->scale);
}

static error__t position_scale_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct position_state *state = data;
    state = &state[number];
    double scale;
    return
        parse_double(&value, &scale)  ?:
        parse_eos(&value)  ?:
        DO(
            state->scale = scale;
            changed_register(owner, number));
}

static error__t position_offset_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct position_state *state = data;
    state = &state[number];
    return format_double(result, length, state->offset);
}

static error__t position_offset_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct position_state *state = data;
    state = &state[number];
    double offset;
    return
        parse_double(&value, &offset)  ?:
        parse_eos(&value)  ?:
        DO(
            state->offset = offset;
            changed_register(owner, number));
}


static error__t position_units_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct position_state *state = data;
    state = &state[number];
    LOCK();
    error__t error = format_string(result, length, "%s", state->units ?: "");
    UNLOCK();
    return error;
}

static error__t position_units_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct position_state *state = data;
    state = &state[number];

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
        IF_ELSE(value == state->value  &&  state->string,
            format_string(string, length, "%s", state->string),
        //else
            format_string(string, length, "0x%08X", value));
    UNLOCK();
    return error;
}


/*****************************************************************************/
/* Type formatting API. */


/* Implements block[n].field=value */
error__t type_get(
    struct type *type, unsigned int number, struct connection_result *result)
{
    uint32_t value;
    return
        TEST_OK_(type->methods->format,
            "Cannot read %s value", type->methods->name)  ?:
        read_register(type, number, &value)  ?:
        type->methods->format(
            type->type_data, number, value, result->string, result->length)  ?:
        DO(result->response = RESPONSE_ONE);
}


/* Implements block[n].field? */
error__t type_put(struct type *type, unsigned int number, const char *string)
{
    uint32_t value;
    return
        TEST_OK_(type->methods->parse,
            "Cannot write %s value", type->methods->name)  ?:
        type->methods->parse(type->type_data, number, string, &value)  ?:
        write_register(type, number, value);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Type access helpers. */


const char *get_type_name(const struct type *type)
{
    return type->methods->name;
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


static const struct type_methods types_table[] = {
    /* Unsigned integer with optional maximum limit. */
    { "uint",
        .init = uint_init,
        .parse = uint_parse, .format = uint_format,
        .attrs = (struct attr_methods[]) {
            { "MAX", .format = uint_max_format, },
        },
        .attr_count = 1,
    },

    /* Bits are simple: 0 or 1. */
    { "bit", .parse = bit_parse, .format = bit_format },

    /* Scaled time and position are very similar, both convert between a
     * floating point representation and a digital hardware value. */
    { "position",
        .init = position_init, .destroy = position_destroy,
        .parse = position_parse, .format = position_format,
        .attrs = (struct attr_methods[]) {
            { "RAW",
                .format = raw_format_int, .put = raw_put_int, },
            { "SCALE", true,
                .format = position_scale_format, .put = position_scale_put, },
            { "OFFSET", true,
                .format = position_offset_format, .put = position_offset_put, },
            { "UNITS", true,
                .format = position_units_format, .put = position_units_put, },
        },
        .attr_count = 4,
    },

    /* The mux types are only valid for bit_in and pos_in classes. */
    { "bit_mux", .parse = bit_mux_parse, .format = bit_mux_format },
    { "pos_mux", .parse = pos_mux_parse, .format = pos_mux_format },

    /* A type for fields where the data is never read and only the action of
     * writing is important: no data allowed. */
    { "action", .parse = action_parse, },

    /* 5-input lookup table with special parsing. */
    { "lut",
        .init = lut_init, .destroy = lut_destroy,
        .parse = lut_parse, .format = lut_format,
        .attrs = (struct attr_methods[]) {
            { "RAW", .format = raw_format_uint, .put = raw_put_uint, }, },
        .attr_count = 1,
    },

    /* Enumerations. */
    { "enum",
        .init = enum_init, .destroy = enum_destroy,
        .add_attribute_line = enum_add_label,
        .parse = enum_parse, .format = enum_format,
        .attrs = (struct attr_methods[]) {
            { "RAW", .format = raw_format_uint, .put = raw_put_uint, },
            { "LABELS", .get_many = enum_labels_get, },
        },
        .attr_count = 2,
    },
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation. */

static error__t lookup_type(
    const char *name, const struct type_methods **result)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(types_table); i ++)
    {
        const struct type_methods *methods = &types_table[i];
        if (strcmp(name, methods->name) == 0)
        {
            *result = methods;
            return ERROR_OK;
        }
    }
    return FAIL_("Unknown field type %s", name);
}

static struct type *create_type_block(
    const struct type_methods *methods,
    const struct register_methods *reg, void *reg_data,
    unsigned int count, void *type_data)
{
    struct type *type = malloc(sizeof(struct type));
    *type = (struct type) {
        .methods = methods,
        .reg = reg,
        .reg_data = reg_data,
        .count = count,
        .type_data = type_data,
    };
    return type;
}

static void create_type_attributes(
    struct type *type, struct hash_table *attr_map)
{
    for (unsigned int i = 0; i < type->methods->attr_count; i ++)
        create_attribute(
            &type->methods->attrs[i], type, type->type_data, type->count,
            attr_map);
}

error__t create_type(
    const char **line, const char *default_type, unsigned int count,
    const struct register_methods *reg, void *reg_data,
    struct hash_table *attr_map, struct type **type)
{
    char type_name[MAX_NAME_LENGTH];
    const struct type_methods *methods = NULL;
    void *type_data = NULL;
    return
        /* If the line is empty fall back to the default type, if given. */
        IF_ELSE(**line == '\0'  &&  default_type,
            DO(line = &default_type),
        //else
            parse_whitespace(line))  ?:
        parse_name(line, type_name, sizeof(type_name))  ?:
        lookup_type(type_name, &methods)  ?:
        IF(methods->init,
            methods->init(line, count, &type_data))  ?:
        DO(
            *type = create_type_block(methods, reg, reg_data, count, type_data);
            create_type_attributes(*type, attr_map));
}
