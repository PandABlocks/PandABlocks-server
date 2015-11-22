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

#include "capture.h"

#define UNASSIGNED_REGISTER ((unsigned int) -1)


static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK(x)     ASSERT_PTHREAD(pthread_mutex_lock(&state_mutex))
#define UNLOCK(x)   ASSERT_PTHREAD(pthread_mutex_unlock(&state_mutex))




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
        snprintf(name, sizeof(name), "%s%d.%s", block_name, i, field_name);
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

/* The class specific state is just the index array. */
void bit_pos_out_init(unsigned int count, void **class_data)
{
    unsigned int *index_array = malloc(count * sizeof(unsigned int));
    for (unsigned int i = 0; i < count; i ++)
        index_array[i] = UNASSIGNED_REGISTER;
    *class_data = index_array;
}


/* For validation ensure that an index has been assigned to each field. */
error__t bit_pos_out_validate(struct class *class)
{
    unsigned int *index_array = class->class_data;
    for (unsigned int i = 0; i < class->count; i ++)
        if (index_array[i] == UNASSIGNED_REGISTER)
            return FAIL_("Output selector not assigned");
    return ERROR_OK;
}


/* We fill in the index array and create name lookups at the same time. */

static error__t check_out_unassigned(
    unsigned int *index_array, unsigned int count)
{
    /* Check that we're starting with an unassigned field set. */
    for (unsigned int i = 0; i < count; i ++)
        if (index_array[i] != UNASSIGNED_REGISTER)
            return FAIL_("Output selection already assigned");
    return ERROR_OK;
}

static error__t parse_out_registers(
    const char **line, unsigned int count, size_t limit,
    unsigned int indices[])
{
    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < count; i ++)
        error =
            parse_whitespace(line)  ?:
            parse_uint(line, &indices[i])  ?:
            TEST_OK_(indices[i] < limit, "Mux index out of range");
    return error;
}

static error__t bit_pos_out_parse_register(
    struct mux_lookup *lookup,
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    unsigned int *index_array = class->class_data;
    return
        check_out_unassigned(index_array, class->count)  ?:
        parse_out_registers(line, class->count, lookup->length, index_array) ?:
        add_mux_indices(
            lookup, block_name, field_name, class->count, index_array);
}

error__t bit_out_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    return bit_pos_out_parse_register(
        &bit_out_state.lookup, class, block_name, field_name, line);
}

error__t pos_out_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    return bit_pos_out_parse_register(
        &pos_out_state.lookup, class, block_name, field_name, line);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Class value access (classes are read only). */

/* The refresh method is called when we need a fresh value.  We retrieve values
 * and changed bits from the hardware and update settings accordingly. */

void bit_out_refresh(struct class *class, unsigned int number)
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

void pos_out_refresh(struct class *class, unsigned int number)
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


/* When reading just return the current value from our static state. */

uint32_t bit_out_read(struct class *class, unsigned int number)
{
    unsigned int *index_array = class->class_data;
    return bit_out_state.bits[index_array[number]];
}

uint32_t pos_out_read(struct class *class, unsigned int number)
{
    unsigned int *index_array = class->class_data;
    return pos_out_state.positions[index_array[number]];
}


/* Computation of change set. */
static void bit_pos_change_set(
    struct class *class, const uint64_t change_index[],
    const uint64_t report_index, bool changes[])
{
    unsigned int *index_array = class->class_data;
    for (unsigned int i = 0; i < class->count; i ++)
        changes[i] = change_index[index_array[i]] >= report_index;
}

void bit_out_change_set(
    struct class *class, const uint64_t report_index[], bool changes[])
{
    bit_pos_change_set(
        class, bit_out_state.change_index,
        report_index[CHANGE_IX_BITS], changes);
}

void pos_out_change_set(
    struct class *class, const uint64_t report_index[], bool changes[])
{
    bit_pos_change_set(
        class, pos_out_state.change_index,
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


error__t bit_out_capture_format(
    struct class *class, void *data, unsigned int number,
    char result[], size_t length)
{
    unsigned int *index_array = data;
    unsigned int ix = index_array[number];
    bool capture = bit_out_state.capture[ix / 32] & (1U << (ix % 32));
    snprintf(result, length, "%d", capture);
    return ERROR_OK;
}

error__t pos_out_capture_format(
    struct class *class, void *data, unsigned int number,
    char result[], size_t length)
{
    unsigned int *index_array = data;
    unsigned int ix = index_array[number];
    bool capture = pos_out_state.capture & (1U << (ix % 32));
    snprintf(result, length, "%d", capture);
    return ERROR_OK;
}


static void update_bit(uint32_t *target, unsigned int ix, bool value)
{
    if (value)
        *target |= 1U << ix;
    else
        *target &= ~(1U << ix);
}

error__t bit_out_capture_put(
    struct class *class, void *data, unsigned int number, const char *value)
{
    bool capture;
    error__t error =
        parse_bit(&value, &capture)  ?:
        parse_eos(&value);

    if (!error)
    {
        unsigned int *index_array = data;
        unsigned int ix = index_array[number];

        update_bit(&bit_out_state.capture[ix / 32], ix % 32, capture);
        uint32_t capture_mask = 0;
        for (unsigned int i = 0; i < BIT_BUS_COUNT / 32; i ++)
            capture_mask |= (uint32_t) (bool) bit_out_state.capture[i] << i;
        hw_write_bit_capture(capture_mask);
        update_capture_index();
    }
    return error;
}

error__t pos_out_capture_put(
    struct class *class, void *data, unsigned int number, const char *value)
{
    bool capture;
    error__t error =
        parse_bit(&value, &capture)  ?:
        parse_eos(&value);

    if (!error)
    {
        unsigned int *index_array = data;
        unsigned int ix = index_array[number];

        update_bit(&pos_out_state.capture, ix, capture);
        hw_write_position_capture(pos_out_state.capture);
        update_capture_index();
    }
    return error;
}

error__t bit_out_index_format(
    struct class *class, void *data, unsigned int number,
    char result[], size_t length)
{
    unsigned int *index_array = data;
    unsigned int ix = index_array[number];
    int capture_index = bit_out_state.capture_index[ix / 32];
    if (capture_index >= 0)
        snprintf(result, length, "%d:%d", capture_index, ix % 32);
    else
        *result = '\0';
    return ERROR_OK;
}

error__t pos_out_index_format(
    struct class *class, void *data, unsigned int number,
    char result[], size_t length)
{
    unsigned int *index_array = data;
    unsigned int ix = index_array[number];
    int capture_index = pos_out_state.capture_index[ix];
    if (capture_index >= 0)
        snprintf(result, length, "%d", capture_index);
    else
        *result = '\0';
    return ERROR_OK;
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
