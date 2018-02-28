/* Implementation of time class. */

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#include "error.h"
#include "hardware.h"
#include "parse.h"
#include "config_server.h"
#include "attributes.h"
#include "fields.h"
#include "types.h"
#include "enums.h"
#include "locking.h"

#include "time.h"


#define TIME_SECS   1

static const struct enum_set time_units_enum_set = {
    .enums = (const struct enum_entry[]) {
        { 0, "min" },
        { 1, "s" },
        { 2, "ms" },
        { 3, "us" },
    },
    .count = 4,
};

static const double time_conversion[] =
{
    (double) 60 * CLOCK_FREQUENCY,      // TIME_MINS
    CLOCK_FREQUENCY,                    // TIME_SECS
    CLOCK_FREQUENCY / 1e3,              // TIME_MSECS
    CLOCK_FREQUENCY / 1e6,              // TIME_USECS
};

static const struct enumeration *time_units_enumeration;


struct time_class_state {
    unsigned int block_base;            // Base address for block
    unsigned int low_register;          // low 32-bits of value
    unsigned int high_register;         // high 16-bits of value
    unsigned int count;                 // Number of instances of this block
    pthread_mutex_t mutex;              // Interlock for block access

    /* If min_value is set then the range of values [1..min_value] will be
     * forbidden.  This is used to assist the hardware. */
    uint64_t min_value;                 // Minimum valid value less 1

    struct time_field {
        unsigned int time_scale;        // Scaling factor selection (enum ix)
        uint64_t value;                 // Current value
        uint64_t update_index;          // Timestamp of last update
    } values[];
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation. */


static error__t time_class_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data,
    struct indent_parser *parser)
{
    size_t fields_size = count * sizeof(struct time_field);
    struct time_class_state *state =
        malloc(sizeof(struct time_class_state) + fields_size);
    *state = (struct time_class_state) {
        .block_base = UNASSIGNED_REGISTER,
        .low_register = UNASSIGNED_REGISTER,
        .high_register = UNASSIGNED_REGISTER,
        .count = count,
        .mutex = PTHREAD_MUTEX_INITIALIZER,
    };
    for (unsigned int i = 0; i < count; i ++)
        state->values[i] = (struct time_field) {
            .time_scale = TIME_SECS,
            .update_index = 1,
        };
    *class_data = state;
    return ERROR_OK;
}


/* Expects a pair of registers: low bits then high bits. */
static error__t time_parse_register(
    void *class_data, struct field *field, unsigned int block_base,
    const char **line)
{
    struct time_class_state *state = class_data;
    state->block_base = block_base;
    return
        parse_whitespace(line)  ?:
        check_parse_register(field, line, &state->low_register)  ?:
        parse_whitespace(line)  ?:
        check_parse_register(field, line, &state->high_register)  ?:
        IF(**line != '\0',
            parse_whitespace(line)  ?:
            parse_char(line, '>')  ?:
            parse_uint64(line, &state->min_value));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Value access. */


static error__t time_class_format(
    uint64_t value, unsigned int scale, char result[], size_t length)
{
    return format_double(
        result, length, (double) value / time_conversion[scale]);
}


static error__t time_get(
    void *class_data, unsigned int number, char result[], size_t length)
{
    struct time_class_state *state = class_data;
    struct time_field *field = &state->values[number];

    LOCK(state->mutex);
    uint64_t value = field->value;
    UNLOCK(state->mutex);
    return time_class_format(value, field->time_scale, result, length);
}


static error__t write_time_value(
    void *class_data, unsigned int number, uint64_t value)
{
    struct time_class_state *state = class_data;
    error__t error =
        TEST_OK_(value == 0  ||  value > state->min_value, "Value too small");
    if (!error)
    {
        LOCK(state->mutex);
        hw_write_register(
            state->block_base, number, state->low_register,
            (uint32_t) value);
        hw_write_register(
            state->block_base, number, state->high_register,
            (uint32_t) (value >> 32));

        struct time_field *field = &state->values[number];
        field->value = value;
        field->update_index = get_change_index();
        UNLOCK(state->mutex);
    }
    return error;
}


static error__t time_class_parse(
    const char **string, unsigned int scale,
    uint64_t max_value, uint64_t *result)
{
    double scaled_value;
    double value;
    return
        parse_double(string, &scaled_value)  ?:
        /* The obvious thing to do here is simply to call llround() on the
         * result of the calculation below and detect range overflow ... good
         * luck with that, seems that whether overflow is actually reported is
         * target dependent, and doesn't work for us.  Ho hum. */
        DO(value = scaled_value * time_conversion[scale])  ?:
        TEST_OK_(0 <= value  &&  value <= max_value,
            "Time setting out of range")  ?:
        DO(*result = (uint64_t) llround(value));
}


static error__t time_put(
    void *class_data, unsigned int number, const char *string)
{
    struct time_class_state *state = class_data;
    uint64_t result;
    return
        time_class_parse(
            &string, state->values[number].time_scale,
            MAX_CLOCK_VALUE, &result)  ?:
        parse_eos(&string)  ?:
        write_time_value(state, number, result);
}


static void time_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct time_class_state *state = class_data;
    LOCK(state->mutex);
    for (unsigned int i = 0; i < state->count; i ++)
        changes[i] = state->values[i].update_index > report_index;
    UNLOCK(state->mutex);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Attributes. */


/* block.time.RAW? */
static error__t time_raw_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct time_class_state *state = class_data;
    struct time_field *field = &state->values[number];

    LOCK(state->mutex);
    uint64_t value = field->value;
    UNLOCK(state->mutex);

    return format_string(result, length, "%"PRIu64, value);
}


/* block.time.RAW=string */
static error__t time_raw_put(
    void *owner, void *class_data, unsigned int number, const char *string)
{
    struct time_class_state *state = class_data;
    uint64_t value;
    return
        parse_uint64(&string, &value)  ?:
        parse_eos(&string)  ?:
        write_time_value(state, number, value);
}


/* block.time.UNITS? */
static error__t shared_units_format(
    unsigned int scale, char result[], size_t length)
{
    const char *units;
    return
        TEST_OK(units = enum_index_to_name(time_units_enumeration, scale))  ?:
        format_string(result, length, "%s", units);
}

static error__t time_class_units_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct time_class_state *state = class_data;
    return shared_units_format(
        state->values[number].time_scale, result, length);
}


/* block.time.UNITS=string */
static error__t shared_units_parse(const char *string, unsigned int *scale)
{
    return TEST_OK_(enum_name_to_index(time_units_enumeration, string, scale),
        "Invalid time units");
}

static error__t time_class_units_put(
    void *owner, void *class_data, unsigned int number, const char *string)
{
    unsigned int scale = 0;
    error__t error = shared_units_parse(string, &scale);
    if (!error)
    {
        struct time_class_state *state = class_data;
        LOCK(state->mutex);
        state->values[number].time_scale = scale;
        state->values[number].update_index = get_change_index();
        UNLOCK(state->mutex);
    }
    return error;
}


static error__t time_min_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct time_class_state *state = class_data;
    struct time_field *field = &state->values[number];
    return format_double(
        result, length,
        (double) (state->min_value + 1) / time_conversion[field->time_scale]);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Time. */

/* The semantics of this code is very similar to that of time_class, but here
 * we're working at the type level with 32-bit values. */

struct time_type_state {
    unsigned int scale[0];
};


static error__t time_type_init(
    const char **string, unsigned int count, void **type_data,
    struct indent_parser *parser)
{
    struct time_type_state *state = malloc(
        sizeof(struct time_type_state) + count * sizeof(unsigned int));
    *state = (struct time_type_state) { };
    for (unsigned int i = 0; i < count; i ++)
        state->scale[i] = TIME_SECS;
    *type_data = state;
    return ERROR_OK;
}


static error__t time_parse(
    void *type_data, unsigned int number,
    const char **string, unsigned int *value)
{
    struct time_type_state *state = type_data;
    uint64_t result;
    return
        time_class_parse(string, state->scale[number], UINT32_MAX, &result)  ?:
        DO(*value = (unsigned int) result);
}


static error__t time_format(
    void *type_data, unsigned int number,
    unsigned int value, char result[], size_t length)
{
    struct time_type_state *state = type_data;
    return time_class_format(value, state->scale[number], result, length);
}


static error__t time_type_units_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct time_type_state *state = data;
    return shared_units_format(state->scale[number], result, length);
}


static error__t time_type_units_put(
    void *owner, void *data, unsigned int number, const char *string)
{
    unsigned int scale = 0;
    error__t error = shared_units_parse(string, &scale);
    if (!error)
    {
        struct time_type_state *state = data;
        state->scale[number] = scale;
        changed_type_register(owner, number);
    }
    return error;
}


static const struct enumeration *time_units_get_enumeration(void *data)
{
    return time_units_enumeration;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Class and type definitions. */

error__t initialise_time(void)
{
    time_units_enumeration = create_static_enumeration(&time_units_enum_set);
    return ERROR_OK;
}

void terminate_time(void)
{
    if (time_units_enumeration)
        destroy_enumeration(time_units_enumeration);
}


const struct class_methods time_class_methods = {
    "time",
    .init = time_class_init,
    .parse_register = time_parse_register,
    .get = time_get,
    .put = time_put,
    .change_set = time_change_set,
    .change_set_index = CHANGE_IX_CONFIG,
    .attrs = (struct attr_methods[]) {
        { "RAW", "Time in ticks",
            .format = time_raw_format,
            .put = time_raw_put,
        },
        { "UNITS", "Units of time setting",
            .in_change_set = true,
            .format = time_class_units_format,
            .put = time_class_units_put,
            .get_enumeration = time_units_get_enumeration,
        },
        { "MIN", "Minimum programmable time",
            .format = time_min_format,
        },
    },
    .attr_count = 3,
};


const struct type_methods time_type_methods = {
    "time",
    .init = time_type_init,
    .parse = time_parse, .format = time_format,
    .attrs = (struct attr_methods[]) {
        { "RAW", "Time in ticks",
            .format = raw_format_uint,
            .put = raw_put_uint,
        },
        { "UNITS", "Units of time setting",
            .in_change_set = true,
            .format = time_type_units_format,
            .put = time_type_units_put,
            .get_enumeration = time_units_get_enumeration,
        },
    },
    .attr_count = 2,
};
