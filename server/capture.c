#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>

#include "error.h"
#include "hardware.h"
#include "parse.h"
#include "config_server.h"
#include "classes.h"
#include "mux_lookup.h"

#include "capture.h"

#define UNASSIGNED_REGISTER ((unsigned int) -1)


static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK(x)     ASSERT_PTHREAD(pthread_mutex_lock(&state_mutex))
#define UNLOCK(x)   ASSERT_PTHREAD(pthread_mutex_unlock(&state_mutex))




/* For bit and pos out classes we read all the values together and record the
 * corresponding change indexes.  This means we need a global state structure to
 * record the last reading, together with index information for each class index
 * to identify the corresponding fields per class. */

static struct {
    bool bits[BIT_BUS_COUNT];
    uint64_t change_index[BIT_BUS_COUNT];
    uint32_t capture[BIT_BUS_COUNT / 32];
} bit_out_state = { };

static struct {
    uint32_t positions[POS_BUS_COUNT];
    uint64_t change_index[POS_BUS_COUNT];
    uint32_t capture;
} pos_out_state = { };


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
    const char **line, unsigned int count, unsigned int limit,
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
    struct mux_lookup *lookup, unsigned int limit,
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    unsigned int *index_array = class->class_data;
    return
        check_out_unassigned(index_array, class->count)  ?:
        parse_out_registers(line, class->count, limit, index_array) ?:
        add_mux_indices(
            lookup, block_name, field_name, class->count, index_array);
}

error__t bit_out_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    return bit_pos_out_parse_register(
        bit_mux_lookup, BIT_BUS_COUNT, class, block_name, field_name, line);
}

error__t pos_out_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    return bit_pos_out_parse_register(
        pos_mux_lookup, POS_BUS_COUNT, class, block_name, field_name, line);
}


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
    }
    return error;
}

error__t bit_out_index_format(
    struct class *class, void *data, unsigned int number,
    char result[], size_t length)
{
    snprintf(result, length, "in a bit");
    return ERROR_OK;
}

error__t pos_out_index_format(
    struct class *class, void *data, unsigned int number,
    char result[], size_t length)
{
    snprintf(result, length, "in a bit");
    return ERROR_OK;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Capture enumeration. */

void report_capture_list(const struct connection_result *result)
{
    result->write_many_end(result->connection);
}

void report_capture_bits(
    const struct connection_result *result, unsigned int bit)
{

    result->write_many_end(result->connection);
}
