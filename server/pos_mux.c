/* Position mux support. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>

#include "error.h"
#include "hardware.h"
#include "hashtable.h"
#include "parse.h"
#include "config_server.h"
#include "attributes.h"
#include "fields.h"
#include "enums.h"
#include "locking.h"

#include "pos_mux.h"


/*****************************************************************************/
/* Position mux table support. */


/* Note that this function is used by both bit_mux and pos_mux, but happens to
 * live here for the time being. */
error__t add_mux_indices(
    struct enumeration *lookup, struct field *field,
    const unsigned int array[], size_t length)
{
    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < length; i ++)
    {
        char name[MAX_NAME_LENGTH];
        format_field_name(name, sizeof(name), field, NULL, i, '\0');
        error = add_enumeration(lookup, name, array[i]);
    }
    return error;
}


/* Map between field names and bit bus indexes. */
static struct enumeration *pos_mux_lookup;


error__t initialise_pos_mux(void)
{
    pos_mux_lookup = create_dynamic_enumeration();
    return add_enumeration(pos_mux_lookup, "ZERO", POS_BUS_ZERO);
}


void terminate_pos_mux(void)
{
    if (pos_mux_lookup)
        destroy_enumeration(pos_mux_lookup);
}


error__t add_pos_mux_index(
    struct field *field, const unsigned int array[], size_t length)
{
    return add_mux_indices(pos_mux_lookup, field, array, length);
}



/*****************************************************************************/
/* pos_mux class. */

struct pos_mux_state {
    pthread_mutex_t mutex;
    unsigned int block_base;
    unsigned int mux_reg;
    unsigned int count;
    struct pos_mux_value {
        unsigned int value;
        uint64_t update_index;
    } values[];
};


static error__t pos_mux_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data,
    struct indent_parser *parser)
{
    struct pos_mux_state *state = malloc(
        sizeof(struct pos_mux_state) + count * sizeof(struct pos_mux_value));
    *state = (struct pos_mux_state) {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .count = count,
    };
    for (unsigned int i = 0; i < count; i ++)
        state->values[i] = (struct pos_mux_value) {
            .value = POS_BUS_ZERO,
            .update_index = 1,
        };
    *class_data = state;

    return ERROR_OK;
}


static error__t pos_mux_parse_register(
    void *class_data, struct field *field, unsigned int block_base,
    const char **line)
{
    struct pos_mux_state *state = class_data;
    state->block_base = block_base;
    return check_parse_register(field, line, &state->mux_reg);
}


static error__t pos_mux_finalise(void *class_data)
{
    struct pos_mux_state *state = class_data;
    for (unsigned int i = 0; i < state->count; i ++)
        hw_write_register(
            state->block_base, i, state->mux_reg, state->values[i].value);
    return ERROR_OK;
}


static error__t pos_mux_get(
    void *class_data, unsigned int number, char result[], size_t length)
{
    struct pos_mux_state *state = class_data;
    return format_enumeration(
        pos_mux_lookup, state->values[number].value, result, length);
}


static error__t pos_mux_put(
    void *class_data, unsigned int number, const char *string)
{
    struct pos_mux_state *state = class_data;
    unsigned int mux_value;
    error__t error = TEST_OK_(
        enum_name_to_index(pos_mux_lookup, string, &mux_value),
        "Invalid position selection");
    if (!error)
    {
        struct pos_mux_value *value = &state->values[number];
        LOCK(state->mutex);
        value->value = mux_value;
        value->update_index = get_change_index();
        hw_write_register(state->block_base, number, state->mux_reg, mux_value);
        UNLOCK(state->mutex);
    }
    return error;
}


static void pos_mux_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct pos_mux_state *state = class_data;
    LOCK(state->mutex);
    for (unsigned int i = 0; i < state->count; i ++)
        changes[i] = state->values[i].update_index > report_index;
    UNLOCK(state->mutex);
}


static const struct enumeration *pos_mux_get_enumeration(void *class_data)
{
    return pos_mux_lookup;
}


/*****************************************************************************/
/* Published class definitions. */

const struct class_methods pos_mux_class_methods = {
    "pos_mux",
    .init = pos_mux_init,
    .parse_register = pos_mux_parse_register,
    .finalise = pos_mux_finalise,
    .get = pos_mux_get,
    .put = pos_mux_put,
    .get_enumeration = pos_mux_get_enumeration,
    .change_set = pos_mux_change_set,
    .change_set_index = CHANGE_IX_CONFIG,
};
