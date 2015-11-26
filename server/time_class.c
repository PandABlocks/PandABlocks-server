/* Implementation of time class. */

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include "error.h"
#include "hardware.h"
#include "parse.h"
#include "config_server.h"
#include "classes.h"
#include "attributes.h"
#include "types.h"

#include "time_class.h"



enum time_scale { TIME_MINS, TIME_SECS, TIME_MSECS, TIME_USECS, };

struct time_state {
    unsigned int block_base;
    unsigned int low_register;
    unsigned int high_register;
    unsigned int count;
    struct time_field {
        enum time_scale time_scale;
        uint64_t value;
        uint64_t update_index;
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
    };
    for (unsigned int i = 0; i < count; i ++)
        state->values[i] = (struct time_field) { .time_scale = TIME_SECS, };
    *class_data = state;
    return ERROR_OK;
}


/* Expects a pair of registers: low bits then high bits. */
static error__t time_parse_register(
    void *class_data, const char *block_name, const char *field_name,
    const char **line)
{
    struct time_state *state = class_data;
    return
        TEST_OK_(state->low_register == UNASSIGNED_REGISTER,
            "Register already assigned")  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &state->low_register)  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &state->high_register);
}


static error__t time_finalise(void *class_data, unsigned int block_base)
{
    struct time_state *state = class_data;
    state->block_base = block_base;
    return
        // Don't need to check high_register, they're assigned together
        TEST_OK_(state->low_register != UNASSIGNED_REGISTER,
            "No register assigned to field");
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Value access. */


static error__t time_get(
    void *class_data, unsigned int number, struct connection_result *result)
{
    struct time_state *state = class_data;

    double conversion = time_conversion[state->values[number].time_scale];
    return
        format_double(
            result->string, result->length,
            (double) state->values[number].value / conversion)  ?:
        DO(result->response = RESPONSE_ONE);
}


static void write_time_value(
    void *class_data, unsigned int number, uint64_t value)
{
    struct time_state *state = class_data;

    hw_write_register(
        state->block_base, number, state->low_register,
        (uint32_t) value);
    hw_write_register(
        state->block_base, number, state->high_register,
        (uint32_t) (value >> 32));

    state->values[number].value = value;
    state->values[number].update_index = get_change_index();
}


static error__t time_put(
    void *class_data, unsigned int number, const char *string)
{
    struct time_state *state = class_data;

    double conversion = time_conversion[state->values[number].time_scale];
    double scaled_value;
    double value;
    return
        parse_double(&string, &scaled_value)  ?:
        parse_eos(&string)  ?:
        DO(value = scaled_value * conversion)  ?:
        TEST_OK_(0 <= value  &&  value <= MAX_CLOCK_VALUE,
            "Time setting out of range")  ?:
        DO(write_time_value(state, number, (uint64_t) llround(value)));
}


static void time_change_set(
    void *class_data, const uint64_t report_index[], bool changes[])
{
    struct time_state *state = class_data;
    uint64_t report = report_index[CHANGE_IX_CONFIG];
    for (unsigned int i = 0; i < state->count; i ++)
        changes[i] = state->values[i].update_index >= report;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Attributes. */


/* block.time.RAW? */
static error__t time_raw_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct time_state *state = class_data;
    return format_string(
        result, length, "%"PRIu64, state->values[number].value);
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
        DO(write_time_value(state, number, value));
}


/* block.time.UNITS? */
static error__t time_units_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct time_state *state = class_data;
    return format_string(
        result, length, "%s", time_units[state->values[number].time_scale]);
}


/* block.time.UNITS=string */
static error__t time_units_put(
    void *owner, void *class_data, unsigned int number, const char *string)
{
    struct time_state *state = class_data;
    for (unsigned int i = 0; i < ARRAY_SIZE(time_units); i ++)
        if (strcmp(string, time_units[i]) == 0)
        {
            state->values[number].time_scale = (enum time_scale) i;
            return ERROR_OK;
        }
    return FAIL_("Invalid time units");
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
    .attrs = (struct attr_methods[]) {
        { "RAW",
            .format = time_raw_format,
            .put = time_raw_put,
        },
        { "UNITS", true,
            .format = time_units_format,
            .put = time_units_put,
        },
    },
    .attr_count = 2,
};
