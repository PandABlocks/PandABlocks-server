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

#include "time_position.h"


enum time_scale { TIME_MINS, TIME_SECS, TIME_MSECS, TIME_USECS, };


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


static error__t time_class_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data)
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
        parse_uint(line, &state->low_register)  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &state->high_register)  ?:
        IF(**line != '\0',
            parse_whitespace(line)  ?:
            parse_char(line, '>')  ?:
            parse_uint64(line, &state->min_value));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Value access. */


static error__t time_class_format(
    uint64_t value, enum time_scale scale, char result[], size_t length)
{
    return format_double(
        result, length, (double) value / time_conversion[scale]);
}


static error__t time_get(
    void *class_data, unsigned int number, struct connection_result *result)
{
    struct time_class_state *state = class_data;
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
    const char *string, enum time_scale scale,
    uint64_t max_value, uint64_t *result)
{
    double scaled_value;
    double value;
    return
        parse_double(&string, &scaled_value)  ?:
        parse_eos(&string)  ?:
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
            string, state->values[number].time_scale,
            MAX_CLOCK_VALUE, &result)  ?:
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
    enum time_scale scale, char result[], size_t length)
{
    return format_string(result, length, "%s", time_units[scale]);
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
static error__t shared_units_parse(const char *string, enum time_scale *scale)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(time_units); i ++)
        if (strcmp(string, time_units[i]) == 0)
        {
            *scale = (enum time_scale) i;
            return ERROR_OK;
        }
    return FAIL_("Invalid time units");
}

static error__t time_class_units_put(
    void *owner, void *class_data, unsigned int number, const char *string)
{
    enum time_scale scale = 0;
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
/* Position. */

struct position_state {
    pthread_mutex_t mutex;
    struct position_field {
        double scale;
        double offset;
        char *units;
    } values[];
};


static error__t position_init(
    const char **string, unsigned int count, void **type_data)
{
    struct position_state *state = malloc(
        sizeof(struct position_state) + count * sizeof(struct position_field));
    *state = (struct position_state) {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
    };
    for (unsigned int i = 0; i < count; i ++)
        state->values[i] = (struct position_field) {
            .scale = 1.0,
        };
    *type_data = state;
    return ERROR_OK;
}

static void position_destroy(void *type_data, unsigned int count)
{
    struct position_state *state = type_data;
    for (unsigned int i = 0; i < count; i ++)
        free(state->values[i].units);
    free(state);
}


static error__t position_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
{
    struct position_state *state = type_data;
    struct position_field *field = &state->values[number];

    LOCK(state->mutex);
    double scale = field->scale;
    double offset = field->offset;
    UNLOCK(state->mutex);

    double position;
    double converted;
    return
        parse_double(&string, &position)  ?:
        parse_eos(&string)  ?:
        DO(converted = (position - offset) / scale)  ?:
        TEST_OK_(INT32_MIN <= converted  &&  converted <= INT32_MAX,
            "Position out of range")  ?:
        DO(*value = (unsigned int) lround(converted));
}


static error__t position_format(
    void *type_data, unsigned int number,
    unsigned int value, char result[], size_t length)
{
    struct position_state *state = type_data;
    struct position_field *field = &state->values[number];

    LOCK(state->mutex);
    double scale = field->scale;
    double offset = field->offset;
    UNLOCK(state->mutex);

    return format_double(result, length, (int) value * scale + offset);
}


static error__t position_scale_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct position_state *state = data;
    struct position_field *field = &state->values[number];

    LOCK(state->mutex);
    double scale = field->scale;
    UNLOCK(state->mutex);

    return format_double(result, length, scale);
}

static error__t position_scale_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct position_state *state = data;
    struct position_field *field = &state->values[number];

    double scale;
    error__t error = parse_double(&value, &scale)  ?:  parse_eos(&value);

    if (!error)
    {
        LOCK(state->mutex);
        field->scale = scale;
        UNLOCK(state->mutex);

        changed_register(owner, number);
    }
    return error;
}

static error__t position_offset_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct position_state *state = data;
    struct position_field *field = &state->values[number];

    LOCK(state->mutex);
    double offset = field->offset;
    UNLOCK(state->mutex);

    return format_double(result, length, offset);
}

static error__t position_offset_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct position_state *state = data;
    struct position_field *field = &state->values[number];

    double offset;
    error__t error = parse_double(&value, &offset)  ?: parse_eos(&value);
    if (!error)
    {
        LOCK(state->mutex);
        field->offset = offset;
        UNLOCK(state->mutex);

        changed_register(owner, number);
    }
    return error;
}


static error__t position_units_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct position_state *state = data;
    struct position_field *field = &state->values[number];

    LOCK(state->mutex);
    error__t error = format_string(result, length, "%s", field->units ?: "");
    UNLOCK(state->mutex);

    return error;
}

static error__t position_units_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct position_state *state = data;
    struct position_field *field = &state->values[number];

    LOCK(state->mutex);
    free(field->units);
    field->units = strdup(value);
    UNLOCK(state->mutex);

    return ERROR_OK;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Time. */

/* The semantics of this code is very similar to that of time_class, but here
 * we're working at the type level with 32-bit values. */

struct time_type_state {
    enum time_scale scale[0];
};


static error__t time_type_init(
    const char **string, unsigned int count, void **type_data)
{
    struct time_type_state *state = malloc(
        sizeof(struct time_type_state) + count * sizeof(enum time_scale));
    *state = (struct time_type_state) { };
    for (unsigned int i = 0; i < count; i ++)
        state->scale[i] = TIME_SECS;
    *type_data = state;
    return ERROR_OK;
}


static error__t time_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
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
    enum time_scale scale = 0;
    error__t error = shared_units_parse(string, &scale);
    if (!error)
    {
        struct time_type_state *state = data;
        state->scale[number] = scale;
        changed_register(owner, number);
    }
    return error;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Class and type definitions. */


const struct class_methods time_class_methods = {
    "time",
    .init = time_class_init,
    .parse_register = time_parse_register,
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
            .format = time_class_units_format,
            .put = time_class_units_put,
        },
        { "MIN",
            .format = time_min_format,
        },
    },
    .attr_count = 3,
};


const struct type_methods position_type_methods = {
    "position",
    .init = position_init, .destroy = position_destroy,
    .parse = position_parse, .format = position_format,
    .attrs = (struct attr_methods[]) {
        { "RAW",
            .format = raw_format_uint, .put = raw_put_uint, },
        { "SCALE", true,
            .format = position_scale_format, .put = position_scale_put, },
        { "OFFSET", true,
            .format = position_offset_format, .put = position_offset_put, },
        { "UNITS", true,
            .format = position_units_format, .put = position_units_put, },
    },
    .attr_count = 4,
};


const struct type_methods time_type_methods = {
    "time",
    .init = time_type_init,
    .parse = time_parse, .format = time_format,
    .attrs = (struct attr_methods[]) {
        { "RAW",
            .format = raw_format_uint,
            .put = raw_put_uint,
        },
        { "UNITS", true,
            .format = time_type_units_format,
            .put = time_type_units_put,
        },
    },
    .attr_count = 2,
};
