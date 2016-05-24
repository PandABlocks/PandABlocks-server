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
#include "fields.h"
#include "output.h"
#include "bit_out.h"
#include "locking.h"
#include "time_position.h"

#include "types.h"



struct type {
    const struct type_methods *methods;
    const struct register_methods *reg;
    void *reg_data;
    unsigned int count;
    void *type_data;
};


/*****************************************************************************/
/* Some support functions. */


error__t read_type_register(
    struct type *type, unsigned int number, uint32_t *value)
{
    return
        TEST_OK_(type->reg->read, "Register cannot be read")  ?:
        type->reg->read(type->reg_data, number, value);
}


error__t write_type_register(
    struct type *type, unsigned int number, uint32_t value)
{
    return
        TEST_OK_(type->reg->write, "Register cannot be written")  ?:
        type->reg->write(type->reg_data, number, value);
}


void changed_type_register(struct type *type, unsigned int number)
{
    if (type->reg->changed)
        type->reg->changed(type->reg_data, number);
}


/* Raw field implementation for those fields that need it. */

error__t raw_format_uint(
    void *owner, void *data, unsigned int number, char result[], size_t length)
{
    struct type *type = owner;
    uint32_t value;
    return
        read_type_register(type, number, &value)  ?:
        format_string(result, length, "%u", value);
}


error__t raw_put_uint(
    void *owner, void *data, unsigned int number, const char *string)
{
    struct type *type = owner;
    unsigned int value;
    return
        parse_uint(&string, &value)  ?:
        parse_eos(&string)  ?:
        write_type_register(type, number, value);
}


error__t raw_format_int(
    void *owner, void *data, unsigned int number, char result[], size_t length)
{
    struct type *type = owner;
    uint32_t value;
    return
        read_type_register(type, number, &value)  ?:
        format_string(result, length, "%d", (int) value);
}


error__t raw_put_int(
    void *owner, void *data, unsigned int number, const char *string)
{
    struct type *type = owner;
    int value;
    return
        parse_int(&string, &value)  ?:
        parse_eos(&string)  ?:
        write_type_register(type, number, (unsigned int) value);
}


/*****************************************************************************/
/* Individual type implementations. */


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
    *max_value = UINT32_MAX;
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

static error__t int_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
{
    return
        parse_int(&string, (int *) value)  ?:
        parse_eos(&string);
}


static error__t uint_format(
    void *type_data, unsigned int number,
    unsigned int value, char string[], size_t length)
{
    return format_string(string, length, "%u", value);
}

static error__t int_format(
    void *type_data, unsigned int number,
    unsigned int value, char string[], size_t length)
{
    return format_string(string, length, "%d", (int) value);
}


static error__t uint_max_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    unsigned int *max_value = data;
    return format_string(result, length, "%u", *max_value);
}


static const struct type_methods uint_type_methods = {
    "uint",
    .init = uint_init,
    .parse = uint_parse, .format = uint_format,
    .attrs = (struct attr_methods[]) {
        { "MAX", "Maximum valid value for this field",
          .format = uint_max_format, },
    },
    .attr_count = 1,
};

static const struct type_methods int_type_methods = {
    "int",
    .parse = int_parse, .format = int_format,
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Scalar type: floating point number scaled by constant factor. */


struct scalar_state {
    double scale;
};


static error__t scalar_init(
    const char **string, unsigned int count, void **type_data)
{
    struct scalar_state *state = malloc(sizeof(struct scalar_state));
    *type_data = state;
    return
        parse_char(string, ' ')  ?:
        parse_double(string, &state->scale);
}


static error__t scalar_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
{
    struct scalar_state *state = type_data;
    double result;
    return
        parse_double(&string, &result)  ?:
        parse_eos(&string)  ?:
        DO(result /= state->scale)  ?:
        TEST_OK_(INT_MIN <= result  &&  result <= INT_MAX,
            "Value out of range")  ?:
        DO(*value = (unsigned int) lround(result));
}


static error__t scalar_format(
    void *type_data, unsigned int number,
    unsigned int value, char string[], size_t length)
{
    struct scalar_state *state = type_data;
    return format_double(string, length, state->scale * value);
}


static const struct type_methods scalar_type_methods = {
    "scalar",
    .init = scalar_init,
    .parse = scalar_parse, .format = scalar_format,
    .attrs = (struct attr_methods[]) {
        {   "RAW", "Underlying integer value",
            .format = raw_format_int, .put = raw_put_int, },
    },
    .attr_count = 1,
};


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


static const struct type_methods bit_type_methods = {
    "bit",
    .parse = bit_parse, .format = bit_format,
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Action types must have an empty write, and cannot be read. */

static error__t action_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
{
    *value = 0;
    return parse_eos(&string);
}


static const struct type_methods action_type_methods = {
    "action",
    .parse = action_parse,
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Lookup table type. */

struct lut_state {
    pthread_mutex_t mutex;
    struct lut_field {
        unsigned int value;
        char *string;
    } values[];
};


static error__t lut_init(
    const char **string, unsigned int count, void **type_data)
{
    struct lut_state *state = malloc(
        sizeof(struct lut_state) + count * sizeof(struct lut_field));
    *state = (struct lut_state) {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
    };
    for (unsigned int i = 0; i < count; i ++)
        state->values[i] = (struct lut_field) { };
    *type_data = state;
    return ERROR_OK;
}

static void lut_destroy(void *type_data, unsigned int count)
{
    struct lut_state *state = type_data;
    for (unsigned int i = 0; i < count; i ++)
        free(state->values[i].string);
    free(state);
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
    struct lut_field *field = &state->values[number];

    error__t error = do_parse_lut(string, value);
    if (!error)
    {
        LOCK(state->mutex);
        field->value = *value;
        free(field->string);
        field->string = strdup(string);
        UNLOCK(state->mutex);
    }
    return error;
}


static error__t lut_format(
    void *type_data, unsigned int number,
    unsigned int value, char string[], size_t length)
{
    struct lut_state *state = type_data;
    struct lut_field *field = &state->values[number];

    error__t error;
    LOCK(state->mutex);
    if (value == field->value  &&  field->string)
        error = format_string(string, length, "%s", field->string);
    else
        /* If no string has been formatted yet, or if our stored value doesn't
         * match the value we're being asked to format just return the raw hex
         * value.  This second case is rather unlikely. */
        error = format_string(string, length, "0x%08X", value);
    UNLOCK(state->mutex);
    return error;
}


static const struct type_methods lut_type_methods = {
    "lut",
    .init = lut_init, .destroy = lut_destroy,
    .parse = lut_parse, .format = lut_format,
    .attrs = (struct attr_methods[]) {
        { "RAW", "Bit pattern written to register",
          .format = raw_format_uint, }, },
    .attr_count = 1,
};


/*****************************************************************************/
/* Type formatting API. */


/* Implements block[n].field=value */
error__t type_get(
    struct type *type, unsigned int number, char result[], size_t length)
{
    uint32_t value;
    return
        TEST_OK_(type->methods->format,
            "Cannot read %s value", type->methods->name)  ?:
        read_type_register(type, number, &value)  ?:
        type->methods->format(type->type_data, number, value, result, length);
}


/* Implements block[n].field? */
error__t type_put(struct type *type, unsigned int number, const char *string)
{
    uint32_t value;
    return
        TEST_OK_(type->methods->parse,
            "Cannot write %s value", type->methods->name)  ?:
        type->methods->parse(type->type_data, number, string, &value)  ?:
        write_type_register(type, number, value);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Type access helpers. */


const char *get_type_name(const struct type *type)
{
    return type->methods->name;
}


const struct enumeration *get_type_enumeration(const struct type *type)
{
    if (type->methods->get_enumeration)
        return type->methods->get_enumeration(type->type_data);
    else
        return NULL;
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
    else
        free(type->type_data);
    free(type);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


static const struct type_methods *types_table[] = {
    &uint_type_methods,             // uint
    &int_type_methods,              // int
    &scalar_type_methods,           // scalar
    &bit_type_methods,              // bit

    &action_type_methods,           // action

    &lut_type_methods,              // lut

    &position_type_methods,         // position
    &time_type_methods,             // time

    &enum_type_methods,             // enum
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation. */

static error__t lookup_type(
    const char *name, const struct type_methods **result)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(types_table); i ++)
    {
        const struct type_methods *methods = types_table[i];
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
    create_attributes(
        type->methods->attrs, type->methods->attr_count,
        type, type->type_data, type->count, attr_map);
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


void *get_type_state(struct type *type)
{
    return type->type_data;
}
