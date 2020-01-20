/* Implementation of basic register interface. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>

#include "error.h"
#include "config_server.h"
#include "hardware.h"
#include "parse.h"
#include "hashtable.h"
#include "attributes.h"
#include "fields.h"
#include "types.h"
#include "locking.h"
#include "extension.h"

#include "register.h"


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Base state, common to all three class implementations here. */

#define UNUSED_REGISTER     UINT_MAX

/* Common shared state for all register functions. */
struct base_state {
    struct type *type;              // Type implementation for value conversion
    unsigned int block_base;        // Base number for block
    /* Optionally one or both of field_register or extension can be defined for
     * writeable field, but only one for read only fields. */
    unsigned int field_register;    // Register to be read or written
    struct extension_address *extension;    // Extension address
};


static void base_destroy(void *class_data)
{
    struct base_state *state = class_data;
    destroy_type(state->type);
    if (state->extension)
        destroy_extension_address(state->extension);
    free(state);
}


/* Depending on whether this is a read or a write register the register syntax
 * is either
 *
 *  reg | X extension | reg X extension         (for write only registers)
 *  reg | X extension                           (for read only registers)
 *
 * In the case where both a register and an extension are specified, both
 * registers are written to when appropriate. */
static error__t base_parse_register(
    struct base_state *state, struct field *field, unsigned int block_base,
    const char **line, bool write_not_read)
{
    state->block_base = block_base;
    state->field_register = UNUSED_REGISTER;
    struct extension_block *extension =
        get_block_extension(get_field_block(field));
    return
        IF_ELSE(read_char(line, 'X'),
            // Extension register only
            parse_extension_address(
                line, extension, write_not_read, &state->extension),
        //else
            // Otherwise there must be a normal register, maybe followed by an
            // extension register
            check_parse_register(field, line, &state->field_register)  ?:
            IF(read_char(line, ' '),
                parse_char(line, 'X')  ?:
                TEST_OK_(write_not_read,
                    "Cannot specify two registers for read")  ?:
                parse_extension_address(
                    line, extension, write_not_read, &state->extension)));

}

static error__t read_parse_register(
    void *class_data, struct field *field, unsigned int block_base,
    const char **line)
{
    return base_parse_register(class_data, field, block_base, line, false);
}

static error__t write_parse_register(
    void *class_data, struct field *field, unsigned int block_base,
    const char **line)
{
    return base_parse_register(class_data, field, block_base, line, true);
}


static const char *base_describe(void *class_data)
{
    struct base_state *state = class_data;
    return get_type_name(state->type);
}


static const struct enumeration *base_get_enumeration(void *class_data)
{
    struct base_state *state = class_data;
    return get_type_enumeration(state->type);
}


/* The following methods defined above are shared among all three classes. */
#define BASE_METHODS \
    .destroy = base_destroy, \
    .describe = base_describe, \
    .get_enumeration = base_get_enumeration


static error__t base_get(
    void *class_data, unsigned int number, char result[], size_t length)
{
    struct base_state *state = class_data;
    return type_get(state->type, number, result, length);
}


static error__t base_put(
    void *class_data, unsigned int number, const char *string)
{
    struct base_state *state = class_data;
    return type_put(state->type, number, string);
}


/* Register access methods. */

/* Note that writing to a register can write to both a hardware and an extension
 * register if appropriate. */
static void write_register(
    struct base_state *state, unsigned int number, uint32_t value)
{
    if (state->field_register != UINT_MAX)
        hw_write_register(
            state->block_base, number, state->field_register, value);
    if (state->extension)
        extension_write_register(state->extension, number, value);
}


static uint32_t read_register(struct base_state *state, unsigned int number)
{
    if (state->extension)
        return extension_read_register(state->extension, number);
    else
        return hw_read_register(
            state->block_base, number, state->field_register);
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
    struct hash_table *attr_map, void **class_data,
    struct indent_parser *parser)
{
    size_t fields_size = count * sizeof(struct simple_field);
    struct simple_state *state = malloc(
        sizeof(struct simple_state) + fields_size);
    *state = (struct simple_state) {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .count = count,
    };
    for (unsigned int i = 0; i < count; i ++)
        state->values[i] = (struct simple_field) {
            .update_index = 1,
        };
    *class_data = state;

    return create_type(
        line, "uint", count, methods, state, attr_map, &state->base.type,
        parser);
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
    return ERROR_OK;
}


static error__t param_write(void *reg_data, unsigned int number, uint32_t value)
{
    struct simple_state *state = reg_data;

    LOCK(state->mutex);
    state->values[number].value = value;
    state->values[number].update_index = get_change_index();
    write_register(&state->base, number, value);
    UNLOCK(state->mutex);
    return ERROR_OK;
}


static struct register_methods param_methods = {
    .read = param_read,
    .write = param_write,
    .changed = simple_register_changed,
};


static error__t parse_default_param(
    const char **line, unsigned int count, struct simple_state *state)
{
    uint32_t default_value;
    return IF(read_char(line, ' '),
        DO(*line = skip_whitespace(*line))  ?:
        parse_char(line, '=')  ?:
        parse_uint32(line, &default_value)  ?:
        DO(
            for (unsigned int i = 0; i < count; i ++)
                state->values[i].value = default_value;
        ));
}


static error__t param_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data,
    struct indent_parser *parser)
{
    return
        simple_register_init(
            &param_methods, line, count, attr_map, class_data, parser)  ?:
        parse_default_param(line, count, *class_data);
}


static error__t param_finalise(void *class_data)
{
    struct simple_state *state = class_data;
    for (unsigned int i = 0; i < state->count; i ++)
        write_register(&state->base, i, state->values[i].value);
    return ERROR_OK;
}


static void param_change_set(
    void *class_data, uint64_t report_index, bool changes[])
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
    .parse_register = write_parse_register,
    .finalise = param_finalise,
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
    uint32_t result = read_register(&state->base, number);
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
    return ERROR_OK;
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
    struct hash_table *attr_map, void **class_data,
    struct indent_parser *parser)
{
    return simple_register_init(
        &read_methods, line, count, attr_map, class_data, parser);
}


const struct class_methods read_class_methods = {
    "read",
    BASE_METHODS,
    .init = read_init,
    .parse_register = read_parse_register,
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
    write_register(state, number, value);
    return ERROR_OK;
}

static struct register_methods write_methods = {
    .write = write_write,
};


static error__t write_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data,
    struct indent_parser *parser)
{
    struct base_state *state = calloc(1, sizeof(struct base_state));
    *class_data = state;

    return create_type(
        line, "uint", count, &write_methods, state, attr_map, &state->type,
        parser);
}


const struct class_methods write_class_methods = {
    "write",
    BASE_METHODS,
    .init = write_init,
    .parse_register = write_parse_register,
    .put = base_put,
};
