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

/* Common shared state for all register functions. */
struct base_state {
    struct type *type;              // Type implementation for value conversion
    unsigned int block_base;        // Base number for block
    /* Either field_register or extension is defined: if extension is non NULL
     * then field_register is ignored. */
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


/* The register specification is either a single register, specifying the
 * hardware register accessed by this field, or is an extension register with a
 * much more complex syntax.  Fortunately this can simply be identified by the
 * presence of an X character in the specification. */
static error__t base_parse_register(
    struct base_state *state, struct field *field, unsigned int block_base,
    const char **line, bool write_not_read)
{
    state->block_base = block_base;

    /* The syntax for extension registers is most simply identified by checking
     * for an X in the line. */
    if (strchr(*line, 'X'))
        return parse_extension_register(
            line, get_block_extension(get_field_block(field)),
            block_base, write_not_read, &state->extension);
    else
        return check_parse_register(field, line, &state->field_register);
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
static error__t write_register(
    struct base_state *state, unsigned int number, uint32_t value)
{
    if (state->extension)
        return extension_write_register(state->extension, number, value);
    else
        return DO(hw_write_register(
            state->block_base, number, state->field_register, value));
}


static error__t read_register(
    struct base_state *state, unsigned int number, uint32_t *result)
{
    if (state->extension)
        return extension_read_register(state->extension, number, result);
    else
        return DO(*result = hw_read_register(
            state->block_base, number, state->field_register));
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
    WITH_MUTEX(state->mutex)
        state->values[number].update_index = get_change_index();
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
    return ERROR_WITH_MUTEX(state->mutex,
        write_register(&state->base, number, value)  ?:
        DO( state->values[number].value = value;
            state->values[number].update_index = get_change_index()));
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
    error__t error = ERROR_OK;
    for (unsigned int i = 0; error && i < state->count; i ++)
        error = write_register(&state->base, i, state->values[i].value);
    return error;
}


static void param_change_set(
    void *class_data, uint64_t report_index, bool changes[])
{
    struct simple_state *state = class_data;
    WITH_MUTEX(state->mutex)
        for (unsigned int i = 0; i < state->count; i ++)
            changes[i] = state->values[i].update_index > report_index;
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
static error__t locked_read_read(
    struct simple_state *state, unsigned int number, uint32_t *result)
{
    error__t error = read_register(&state->base, number, result);
    if (!error  &&  *result != state->values[number].value)
    {
        state->values[number].value = *result;
        state->values[number].update_index = get_change_index();
    }
    return error;
}

static error__t read_read(void *reg_data, unsigned int number, uint32_t *value)
{
    struct simple_state *state = reg_data;
    return ERROR_WITH_MUTEX(state->mutex,
        locked_read_read(state, number, value));
}


static void read_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct simple_state *state = class_data;
    WITH_MUTEX(state->mutex)
    {
        for (unsigned int i = 0; i < state->count; i ++)
        {
            uint32_t result;
            ERROR_REPORT(
                locked_read_read(state, i, &result),
                "Error reading register while polling change set");
            changes[i] = state->values[i].update_index > report_index;
        }
    }
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
    return write_register(state, number, value);
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
