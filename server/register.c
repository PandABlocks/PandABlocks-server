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
#include "capture.h"

#include "register.h"


struct register_methods {
    /* Reads current register value. */
    uint32_t (*read)(
        void *reg_data, unsigned int block_base, unsigned int number);
    /* Writes to register. */
    void (*write)(
        void *reg_data, unsigned int block_base, unsigned int number,
        uint32_t value);

    /* Parses register definition line. */
    error__t (*parse_register)(const char **line, void *reg_data);

    /* Computes associated change set. */
    void (*change_set)(
        void *reg_data, unsigned int block_base,
        const uint64_t report_index[], bool changes[]);

    /* Releases any register specific resources. */
    void (*destroy)(void *reg_data);
};


struct register_api {
    const struct register_methods *methods;
    void *data;
    unsigned int block_base;
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Core api wrappers. */

uint32_t read_register(struct register_api *reg, unsigned int number)
{
    return reg->methods->read(reg->data, reg->block_base, number);
}


void write_register(
    struct register_api *reg, unsigned int number, uint32_t value)
{
    reg->methods->write(reg->data, reg->block_base, number, value);
}


void register_change_set(
    struct register_api *reg, const uint64_t report_index[], bool changes[])
{
    reg->methods->change_set(reg->data, reg->block_base, report_index, changes);
}


error__t register_parse_register(const char **line, struct register_api *reg)
{
    return reg->methods->parse_register(line, reg->data);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Administration. */


static struct register_api *create_register_api(
    const struct register_methods *methods, void *data)
{
    struct register_api *reg = malloc(sizeof(struct register_api));
    *reg = (struct register_api) {
        .methods = methods,
        .data = data,
    };
    return reg;
}


error__t finalise_register(struct register_api *reg, unsigned int block_base)
{
    reg->block_base = block_base;
    return ERROR_OK;
}


void destroy_register(struct register_api *reg)
{
    if (reg->methods->destroy)
        reg->methods->destroy(reg->data);
    free(reg);
}



/*****************************************************************************/
/* Individual implementations. */


/* Parameter and simple registers share the same basic state. */
struct simple_state {
    unsigned int field_register;
    unsigned int count;
    struct simple_field {
        uint32_t value;
        uint64_t update_index;
    } values[];
};


static struct register_api *create_simple_register(
    struct register_methods *methods, unsigned int count)
{
    size_t fields_size = count * sizeof(struct simple_field);
    struct simple_state *state = malloc(
        sizeof(struct simple_state) + fields_size);
    *state = (struct simple_state) {
        .field_register = UNASSIGNED_REGISTER,
        .count = count,
    };
    memset(state->values, 0, fields_size);
    return create_register_api(methods, state);
}


static error__t simple_parse_register(const char **line, void *reg_data)
{
    struct simple_state *state = reg_data;
    return
        TEST_OK_(state->field_register == UNASSIGNED_REGISTER,
            "Register already assigned")  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &state->field_register);
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Parameter registers. */

/* All of bit_in, pos_in and param have very similar behaviour: values are
 * written to a register, the written value is cached for readback, and we keep
 * track of the report index. */

static uint32_t param_read(
    void *reg_data, unsigned int block_base, unsigned int number)
{
    struct simple_state *state = reg_data;
    return state->values[number].value;
}


static void param_write(
    void *reg_data, unsigned int block_base, unsigned int number,
    uint32_t value)
{
    struct simple_state *state = reg_data;
    state->values[number].value = value;
    state->values[number].update_index = get_change_index();
    hw_write_register(block_base, number, state->field_register, value);
}


static void param_change_set(
    void *reg_data, unsigned int block_base,
    const uint64_t report_index[], bool changes[])
{
    struct simple_state *state = reg_data;
    uint64_t report = report_index[CHANGE_IX_CONFIG];
    for (unsigned int i = 0; i < state->count; i ++)
        changes[i] = state->values[i].update_index >= report;
}


static struct register_methods param_methods = {
    .read = param_read,
    .write = param_write,
    .change_set = param_change_set,
    .parse_register = simple_parse_register,
    .destroy = free,
};


struct register_api *create_param_register(unsigned int count)
{
    return create_simple_register(&param_methods, count);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Read only registers. */

/* These are very similar to parameter registers, but reading and change_set
 * control are somewhat different. */

static uint32_t read_read(
    void *reg_data, unsigned int block_base, unsigned int number)
{
    struct simple_state *state = reg_data;
    uint32_t result =
        hw_read_register(block_base, number, state->field_register);
    if (result != state->values[number].value)
    {
        state->values[number].value = result;
        state->values[number].update_index = get_change_index();
    }
    return result;
}


static void read_change_set(
    void *reg_data, unsigned int block_base,
    const uint64_t report_index[], bool changes[])
{
    struct simple_state *state = reg_data;
    uint64_t report = report_index[CHANGE_IX_READ];
    for (unsigned int i = 0; i < state->count; i ++)
    {
        read_read(reg_data, block_base, i);
        changes[i] = state->values[i].update_index >= report;
    }
}


static struct register_methods read_methods = {
    .read = read_read,
    .change_set = read_change_set,
    .parse_register = simple_parse_register,
    .destroy = free,
};


struct register_api *create_read_register(unsigned int count)
{
    return create_simple_register(&read_methods, count);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Write only registers. */

struct write_state {
    unsigned int field_register;
};


static error__t write_parse_register(const char **line, void *reg_data)
{
    struct write_state *state = reg_data;
    return parse_uint(line, &state->field_register);
}


static void write_write(
    void *reg_data, unsigned int block_base, unsigned int number,
    uint32_t value)
{
    struct write_state *state = reg_data;
    hw_write_register(block_base, number, state->field_register, value);
}


static struct register_methods write_methods = {
    .write = write_write,
    .parse_register = write_parse_register,
    .destroy = free,
};


struct register_api *create_write_register(unsigned int count)
{
    return create_register_api(
        &write_methods, malloc(sizeof(struct write_state)));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Capture output registers. */

/* For these registers all the work is done elsewhere, here we provide just the
 * basic hooks for register access to help with type integration. */


static const struct register_methods bit_out_methods = {
    .read = bit_out_read,
};

struct register_api *create_bit_out_register(void *class_data)
{
    return create_register_api(&bit_out_methods, class_data);
}


static const struct register_methods pos_out_methods = {
    .read = pos_out_read,
};

struct register_api *create_pos_out_register(void *class_data)
{
    return create_register_api(&pos_out_methods, class_data);
}
