/* Implementation of time class. */

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <pthread.h>

#include "error.h"
#include "hardware.h"
#include "parse.h"
#include "config_server.h"
#include "classes.h"
#include "attributes.h"
#include "types.h"
#include "locking.h"

#include "time_class.h"



struct time_state {
    unsigned int block_base;            // Base address for block
    unsigned int low_register;          // low 32-bits of value
    unsigned int high_register;         // high 16-bits of value
    unsigned int count;                 // Number of instances of this block
    pthread_mutex_t mutex;              // Interlock for block access

    /* If min_value is set then the range of values [1..min_value] will be
     * forbidden.  This is used to assist the hardware. */
    uint64_t min_value;                 // Minimum valid value less 1

    struct time_field {
        enum time_scale time_scale;         // Scaling factor selection
        uint64_t value;                     // Current value
        uint64_t update_index;              // Timestamp of last update
    } values[];
};

static const double time_conversion[] =
{
    (double) 60 * CLOCK_FREQUENCY,      // TIME_MINS
    CLOCK_FREQUENCY,                    // TIME_SECS
    CLOCK_FREQUENCY / 1e3,              // TIME_MSECS
    CLOCK_FREQUENCY / 1e6,              // TIME_USECS
};

static const char *time_units[] = { "min", "s", "ms", "us", };


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation. */


static error__t time_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data)
{
    size_t fields_size = count * sizeof(struct time_field);
    struct time_state *state = malloc(sizeof(struct time_state) + fields_size);
    *state = (struct time_state) {
        .block_base = UNASSIGNED_REGISTER,
        .low_register = UNASSIGNED_REGISTER,
        .high_register = UNASSIGNED_REGISTER,
        .count = count,
        .mutex = PTHREAD_MUTEX_INITIALIZER,
    };
    for (unsigned int i = 0; i < count; i ++)
        state->values[i] = (struct time_field) {
            .time_scale = TIME_SECS,
        };
    *class_data = state;
    return ERROR_OK;
}


/* Expects a pair of registers: low bits then high bits. */
static error__t time_parse_register(
    void *class_data, struct field *field, const char **line)
{
    struct time_state *state = class_data;
    return
        TEST_OK_(state->low_register == UNASSIGNED_REGISTER,
            "Register already assigned")  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &state->low_register)  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &state->high_register)  ?:
        IF(**line != '\0',
            parse_whitespace(line)  ?:
            parse_char(line, '>')  ?:
            parse_uint64(line, &state->min_value));
}


static error__t time_finalise(void *class_data, unsigned int block_base)
{
    struct time_state *state = class_data;
    state->block_base = block_base;
    return ERROR_OK;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Value access. */


error__t time_class_format(
    uint64_t value, enum time_scale scale, char result[], size_t length)
{
    double conversion = time_conversion[scale];
    return format_double(result, length, (double) value / conversion);
}


static error__t time_get(
    void *class_data, unsigned int number, struct connection_result *result)
{
    struct time_state *state = class_data;
    struct time_field *field = &state->values[number];

    LOCK(state->mutex);
    uint64_t value = field->value;
    UNLOCK(state->mutex);
    return
        time_class_format(
            value, field->time_scale, result->string, result->length)  ?:
        DO(result->response = RESPONSE_ONE);
}


static error__t write_time_value(
    void *class_data, unsigned int number, uint64_t value)
{
    struct time_state *state = class_data;
    error__t error =
        TEST_OK_(value == 0  ||  value > state->min_value, "Value too small");
    if (!error)
    {
        /* A non-zero value is offset by min_value before being written to the
         * registers, but we store the raw uncompensated value for readback. */
        uint64_t write_value = value == 0 ? 0 : value - state->min_value;

        LOCK(state->mutex);
        hw_write_register(
            state->block_base, number, state->low_register,
            (uint32_t) write_value);
        hw_write_register(
            state->block_base, number, state->high_register,
            (uint32_t) (write_value >> 32));

        struct time_field *field = &state->values[number];
        field->value = value;
        field->update_index = get_change_index();
        UNLOCK(state->mutex);
    }
    return error;
}


error__t time_class_parse(
    const char *string, enum time_scale scale, uint64_t *result)
{
    double conversion = time_conversion[scale];
    double scaled_value;
    double value;
    return
        parse_double(&string, &scaled_value)  ?:
        parse_eos(&string)  ?:
        /* The obvious thing to do here is simply to call llround() on the
         * result of the calculation below and detect range overflow ... good
         * luck with that, seems that whether overflow is actually reported is
         * target dependent, and doesn't work for us.  Ho hum. */
        DO(value = scaled_value * conversion)  ?:
        TEST_OK_(0 <= value  &&  value <= MAX_CLOCK_VALUE,
            "Time setting out of range")  ?:
        DO(*result = (uint64_t) llround(value));
}


static error__t time_put(
    void *class_data, unsigned int number, const char *string)
{
    struct time_state *state = class_data;
    uint64_t result;
    return
        time_class_parse(
            string, state->values[number].time_scale, &result)  ?:
        write_time_value(state, number, result);
}


static void time_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct time_state *state = class_data;
    LOCK(state->mutex);
    for (unsigned int i = 0; i < state->count; i ++)
        changes[i] = state->values[i].update_index >= report_index;
    UNLOCK(state->mutex);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Attributes. */


/* block.time.RAW? */
static error__t time_raw_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct time_state *state = class_data;
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
    struct time_state *state = class_data;
    uint64_t value;
    return
        parse_uint64(&string, &value)  ?:
        parse_eos(&string)  ?:
        write_time_value(state, number, value);
}


/* block.time.UNITS? */
error__t time_class_units_format(
    enum time_scale scale, char result[], size_t length)
{
    return format_string(result, length, "%s", time_units[scale]);
}

static error__t time_units_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct time_state *state = class_data;
    return time_class_units_format(
        state->values[number].time_scale, result, length);
}


/* block.time.UNITS=string */
error__t time_class_units_parse(const char *string, enum time_scale *scale)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(time_units); i ++)
        if (strcmp(string, time_units[i]) == 0)
        {
            *scale = (enum time_scale) i;
            return ERROR_OK;
        }
    return FAIL_("Invalid time units");
}

static error__t time_units_put(
    void *owner, void *class_data, unsigned int number, const char *string)
{
    enum time_scale scale = 0;
    error__t error = time_class_units_parse(string, &scale);
    if (!error)
    {
        struct time_state *state = class_data;
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
    struct time_state *state = class_data;
    struct time_field *field = &state->values[number];
    double conversion = time_conversion[field->time_scale];
    return format_double(
        result, length, (double) (state->min_value + 1) / conversion);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

const struct class_methods time_class_methods = {
    "time",
    .init = time_init,
    .parse_register = time_parse_register,
    .finalise = time_finalise,
    .get = time_get,
    .put = time_put,
    .change_set = time_change_set,
    .change_set_index = CHANGE_IX_CONFIG,
    .attrs = (struct attr_methods[]) {
        { "RAW",
            .format = time_raw_format,
            .put = time_raw_put,
        },
        { "UNITS", true,
            .format = time_units_format,
            .put = time_units_put,
        },
        { "MIN",
            .format = time_min_format,
        },
    },
    .attr_count = 3,
};
