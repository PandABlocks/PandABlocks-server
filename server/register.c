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


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Base state, common to all three class implementations here. */

struct base_state {
    struct type *type;
    unsigned int block_base;
    unsigned int field_register;
};


static void base_destroy(void *class_data)
{
    struct base_state *state = class_data;
    destroy_type(state->type);
}


static error__t base_parse_attribute(void *class_data, const char **line)
{
    struct base_state *state = class_data;
    return type_parse_attribute(state->type, line);
}


static error__t base_parse_register(
    void *class_data, const char *block_name, const char *field_name,
    const char **line)
{
    struct base_state *state = class_data;
    return
        TEST_OK_(state->field_register == UNASSIGNED_REGISTER,
            "Register already assigned")  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &state->field_register);
}


static error__t base_finalise(void *class_data, unsigned int block_base)
{
    struct base_state *state = class_data;
    state->block_base = block_base;
    return ERROR_OK;
}


static const char *base_describe(void *class_data)
{
    struct base_state *state = class_data;
    return get_type_name(state->type);
}


/* The following methods defined above are shared among all three classes. */
#define BASE_METHODS \
    .destroy = base_destroy, \
    .parse_attribute = base_parse_attribute, \
    .parse_register = base_parse_register, \
    .finalise = base_finalise, \
    .describe = base_describe


static error__t base_get(
    void *class_data, unsigned int number, struct connection_result *result)
{
    struct base_state *state = class_data;
    return type_get(state->type, number, result);
}


static error__t base_put(
    void *class_data, unsigned int number, const char *string)
{
    struct base_state *state = class_data;
    return type_put(state->type, number, string);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Shared implementation for param and read classes, in particular param and
 * read classes share the same state. */

struct simple_state {
    struct base_state base;

    /* For both parameter and read registers we keep track of the last
     * read/written value and an update index, but we manage these fields
     * differently. */
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
        .base = { .field_register = UNASSIGNED_REGISTER, },
        .count = count,
    };
    memset(state->values, 0, fields_size);
    *class_data = state;

    return create_type(
        line, "uint", count, methods, state, attr_map, &state->base.type);
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
    hw_write_register(
        state->base.block_base, number, state->base.field_register, value);
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
    BASE_METHODS,
    .init = param_init,
    .get = base_get,
    .put = base_put,
    .change_set = param_change_set,
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Read only registers. */

/* These are very similar to parameter registers, but reading and change_set
 * control are somewhat different. */

static uint32_t read_read(void *reg_data, unsigned int number)
{
    struct simple_state *state = reg_data;
    uint32_t result = hw_read_register(
        state->base.block_base, number, state->base.field_register);
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
    BASE_METHODS,
    .init = read_init,
    .get = base_get,
    .change_set = read_change_set,
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Write only registers. */

/* For this the base state is sufficient. */

static void write_write(void *reg_data, unsigned int number, uint32_t value)
{
    struct base_state *state = reg_data;
    hw_write_register(state->block_base, number, state->field_register, value);
}

static struct register_methods write_methods = {
    .write = write_write,
};


static error__t write_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data)
{
    struct base_state *state = malloc(sizeof(struct base_state));
    *state = (struct base_state) {
        .field_register = UNASSIGNED_REGISTER,
    };
    *class_data = state;

    return create_type(
        line, "uint", count, &write_methods, state, attr_map, &state->type);
}


const struct class_methods write_class_methods = {
    "write",
    BASE_METHODS,
    .init = write_init,
    .put = base_put,
};
