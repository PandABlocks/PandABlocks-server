/* Implementation of basic register interface. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "error.h"
#include "config_server.h"
#include "hardware.h"
#include "parse.h"
#include "hashtable.h"
#include "capture.h"
#include "types.h"
#include "attributes.h"
#include "classes.h"

#include "register.h"


/*****************************************************************************/
/* Shared implementation for param and read classes: they share the same basic
 * state and quite a bit of implementation is shared. */


/* Parameter and read registers share the same basic state. */
struct simple_state {
    struct type *type;
    unsigned int block_base;
    unsigned int field_register;

    unsigned int count;
    struct simple_field {
        uint32_t value;
        uint64_t update_index;
    } values[];
};


static error__t simple_register_init(
    struct register_methods *methods,
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data)
{
    size_t fields_size = count * sizeof(struct simple_field);
    struct simple_state *state = malloc(
        sizeof(struct simple_state) + fields_size);
    *state = (struct simple_state) {
        .field_register = UNASSIGNED_REGISTER,
        .count = count,
    };
    memset(state->values, 0, fields_size);
    *class_data = state;

    return create_type(
        line, "uint", count, methods, state, attr_map, &state->type);
}


static void simple_register_destroy(void *class_data)
{
    struct simple_state *state = class_data;
    destroy_type(state->type);
}


static error__t simple_register_parse_attribute(
    void *class_data, const char **line)
{
    struct simple_state *state = class_data;
    return type_parse_attribute(state->type, line);
}


static error__t simple_register_parse_register(
    void *class_data, const char *block_name, const char *field_name,
    const char **line)
{
    struct simple_state *state = class_data;
    return
        TEST_OK_(state->field_register == UNASSIGNED_REGISTER,
            "Register already assigned")  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &state->field_register);
}


static error__t simple_register_finalise(
    void *class_data, unsigned int block_base)
{
    struct simple_state *state = class_data;
    state->block_base = block_base;
    return ERROR_OK;
}


static const char *simple_register_describe(void *class_data)
{
    struct simple_state *state = class_data;
    return get_type_name(state->type);
}


/* The following methods defined above are shared between param and read
 * classes. */
#define TYPED_REGISTER_METHODS \
    .destroy = simple_register_destroy, \
    .parse_attribute = simple_register_parse_attribute, \
    .parse_register = simple_register_parse_register, \
    .finalise = simple_register_finalise, \
    .describe = simple_register_describe


static error__t simple_register_get(
    void *class_data, unsigned int number, struct connection_result *result)
{
    struct simple_state *state = class_data;
    return type_get(state->type, number, result);
}


static error__t simple_register_put(
    void *class_data, unsigned int number, const char *string)
{
    struct simple_state *state = class_data;
    return type_put(state->type, number, string);
}


static void simple_register_changed(void *reg_data, unsigned int number)
{
    struct simple_state *state = reg_data;
    state->values[number].update_index = get_change_index();
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Parameter registers. */

/* All of bit_in, pos_in and param have very similar behaviour: values are
 * written to a register, the written value is cached for readback, and we keep
 * track of the report index. */

static uint32_t param_read(void *reg_data, unsigned int number)
{
    struct simple_state *state = reg_data;
    return state->values[number].value;
}


static void param_write(void *reg_data, unsigned int number, uint32_t value)
{
    struct simple_state *state = reg_data;
    state->values[number].value = value;
    state->values[number].update_index = get_change_index();
    hw_write_register(state->block_base, number, state->field_register, value);
}


static struct register_methods param_methods = {
    .read = param_read,
    .write = param_write,
    .changed = simple_register_changed,
};


static error__t param_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data)
{
    return simple_register_init(
        &param_methods, line, count, attr_map, class_data);
}


static void param_change_set(
    void *class_data, const uint64_t report_index[], bool changes[])
{
    struct simple_state *state = class_data;
    uint64_t report = report_index[CHANGE_IX_CONFIG];
    for (unsigned int i = 0; i < state->count; i ++)
        changes[i] = state->values[i].update_index >= report;
}


const struct class_methods param_class_methods = {
    "param",
    TYPED_REGISTER_METHODS,
    .init = param_init,
    .get = simple_register_get,
    .put = simple_register_put,
    .change_set = param_change_set,
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Read only registers. */

/* These are very similar to parameter registers, but reading and change_set
 * control are somewhat different. */

static uint32_t read_read(void *reg_data, unsigned int number)
{
    struct simple_state *state = reg_data;
    uint32_t result =
        hw_read_register(state->block_base, number, state->field_register);
    if (result != state->values[number].value)
    {
        state->values[number].value = result;
        state->values[number].update_index = get_change_index();
    }
    return result;
}


static void read_change_set(
    void *class_data, const uint64_t report_index[], bool changes[])
{
    struct simple_state *state = class_data;
    uint64_t report = report_index[CHANGE_IX_READ];
    for (unsigned int i = 0; i < state->count; i ++)
    {
        read_read(state, i);
        changes[i] = state->values[i].update_index >= report;
    }
}


static struct register_methods read_methods = {
    .read = read_read,
    .changed = simple_register_changed,
};


static error__t read_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data)
{
    return simple_register_init(
        &read_methods, line, count, attr_map, class_data);
}


const struct class_methods read_class_methods = {
    "read",
    TYPED_REGISTER_METHODS,
    .init = read_init,
    .get = simple_register_get,
    .change_set = read_change_set,
};


/*****************************************************************************/
/* Write only registers. */

struct write_state {
    struct type *type;
    unsigned int block_base;
    unsigned int field_register;
};


static void write_write(void *reg_data, unsigned int number, uint32_t value)
{
    struct write_state *state = reg_data;
    hw_write_register(state->block_base, number, state->field_register, value);
}

static struct register_methods write_methods = {
    .write = write_write,
};


static error__t  write_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data)
{
    struct write_state *state = malloc(sizeof(struct write_state));
    *state = (struct write_state) { };
    *class_data = state;

    return create_type(
        line, "uint", count, &write_methods, state, attr_map, &state->type);
}

static void write_destroy(void *class_data)
{
    struct write_state *state = class_data;
    destroy_type(state->type);
}

static error__t write_parse_attribute(void *class_data, const char **line)
{
    struct write_state *state = class_data;
    return type_parse_attribute(state->type, line);
}

static error__t write_parse_register(
    void *class_data, const char *block_name, const char *field_name,
    const char **line)
{
    struct write_state *state = class_data;
    return parse_uint(line, &state->field_register);
}

static error__t write_finalise(void *class_data, unsigned int block_base)
{
    struct write_state *state = class_data;
    state->block_base = block_base;
    return ERROR_OK;
}

static const char *write_describe(void *class_data)
{
    struct write_state *state = class_data;
    return get_type_name(state->type);
}

static error__t write_put(
    void *class_data, unsigned int number, const char *string)
{
    struct write_state *state = class_data;
    return type_put(state->type, number, string);
}


const struct class_methods write_class_methods = {
    "write",
    .destroy = write_destroy,
    .parse_attribute = write_parse_attribute,
    .parse_register = write_parse_register,
    .finalise = write_finalise,
    .describe = write_describe,
    .init = write_init,
    .put = write_put,
};
