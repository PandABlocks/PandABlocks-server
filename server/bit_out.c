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
#include "classes.h"
#include "attributes.h"
#include "types.h"
#include "enums.h"
#include "locking.h"
#include "output.h"

#include "bit_out.h"


/* Protects updating of bits. */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;


/* Position and update indices for the bits. */
static bool bit_value[BIT_BUS_COUNT];
static uint64_t bit_update_index[BIT_BUS_COUNT];


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


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


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* bit_mux lookup and associated type methods. */


static struct enumeration *bit_mux_lookup;


void report_capture_bits(struct connection_result *result, unsigned int group)
{
    for (unsigned int i = 0; i < 32; i ++)
        result->write_many(result->write_context,
            enum_index_to_name(bit_mux_lookup, 32*group + i) ?: "");
    result->response = RESPONSE_MANY;
}


static error__t bit_mux_init(
    const char **string, unsigned int count, void **type_data)
{
    *type_data = bit_mux_lookup;
    return ERROR_OK;
}


/* We need a dummy type destroy method to ensure that the associated
 * bit_mux_lookup is not destroyed. */
static void bit_mux_destroy(void *type_data, unsigned int count)
{
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Field specific state. */


struct bit_out_state {
    unsigned int count;
    unsigned int index_array[];
};



static error__t bit_out_get(
    void *class_data, unsigned int number, struct connection_result *result)
{
    struct bit_out_state *state = class_data;
    bool bit = bit_value[state->index_array[number]];
    return write_one_result(result, "%d", bit);
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
    struct hash_table *attr_map, void **class_data)
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
        parse_whitespace(line)  ?:
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
    bit_mux_lookup = create_dynamic_enumeration(BIT_BUS_COUNT);
    return ERROR_OK;
}


void terminate_bit_out(void)
{
    if (bit_mux_lookup)
        destroy_enumeration(bit_mux_lookup);
    for (unsigned int i = 0; i < BIT_BUS_COUNT/32; i ++)
        free(group_name[i]);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Type and class definitions. */


const struct type_methods bit_mux_type_methods = {
    "bit_mux",
    .init = bit_mux_init,
    .destroy = bit_mux_destroy,
    .parse = enum_parse,
    .format = enum_format,
    .get_enumeration = enum_get_enumeration,
};


const struct class_methods bit_out_class_methods = {
    "bit_out",
    .init = bit_out_init,
    .parse_register = bit_out_parse_register,
    .get = bit_out_get, .refresh = bit_out_refresh,
    .change_set = bit_out_change_set,
    .change_set_index = CHANGE_IX_BITS,
    .attrs = (struct attr_methods[]) {
        { "CAPTURE_WORD", "Name of field containing this bit",
            .format = capture_word_format,
        },
        { "OFFSET", "Position of this bit in captured word",
            .format = offset_format,
        },
    },
    .attr_count = 2,
};
