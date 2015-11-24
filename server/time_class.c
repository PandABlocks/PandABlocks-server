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
#include "types.h"

#include "time_class.h"


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Time definitions. */


enum time_scale { TIME_MINS, TIME_SECS, TIME_MSECS, TIME_USECS, };

struct time_state {
    enum time_scale time_scale;
    uint64_t value;
    uint64_t update_index;
};

static const double time_conversion[] =
{
    (double) 60 * CLOCK_FREQUENCY,      // TIME_MINS
    CLOCK_FREQUENCY,                    // TIME_SECS
    CLOCK_FREQUENCY / 1e3,              // TIME_MSECS
    CLOCK_FREQUENCY / 1e6,              // TIME_USECS
};

static const char *time_units[] = { "min", "s", "ms", "us", };


void time_init(unsigned int count, void **class_data)
{
    struct time_state *state = calloc(count, sizeof(struct time_state));
    for (unsigned int i = 0; i < count; i ++)
        state[i].time_scale = TIME_SECS;
    *class_data = state;
}


void time_change_set(
    struct class *class, const uint64_t report_index[], bool changes[])
{
    struct time_state *state = class->class_data;
    uint64_t report = report_index[CHANGE_IX_CONFIG];
    for (unsigned int i = 0; i < class->count; i ++)
        changes[i] = state[i].update_index >= report;
}


static void write_time_register(
    struct class *class, unsigned int number, uint64_t value)
{
    hw_write_register(
        class->block_base, number, class->field_register,
        (uint32_t) value);
    hw_write_register(
        class->block_base, number, class->field_register + 1,
        (uint32_t) (value >> 32));
}


error__t time_get(
    struct class *class, unsigned int number,
    struct connection_result *result)
{
    struct time_state *state = class->class_data;
    state = &state[number];

    double conversion = time_conversion[state->time_scale];
    return
        format_double(
            result->string, result->length,
            (double) state->value / conversion)  ?:
        DO(result->response = RESPONSE_ONE);
}


static void write_time_value(
    struct class *class, unsigned int number, uint64_t value)
{
    struct time_state *state = class->class_data;
    state = &state[number];
    state->value = value;
    write_time_register(class, number, value);
    state->update_index = get_change_index();
}


error__t time_put(
    struct class *class, unsigned int number, const char *string)
{
    struct time_state *state = class->class_data;
    state = &state[number];

    double conversion = time_conversion[state->time_scale];
    double scaled_value;
    double value;
    return
        parse_double(&string, &scaled_value)  ?:
        parse_eos(&string)  ?:
        DO(value = scaled_value * conversion)  ?:
        TEST_OK_(0 <= value  &&  value <= MAX_CLOCK_VALUE,
            "Time setting out of range")  ?:
        DO(write_time_value(class, number, (uint64_t) llround(value)));
}


error__t time_raw_format(
    struct class *class, void *data, unsigned int number,
    char result[], size_t length)
{
    struct time_state *state = class->class_data;
    state = &state[number];
    return format_string(result, length, "%"PRIu64, state->value);
}


error__t time_raw_put(
    struct class *class, void *data, unsigned int number, const char *string)
{
    uint64_t value;
    return
        parse_uint64(&string, &value)  ?:
        parse_eos(&string)  ?:
        DO(write_time_value(class, number, value));
}


error__t time_scale_format(
    struct class *class, void *data, unsigned int number,
    char result[], size_t length)
{
    struct time_state *state = class->class_data;
    state = &state[number];
    return format_string(result, length, "%s", time_units[state->time_scale]);
}


error__t time_scale_put(
    struct class *class, void *data, unsigned int number, const char *value)
{
    struct time_state *state = class->class_data;
    state = &state[number];
    for (unsigned int i = 0; i < ARRAY_SIZE(time_units); i ++)
        if (strcmp(value, time_units[i]) == 0)
        {
            state->time_scale = (enum time_scale) i;
            return ERROR_OK;
        }
    return FAIL_("Invalid time units");
}
