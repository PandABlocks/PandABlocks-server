/* Support for bit_out class. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "error.h"
#include "hardware.h"
#include "parse.h"
#include "config_server.h"
#include "data_server.h"
#include "fields.h"
#include "attributes.h"
#include "types.h"
#include "enums.h"
#include "locking.h"
#include "pos_mux.h"
#include "output.h"

#include "bit_out.h"


/* Maximum valid delay, defined by hardware. */
#define MAX_BIT_MUX_DELAY   31


/* Protects updating of bits. */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;


/* Position and update indices for the bits. */
static bool bit_value[BIT_BUS_COUNT];
static uint64_t bit_update_index[] = { [0 ... BIT_BUS_COUNT-1] = 1 };


/*****************************************************************************/
/* bit_mux lookup and associated class methods. */


struct bit_mux_state {
    pthread_mutex_t mutex;
    unsigned int block_base;
    unsigned int mux_reg;
    unsigned int delay_reg;
    unsigned int count;
    struct bit_mux_value {
        unsigned int value;
        unsigned int delay;
        uint64_t update_index;
    } values[];
};


static struct enumeration *bit_mux_lookup;


/* Implements .BITS attribute for bit group capture fields. */
void report_capture_bits(struct connection_result *result, unsigned int group)
{
    for (unsigned int i = 0; i < 32; i ++)
        result->write_many(result->write_context,
            enum_index_to_name(bit_mux_lookup, 32*group + i) ?: "");
}


static error__t parse_default_param(
    const char **line, unsigned int count, struct bit_mux_state *state)
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


static error__t bit_mux_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data,
    struct indent_parser *parser)
{
    struct bit_mux_state *state = malloc(
        sizeof(struct bit_mux_state) + count * sizeof(struct bit_mux_value));
    *state = (struct bit_mux_state) {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .count = count,
    };
    for (unsigned int i = 0; i < count; i ++)
        state->values[i] = (struct bit_mux_value) {
            .value = BIT_BUS_ZERO,
            .update_index = 1,
        };
    *class_data = state;

    return parse_default_param(line, count, state);
}


static error__t bit_mux_finalise(void *class_data)
{
    struct bit_mux_state *state = class_data;
    for (unsigned int i = 0; i < state->count; i ++)
        hw_write_register(
            state->block_base, i, state->mux_reg, state->values[i].value);
    return ERROR_OK;
}


static error__t bit_mux_parse_register(
    void *class_data, struct field *field, unsigned int block_base,
    const char **line)
{
    struct bit_mux_state *state = class_data;
    state->block_base = block_base;
    return
        check_parse_register(field, line, &state->mux_reg) ?:
        parse_whitespace(line)  ?:
        check_parse_register(field, line, &state->delay_reg);
}


static error__t bit_mux_get(
    void *class_data, unsigned int number, char result[], size_t length)
{
    struct bit_mux_state *state = class_data;
    return format_enumeration(
        bit_mux_lookup, state->values[number].value, result, length);
}


static error__t bit_mux_put(
    void *class_data, unsigned int number, const char *string)
{
    struct bit_mux_state *state = class_data;
    unsigned int mux_value;
    error__t error = TEST_OK_(
        enum_name_to_index(bit_mux_lookup, string, &mux_value),
        "Invalid bit bus selection");
    if (!error)
    {
        struct bit_mux_value *value = &state->values[number];
        LOCK(state->mutex);
        value->value = mux_value;
        value->update_index = get_change_index();
        hw_write_register(state->block_base, number, state->mux_reg, mux_value);
        UNLOCK(state->mutex);
    }
    return error;
}


static void bit_mux_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct bit_mux_state *state = class_data;
    LOCK(state->mutex);
    for (unsigned int i = 0; i < state->count; i ++)
        changes[i] = state->values[i].update_index > report_index;
    UNLOCK(state->mutex);
}


static const struct enumeration *bit_mux_get_enumeration(void *class_data)
{
    return bit_mux_lookup;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* DELAY and MAX_DELAY attributes. */

static error__t bit_mux_delay_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct bit_mux_state *state = data;
    return format_string(result, length, "%u", state->values[number].delay);
}


static error__t bit_mux_delay_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct bit_mux_state *state = data;
    unsigned int delay;
    return
        parse_uint(&value, &delay)  ?:
        TEST_OK_(delay <= MAX_BIT_MUX_DELAY, "Delay too long")  ?:
        DO( state->values[number].delay = delay;
            hw_write_register(
                state->block_base, number, state->delay_reg, delay));
}


static error__t bit_mux_max_delay_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    return format_string(result, length, "%u", MAX_BIT_MUX_DELAY);
}


/*****************************************************************************/
/* bit_out class. */


struct bit_out_state {
    unsigned int count;
    unsigned int index_array[];
};



static error__t bit_out_get(
    void *class_data, unsigned int number, char result[], size_t length)
{
    struct bit_out_state *state = class_data;
    bool bit = bit_value[state->index_array[number]];
    return format_string(result, length, "%d", bit);
}


static void bit_out_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct bit_out_state *state = class_data;
    LOCK(mutex);
    for (unsigned int i = 0; i < state->count; i ++)
        changes[i] = bit_update_index[state->index_array[i]] > report_index;
    UNLOCK(mutex);
}


static error__t bit_out_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data,
    struct indent_parser *parser)
{
    struct bit_out_state *state = malloc(
        sizeof(struct bit_out_state) + count * sizeof(unsigned int));
    *state = (struct bit_out_state) {
        .count = count,
    };
    *class_data = state;
    return ERROR_OK;
}


static error__t bit_out_parse_register(
    void *class_data, struct field *field, unsigned int block_base,
    const char **line)
{
    struct bit_out_state *state = class_data;
    return
        parse_uint_array(line, state->index_array, state->count)  ?:
        add_mux_indices(
            bit_mux_lookup, field, state->index_array, state->count);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Attributes. */


static char *group_name[BIT_BUS_COUNT/32];


void set_bit_group_name(unsigned int group, const char *name)
{
    group_name[group] = strdup(name);
}


static error__t capture_word_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct bit_out_state *state = data;
    unsigned int group = state->index_array[number] / 32;
    return format_string(result, length, "%s", group_name[group]);
}


static error__t offset_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct bit_out_state *state = data;
    unsigned int offset = state->index_array[number] % 32;
    return format_string(result, length, "%u", offset);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Note: bit_mux_lookup declaration belongs here, not in enums.c */

error__t initialise_bit_out(void)
{
    bit_mux_lookup = create_dynamic_enumeration();
    return
        add_enumeration(bit_mux_lookup, "ZERO", BIT_BUS_ZERO)  ?:
        add_enumeration(bit_mux_lookup, "ONE", BIT_BUS_ONE);
}


void terminate_bit_out(void)
{
    if (bit_mux_lookup)
        destroy_enumeration(bit_mux_lookup);
    for (unsigned int i = 0; i < BIT_BUS_COUNT/32; i ++)
        free(group_name[i]);
}


void do_bit_out_refresh(uint64_t change_index)
{
    LOCK(mutex);
    bool changes[BIT_BUS_COUNT];
    hw_read_bits(bit_value, changes);
    for (unsigned int i = 0; i < BIT_BUS_COUNT; i ++)
        if (changes[i]  &&  change_index > bit_update_index[i])
            bit_update_index[i] = change_index;
    UNLOCK(mutex);
}


static void bit_out_refresh(void *class_data, unsigned int number)
{
    do_bit_out_refresh(get_change_index());
}


/*****************************************************************************/
/* Class definitions. */


const struct class_methods bit_mux_class_methods = {
    "bit_mux",
    .init = bit_mux_init,
    .finalise = bit_mux_finalise,
    .parse_register = bit_mux_parse_register,
    .get = bit_mux_get,
    .put = bit_mux_put,
    .get_enumeration = bit_mux_get_enumeration,
    .change_set = bit_mux_change_set,
    .change_set_index = CHANGE_IX_CONFIG,
    DEFINE_ATTRIBUTES(
        { "DELAY", "Clock delay on input",
            .in_change_set = true,
            .format = bit_mux_delay_format,
            .put = bit_mux_delay_put,
        },
        { "MAX_DELAY", "Maximum valid input delay",
            .format = bit_mux_max_delay_format,
        },
    ),
};


const struct class_methods bit_out_class_methods = {
    "bit_out",
    .init = bit_out_init,
    .parse_register = bit_out_parse_register,
    .get = bit_out_get, .refresh = bit_out_refresh,
    .change_set = bit_out_change_set,
    .change_set_index = CHANGE_IX_BITS,
    DEFINE_ATTRIBUTES(
        { "CAPTURE_WORD", "Name of field containing this bit",
            .format = capture_word_format,
        },
        { "OFFSET", "Position of this bit in captured word",
            .format = offset_format,
        },
    ),
};
