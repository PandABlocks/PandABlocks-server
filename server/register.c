/* Implementation of basic register interface. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>

#include "error.h"
#include "config_server.h"
#include "hardware.h"
#include "parse.h"
#include "hashtable.h"
#include "capture.h"
#include "types.h"
#include "attributes.h"
#include "classes.h"
#include "locking.h"

#include "register.h"


/* Common shared state for all register functions. */
struct base_state {
    struct type *type;              // Type implementation for value conversion
    unsigned int block_base;        // Base number for block
    unsigned int field_register;    // Register to be written

    const struct filter_methods *filter;  // Optional filter
    void *filter_data;              // Context for filter
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Register filter API. */

/* A register filter is used to transform values being read and written. */

/* All the fields of a filter are optional. */
struct filter_methods {
    const char *name;

    /* Parses rest of line and creates context as appropriate. */
    error__t (*init)(const char **line, void **context);

    /* This transforms *value according to the filter rules, or returns an error
     * if the value is unacceptable. */
    error__t (*write)(void *context, uint32_t *value);

    /* Transforms *value according to filter rules (should be inverse to rules
     * applied by .write), or returns error if bad value read. */
    error__t (*read)(void *context, uint32_t *value);
};


/* Minimum value: this excludes values in the range 0..minval-1, and applies
 * minval as an offset to non-zero values. */
static error__t minval_init(const char **line, void **context)
{
    uint32_t *minval = malloc(sizeof(uint32_t));
    *context = minval;
    return
        parse_whitespace(line)  ?:
        parse_uint(line, minval);
}

static error__t minval_write(void *context, uint32_t *value)
{
    uint32_t *minval = context;
    return
        TEST_OK_(*value == 0  ||  *value > *minval, "Value too small")  ?:
        IF(*value > 0, DO(*value -= *minval));
}

static error__t minval_read(void *context, uint32_t *value)
{
    uint32_t *minval = context;
    return
        TEST_OK_(*value < UINT32_MAX - *minval, "Value too large")  ?:
        IF(*value > 0, DO(*value += *minval));
}

static const struct filter_methods filter_list[] = {
    { "minval",
        .init = minval_init,
        .write = minval_write,
        .read = minval_read, },
};


/* Helper for filter creation: searches filter_list for named filter. */
static error__t lookup_filter(
    const char *name, const struct filter_methods **filter)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(filter_list); i ++)
        if (strcmp(name, filter_list[i].name) == 0)
        {
            *filter = &filter_list[i];
            return ERROR_OK;
        }
    return FAIL_("Register filter %s not found", name);
}


/* Creates filter object if appropriate. */
static error__t create_filter(const char **line, struct base_state *state)
{
    if (**line == '\0')
        return ERROR_OK;
    else
    {
        char filter_name[MAX_NAME_LENGTH];
        return
            parse_whitespace(line)  ?:
            parse_name(line, filter_name, sizeof(filter_name))  ?:
            lookup_filter(filter_name, &state->filter)  ?:
            IF(state->filter->init,
                state->filter->init(line, &state->filter_data));
    }
}


/* Both read and write filtering are optional, only invoked if the filter is
 * present. */

static error__t filter_write(struct base_state *state, uint32_t *value)
{
    return
        IF(state->filter  &&  state->filter->write,
            state->filter->write(state->filter_data, value));
}

static error__t filter_read(struct base_state *state, uint32_t *value)
{
    return
        IF(state->filter  &&  state->filter->read,
            state->filter->read(state->filter_data, value));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Base state, common to all three class implementations here. */

static void base_destroy(void *class_data)
{
    struct base_state *state = class_data;
    destroy_type(state->type);
    free(state->filter_data);
}


static error__t base_parse_attribute(void *class_data, const char **line)
{
    struct base_state *state = class_data;
    return type_parse_attribute(state->type, line);
}


static error__t base_parse_register(
    void *class_data, struct field *field, const char **line)
{
    struct base_state *state = class_data;
    return
        TEST_OK_(state->field_register == UNASSIGNED_REGISTER,
            "Register already assigned")  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &state->field_register)  ?:
        create_filter(line, state);
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
    pthread_mutex_t mutex;

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
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .count = count,
    };
    for (unsigned int i = 0; i < count; i ++)
        state->values[i] = (struct simple_field) { };
    *class_data = state;

    return
        TRY_CATCH(
            create_type(
                line, "uint", count, methods, state, attr_map,
                &state->base.type),
        //catch
            /* If type creation failed then class creation failed. */
            free(state));
}


static void simple_register_changed(void *reg_data, unsigned int number)
{
    struct simple_state *state = reg_data;
    LOCK(state->mutex);
    state->values[number].update_index = get_change_index();
    UNLOCK(state->mutex);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Parameter registers. */

/* All of bit_in, pos_in and param have very similar behaviour: values are
 * written to a register, the written value is cached for readback, and we keep
 * track of the report index. */

static error__t param_read(void *reg_data, unsigned int number, uint32_t *value)
{
    struct simple_state *state = reg_data;
    *value = state->values[number].value;
    return filter_read(&state->base, value);
}


static error__t param_write(void *reg_data, unsigned int number, uint32_t value)
{
    struct simple_state *state = reg_data;

    error__t error = filter_write(&state->base, &value);
    if (!error)
    {
        LOCK(state->mutex);
        state->values[number].value = value;
        state->values[number].update_index = get_change_index();
        hw_write_register(
            state->base.block_base, number, state->base.field_register, value);
        UNLOCK(state->mutex);
    }
    return error;
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
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct simple_state *state = class_data;
    LOCK(state->mutex);
    for (unsigned int i = 0; i < state->count; i ++)
        changes[i] = state->values[i].update_index > report_index;
    UNLOCK(state->mutex);
}


const struct class_methods param_class_methods = {
    "param",
    BASE_METHODS,
    .init = param_init,
    .get = base_get,
    .put = base_put,
    .change_set = param_change_set,
    .change_set_index = CHANGE_IX_CONFIG,
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Read only registers. */

/* These are very similar to parameter registers, but reading and change_set
 * control are somewhat different. */

/* This must be called under a lock. */
static uint32_t unlocked_read_read(
    struct simple_state *state, unsigned int number)
{
    uint32_t result = hw_read_register(
        state->base.block_base, number, state->base.field_register);
    if (result != state->values[number].value)
    {
        state->values[number].value = result;
        state->values[number].update_index = get_change_index();
    }
    return result;
}

static error__t read_read(void *reg_data, unsigned int number, uint32_t *value)
{
    struct simple_state *state = reg_data;

    LOCK(state->mutex);
    *value = unlocked_read_read(state, number);
    UNLOCK(state->mutex);
    return filter_read(&state->base, value);
}


static void read_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct simple_state *state = class_data;
    LOCK(state->mutex);
    for (unsigned int i = 0; i < state->count; i ++)
    {
        unlocked_read_read(state, i);
        changes[i] = state->values[i].update_index > report_index;
    }
    UNLOCK(state->mutex);
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
    .change_set_index = CHANGE_IX_READ,
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Write only registers. */

/* For this the base state is sufficient. */

static error__t write_write(void *reg_data, unsigned int number, uint32_t value)
{
    struct base_state *state = reg_data;
    return
        filter_write(state, &value)  ?:
        DO(hw_write_register(
            state->block_base, number, state->field_register, value));
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
