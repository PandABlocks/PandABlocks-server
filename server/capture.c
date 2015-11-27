#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

#include "error.h"
#include "hardware.h"
#include "hashtable.h"
#include "parse.h"
#include "config_server.h"
#include "classes.h"
#include "attributes.h"
#include "types.h"
#include "register.h"

#include "capture.h"


static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()      ASSERT_PTHREAD(pthread_mutex_lock(&state_mutex))
#define UNLOCK()    ASSERT_PTHREAD(pthread_mutex_unlock(&state_mutex))




/* These manage the conversion between bit and position multiplexer register
 * settings and sensible user readable names.
 *
 * For multiplexer selections we convert the register value to and from a
 * multiplexer name and read and write the corresponding register.  The main
 * complication is that we need to map multiplexers to indexes. */

struct mux_lookup {
    size_t length;                  // Number of mux entries
    struct hash_table *numbers;     // Lookup converting name to index
    char **names;                   // Array of mux entry names
};


/* For bit and pos out classes we read all the values together and record the
 * corresponding change indexes.  This means we need a global state structure to
 * record the last reading, together with index information for each class index
 * to identify the corresponding fields per class. */

static struct {
    struct mux_lookup lookup;       // Map between mux index and names
    bool bits[BIT_BUS_COUNT];       // Current value of each bit
    uint64_t change_index[BIT_BUS_COUNT];   // Change flag for each bit
    uint32_t capture[BIT_BUS_COUNT / 32];   // Capture request for each bit
    int capture_index[BIT_BUS_COUNT / 32];  // Capture index for each bit
} bit_out_state = { };

static struct {
    struct mux_lookup lookup;       // Map between mux index and names
    uint32_t positions[POS_BUS_COUNT];      // Current array of positions
    uint64_t change_index[POS_BUS_COUNT];   // Change flag for each position
    uint32_t capture;                   // Capture request for each position
    int capture_index[POS_BUS_COUNT];   // Capture index for each position
} pos_out_state = { };



/*****************************************************************************/
/* Multiplexer name lookup. */


/* Creates Mux lookup to support the given number of valid indexes. */
static void mux_lookup_initialise(struct mux_lookup *lookup, size_t length)
{
    *lookup = (struct mux_lookup) {
        .length = length,
        .numbers = hash_table_create(false),
        .names = calloc(length, sizeof(char *)),
    };
}


static void mux_lookup_destroy(struct mux_lookup *lookup)
{
    if (lookup->numbers)
        hash_table_destroy(lookup->numbers);
    if (lookup->names)
    {
        for (unsigned int i = 0; i < lookup->length; i ++)
            free(lookup->names[i]);
        free(lookup->names);
    }
}


/* Add name<->index mapping, called during configuration file parsing. */
static error__t mux_lookup_insert(
    struct mux_lookup *lookup, unsigned int ix, const char *name)
{
    return
        TEST_OK_(ix < lookup->length, "Index %u out of range", ix)  ?:
        TEST_OK_(!lookup->names[ix], "Index %u already assigned", ix)  ?:
        DO(lookup->names[ix] = strdup(name))  ?:
        TEST_OK_(!hash_table_insert(
            lookup->numbers, lookup->names[ix], (void *) (uintptr_t) ix),
            "Duplicate mux name %s", name);
}


/* During register definition parsing add index<->name conversions. */
static error__t add_mux_indices(
    struct mux_lookup *lookup,
    const char *block_name, const char *field_name, unsigned int count,
    unsigned int indices[])
{
    /* Add mux entries for our instances. */
    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < count; i ++)
    {
        char name[MAX_NAME_LENGTH];
        snprintf(name, sizeof(name), "%s%d.%s", block_name, i + 1, field_name);
        error = mux_lookup_insert(lookup, indices[i], name);
    }
    return error;
}


/* Converts field name to corresponding index. */
static error__t mux_lookup_name(
    struct mux_lookup *lookup, const char *name, unsigned int *ix)
{
    void *value;
    return
        TEST_OK_(hash_table_lookup_bool(lookup->numbers, name, &value),
            "Mux selector not known")  ?:
        DO(*ix = (unsigned int) (uintptr_t) value);
}


/* Converts register index to multiplexer name, or returns error if an invalid
 * value is read. */
static error__t mux_lookup_index(
    struct mux_lookup *lookup, unsigned int ix, char result[], size_t length)
{
    return
        TEST_OK_(ix < lookup->length, "Index out of range")  ?:
        TEST_OK_(lookup->names[ix], "Mux name unassigned")  ?:
        TEST_OK(strlen(lookup->names[ix]) < length)  ?:
        DO(strcpy(result, lookup->names[ix]));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* bit_mux and pos_mux type methods. */

error__t bit_mux_format(
    void *type_data, unsigned int number,
    unsigned int value, char result[], size_t length)
{
    return mux_lookup_index(&bit_out_state.lookup, value, result, length);
}

error__t pos_mux_format(
    void *type_data, unsigned int number,
    unsigned int value, char result[], size_t length)
{
    return mux_lookup_index(&pos_out_state.lookup, value, result, length);
}


error__t bit_mux_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
{
    return mux_lookup_name(&bit_out_state.lookup, string, value);
}

error__t pos_mux_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
{
    return mux_lookup_name(&pos_out_state.lookup, string, value);
}



/*****************************************************************************/
/* Class initialisation. */

struct capture_state {
    unsigned int count;
    struct type *type;
    unsigned int index_array[];
};


static error__t capture_init(
    const struct register_methods *register_methods, const char *type_name,
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data)
{
    struct capture_state *state =
        malloc(sizeof(struct capture_state) + count * sizeof(unsigned int));

    *state = (struct capture_state) { .count = count, };
    for (unsigned int i = 0; i < count; i ++)
        state->index_array[i] = UNASSIGNED_REGISTER;
    *class_data = state;

    return create_type(
        &type_name, NULL, count, register_methods, state,
        attr_map, &state->type);
}


static uint32_t bit_out_read(void *reg_data, unsigned int number)
{
    struct capture_state *state = reg_data;
    return bit_out_state.bits[state->index_array[number]];
}

static uint32_t pos_out_read(void *reg_data, unsigned int number)
{
    struct capture_state *state = reg_data;
    return pos_out_state.positions[state->index_array[number]];
}

static const struct register_methods bit_out_methods = {
    .read = bit_out_read,
};

static const struct register_methods pos_out_methods = {
    .read = pos_out_read,
};


static error__t bit_out_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data)
{
    return capture_init(
        &bit_out_methods, "bit", line, count, attr_map, class_data);
}

static error__t pos_out_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data)
{
    return capture_init(
        &pos_out_methods, "position", line, count, attr_map, class_data);
}


/* For validation ensure that an index has been assigned to each field. */
static error__t capture_finalise(void *class_data, unsigned int block_base)
{
    struct capture_state *state = class_data;
    for (unsigned int i = 0; i < state->count; i ++)
        if (state->index_array[i] == UNASSIGNED_REGISTER)
            return FAIL_("Output selector not assigned");
    return ERROR_OK;
}


static void capture_destroy(void *class_data)
{
    struct capture_state *state = class_data;
    destroy_type(state->type);
}


/* We fill in the index array and create name lookups at the same time. */

static error__t check_out_unassigned(struct capture_state *state)
{
    /* Check that we're starting with an unassigned field set. */
    for (unsigned int i = 0; i < state->count; i ++)
        if (state->index_array[i] != UNASSIGNED_REGISTER)
            return FAIL_("Output selection already assigned");
    return ERROR_OK;
}

static error__t parse_out_registers(
    struct capture_state *state, const char **line, size_t limit)
{
    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < state->count; i ++)
        error =
            parse_whitespace(line)  ?:
            parse_uint(line, &state->index_array[i])  ?:
            TEST_OK_(state->index_array[i] < limit, "Mux index out of range");
    return error;
}

static error__t capture_parse_register(
    struct mux_lookup *lookup, struct capture_state *state,
    const char *block_name, const char *field_name, const char **line)
{
    return
        check_out_unassigned(state)  ?:
        parse_out_registers(state, line, lookup->length) ?:
        add_mux_indices(
            lookup, block_name, field_name, state->count, state->index_array);
}

static error__t bit_out_parse_register(
    void *class_data, const char *block_name, const char *field_name,
    const char **line)
{
    return capture_parse_register(
        &bit_out_state.lookup, class_data, block_name, field_name, line);
}

static error__t pos_out_parse_register(
    void *class_data, const char *block_name, const char *field_name,
    const char **line)
{
    return capture_parse_register(
        &pos_out_state.lookup, class_data, block_name, field_name, line);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Class value access (classes are read only). */

/* The refresh method is called when we need a fresh value.  We retrieve values
 * and changed bits from the hardware and update settings accordingly. */

static void bit_out_refresh(void *class_data, unsigned int number)
{
    LOCK();
    uint64_t change_index = get_change_index();
    bool changes[BIT_BUS_COUNT];
    hw_read_bits(bit_out_state.bits, changes);
    for (unsigned int i = 0; i < BIT_BUS_COUNT; i ++)
        if (changes[i])
            bit_out_state.change_index[i] = change_index;
    UNLOCK();
}

static void pos_out_refresh(void *class_data, unsigned int number)
{
    LOCK();
    uint64_t change_index = get_change_index();
    bool changes[POS_BUS_COUNT];
    hw_read_positions(pos_out_state.positions, changes);
    for (unsigned int i = 0; i < POS_BUS_COUNT; i ++)
        if (changes[i])
            pos_out_state.change_index[i] = change_index;
    UNLOCK();
}

void do_bit_out_refresh(void) { bit_out_refresh(NULL, 0); }
void do_pos_out_refresh(void) { pos_out_refresh(NULL, 0); }


/* When reading just return the current value from our static state. */

static error__t capture_get(
    void *class_data, unsigned int number, struct connection_result *result)
{
    struct capture_state *state = class_data;
    return type_get(state->type, number, result);
}


/* Computation of change set. */
static void bit_pos_change_set(
    struct capture_state *state, const uint64_t change_index[],
    const uint64_t report_index, bool changes[])
{
    for (unsigned int i = 0; i < state->count; i ++)
        changes[i] = change_index[state->index_array[i]] >= report_index;
}

static void bit_out_change_set(
    void *class_data, const uint64_t report_index[], bool changes[])
{
    bit_pos_change_set(
        class_data, bit_out_state.change_index,
        report_index[CHANGE_IX_BITS], changes);
}

static void pos_out_change_set(
    void *class_data, const uint64_t report_index[], bool changes[])
{
    bit_pos_change_set(
        class_data, pos_out_state.change_index,
        report_index[CHANGE_IX_POSITION], changes);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Attributes. */


/* Update the bit and pos capture index arrays.  This needs to be called each
 * time either capture mask is written. */
static void update_capture_index(void)
{
    int capture_index = 0;

    /* Position capture. */
    for (unsigned int i = 0; i < POS_BUS_COUNT; i ++)
        if (pos_out_state.capture & (1U << i))
            pos_out_state.capture_index[i] = capture_index++;
        else
            pos_out_state.capture_index[i] = -1;

    /* Bit capture. */
    for (unsigned int i = 0; i < BIT_BUS_COUNT / 32; i ++)
        if (bit_out_state.capture[i])
            bit_out_state.capture_index[i] = capture_index++;
        else
            bit_out_state.capture_index[i] = -1;
}


static error__t bit_out_capture_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct capture_state *state = data;
    unsigned int ix = state->index_array[number];
    bool capture = bit_out_state.capture[ix / 32] & (1U << (ix % 32));
    return format_string(result, length, "%d", capture);
}

static error__t pos_out_capture_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct capture_state *state = data;
    unsigned int ix = state->index_array[number];
    bool capture = pos_out_state.capture & (1U << (ix % 32));
    return format_string(result, length, "%d", capture);
}


static void update_bit(uint32_t *target, unsigned int ix, bool value)
{
    if (value)
        *target |= 1U << ix;
    else
        *target &= ~(1U << ix);
}

static error__t bit_out_capture_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct capture_state *state = data;
    unsigned int ix = state->index_array[number];

    bool capture;
    error__t error =
        parse_bit(&value, &capture)  ?:
        parse_eos(&value);

    if (!error)
    {
        update_bit(&bit_out_state.capture[ix / 32], ix % 32, capture);
        uint32_t capture_mask = 0;
        for (unsigned int i = 0; i < BIT_BUS_COUNT / 32; i ++)
            capture_mask |= (uint32_t) (bool) bit_out_state.capture[i] << i;
        hw_write_bit_capture(capture_mask);
        update_capture_index();
    }
    return error;
}

static error__t pos_out_capture_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct capture_state *state = data;
    unsigned int ix = state->index_array[number];

    bool capture;
    error__t error =
        parse_bit(&value, &capture)  ?:
        parse_eos(&value);

    if (!error)
    {
        update_bit(&pos_out_state.capture, ix, capture);
        hw_write_position_capture(pos_out_state.capture);
        update_capture_index();
    }
    return error;
}

static error__t bit_out_index_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct capture_state *state = data;
    unsigned int ix = state->index_array[number];

    int capture_index = bit_out_state.capture_index[ix / 32];
    return
        IF_ELSE(capture_index >= 0,
            format_string(result, length, "%d:%d", capture_index, ix % 32),
        // else
            DO(*result = '\0'));
}

static error__t pos_out_index_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct capture_state *state = data;
    unsigned int ix = state->index_array[number];

    int capture_index = pos_out_state.capture_index[ix];
    return
        IF_ELSE(capture_index >= 0,
            format_string(result, length, "%d", capture_index),
        // else
            DO(*result = '\0'));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Capture enumeration. */

void report_capture_list(struct connection_result *result)
{
    /* Position capture. */
    for (unsigned int i = 0; i < POS_BUS_COUNT; i ++)
        if (pos_out_state.capture & (1U << i))
            result->write_many(
                result->write_context, pos_out_state.lookup.names[i]);

    /* Bit capture. */
    for (unsigned int i = 0; i < BIT_BUS_COUNT / 32; i ++)
        if (bit_out_state.capture[i])
        {
            char string[MAX_NAME_LENGTH];
            snprintf(string, sizeof(string), "*BITS%d", i);
            result->write_many(result->write_context, string);
        }

    result->response = RESPONSE_MANY;
}


void report_capture_bits(struct connection_result *result, unsigned int group)
{
    for (unsigned int i = 0; i < 32; i ++)
        result->write_many(result->write_context,
            bit_out_state.lookup.names[32*group + i] ?: "");
    result->response = RESPONSE_MANY;
}


void report_capture_positions(struct connection_result *result)
{
    for (unsigned int i = 0; i < POS_BUS_COUNT; i ++)
        result->write_many(result->write_context,
            pos_out_state.lookup.names[i] ?: "");
    result->response = RESPONSE_MANY;
}


void reset_capture_list(void)
{
    for (unsigned int i = 0; i < BIT_BUS_COUNT / 32; i ++)
        bit_out_state.capture[i] = 0;
    pos_out_state.capture = 0;
    update_capture_index();
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Startup and shutdown. */


error__t initialise_capture(void)
{
    /* Block input multiplexer maps.  These are initialised by each
     * {bit,pos}_out field as it is loaded. */
    mux_lookup_initialise(&bit_out_state.lookup, BIT_BUS_COUNT);
    mux_lookup_initialise(&pos_out_state.lookup, POS_BUS_COUNT);
    update_capture_index();
    return ERROR_OK;
}


void terminate_capture(void)
{
    mux_lookup_destroy(&bit_out_state.lookup);
    mux_lookup_destroy(&pos_out_state.lookup);
}


const struct class_methods bit_out_class_methods = {
    "bit_out",
    .init = bit_out_init,
    .parse_register = bit_out_parse_register,
    .finalise = capture_finalise,
    .destroy = capture_destroy,
    .get = capture_get, .refresh = bit_out_refresh,
    .change_set = bit_out_change_set,
    .attrs = (struct attr_methods[]) {
        { "CAPTURE", true,
            .format = bit_out_capture_format,
            .put = bit_out_capture_put,
        },
        { "CAPTURE_INDEX",
            .format = bit_out_index_format,
        },
    },
    .attr_count = 2,
};

const struct class_methods pos_out_class_methods = {
    "pos_out",
    .init = pos_out_init,
    .parse_register = pos_out_parse_register,
    .finalise = capture_finalise,
    .destroy = capture_destroy,
    .get = capture_get, .refresh = pos_out_refresh,
    .change_set = pos_out_change_set,
    .attrs = (struct attr_methods[]) {
        { "CAPTURE", true,
            .format = pos_out_capture_format,
            .put = pos_out_capture_put,
        },
        { "CAPTURE_INDEX",
            .format = pos_out_index_format,
        },
    },
    .attr_count = 2,
};
