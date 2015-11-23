#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <math.h>

#include "error.h"
#include "hashtable.h"
#include "parse.h"
#include "config_server.h"
#include "fields.h"
#include "types.h"
#include "attributes.h"
#include "hardware.h"
#include "capture.h"

#include "classes.h"


#define UNASSIGNED_REGISTER ((unsigned int) -1)


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Abstract interface to class. */
struct class_methods {
    const char *name;

    /* Type information. */
    const char *default_type;   // Default type.  If NULL no type is created
    bool force_type;            // If set default_type cannot be modified

    /* Called to parse the class definition line for a field.  The corresponding
     * class has already been identified. */
    void (*init)(unsigned int count, void **class_data);

    /* Parses the register definition line for this field. */
    error__t (*parse_register)(
        struct class *class, const char *block_name, const char *field_name,
        const char **line);
    /* Called after startup to validate setup. */
    error__t (*validate)(struct class *class);
    /* Called during shutdown to release all class resources. */
    void (*destroy)(struct class *class);

    /* Register read/write methods. */
    uint32_t (*read)(struct class *class, unsigned int number);
    void (*write)(struct class *class, unsigned int number, uint32_t value);
    /* For the _out classes the data provided by .read() needs to be loaded as a
     * separate action, this optional method does this. */
    void (*refresh)(struct class *class, unsigned int number);
    /* Computes change set for this class.  The class looks up its own change
     * index in report_index[] and updates changes[] accordingly. */
    void (*change_set)(
        struct class *class, const uint64_t report_index[], bool changes[]);

    /* Direct access to fields bypassing read/write/type handling. */
    error__t (*get)(
        struct class *class, unsigned int ix,
        struct connection_result *result);
    error__t (*put)(
        struct class *class, unsigned int ix, const char *value);
    error__t (*put_table)(
        struct class *class, unsigned int ix,
        bool append, struct put_table_writer *writer);

    /* Class specific attributes. */
    const struct attr_methods *attrs;
    unsigned int attr_count;
};



/*****************************************************************************/
/* Individual class implementations. */



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Parameters. */

/* All of bit_in, pos_in and param have very similar behaviour: values are
 * written to a register, the written value is cached for readback, and we keep
 * track of the report index. */

struct param_state {
    uint32_t value;
    uint64_t update_index;
};

static void param_init(unsigned int count, void **class_data)
{
    *class_data = calloc(count, sizeof(struct param_state));
}


static uint32_t param_read(struct class *class, unsigned int number)
{
    struct param_state *state = class->class_data;
    return state[number].value;
}


static void param_write(
    struct class *class, unsigned int number, uint32_t value)
{
    struct param_state *state = class->class_data;
    state[number].value = value;
    state[number].update_index = get_change_index();
    hw_write_register(class->block_base, number, class->field_register, value);
}


static void param_change_set(
    struct class *class, const uint64_t report_index[], bool changes[])
{
    struct param_state *state = class->class_data;
    uint64_t report = report_index[CHANGE_IX_CONFIG];
    for (unsigned int i = 0; i < class->count; i ++)
        changes[i] = state[i].update_index >= report;
}

#define PARAM_CLASS \
    .init = param_init, \
    .parse_register = default_parse_register, \
    .validate = default_validate, \
    .read = param_read, \
    .write = param_write, \
    .change_set = param_change_set


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Read class. */

/* We track each read and use changes in the read value to update the change
 * index. */

struct read_state {
    uint32_t value;
    uint64_t update_index;
};


static void read_init(unsigned int count, void **class_data)
{
    *class_data = calloc(count, sizeof(struct read_state));
}


/* Reading is a two stage process: each time we do a read we check the value and
 * update the update_index accordingly. */
static void read_refresh(struct class *class, unsigned int number)
{
    struct read_state *state = class->class_data;
    uint32_t result =
        hw_read_register(class->block_base, number, class->field_register);
    if (result != state[number].value)
    {
        state[number].value = result;
        state[number].update_index = get_change_index();
    }
}

static uint32_t read_read(struct class *class, unsigned int number)
{
    struct read_state *state = class->class_data;
    return state[number].value;
}

static void read_change_set(
    struct class *class, const uint64_t report_index[], bool changes[])
{
    struct param_state *state = class->class_data;
    uint64_t report = report_index[CHANGE_IX_READ];
    for (unsigned int i = 0; i < class->count; i ++)
        changes[i] = state[i].update_index >= report;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Write class. */

/* This one is simple.  We write to the register.  That's all. */

static void write_write(
    struct class *class, unsigned int number, uint32_t value)
{
    hw_write_register(class->block_base, number, class->field_register, value);
}


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


static void time_init(unsigned int count, void **class_data)
{
    struct time_state *state = calloc(count, sizeof(struct time_state));
    for (unsigned int i = 0; i < count; i ++)
        state[i].time_scale = TIME_SECS;
    *class_data = state;
}


static void time_change_set(
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


static error__t time_get(
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


static error__t time_put(
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


static error__t time_raw_format(
    struct class *class, void *data, unsigned int number,
    char result[], size_t length)
{
    struct time_state *state = class->class_data;
    state = &state[number];
    return format_string(result, length, "%"PRIu64, state->value);
}


static error__t time_raw_put(
    struct class *class, void *data, unsigned int number, const char *string)
{
    uint64_t value;
    return
        parse_uint64(&string, &value)  ?:
        parse_eos(&string)  ?:
        DO(write_time_value(class, number, value));
}


static error__t time_scale_format(
    struct class *class, void *data, unsigned int number,
    char result[], size_t length)
{
    struct time_state *state = class->class_data;
    state = &state[number];
    return format_string(result, length, "%s", time_units[state->time_scale]);
}


static error__t time_scale_put(
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


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Table definitions. */

static error__t table_get(
    struct class *class, unsigned int ix,
    struct connection_result *result)
{
    return FAIL_("Not implemented");
}

static error__t table_put_table(
    struct class *class, unsigned int ix,
    bool append, struct put_table_writer *writer)
{
    return FAIL_("block.field< not implemented yet");
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Common defaults for simple register assignment. */

static error__t default_validate(struct class *class)
{
    return
        TEST_OK_(class->field_register != UNASSIGNED_REGISTER,
            "No register assigned to field");
}

static error__t default_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    return
        TEST_OK_(class->field_register == UNASSIGNED_REGISTER,
            "Register already assigned")  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &class->field_register);
}


/*****************************************************************************/
/* Top level list of classes. */

static const struct class_methods classes_table[] = {
    { "bit_in", "bit_mux", true, PARAM_CLASS, },
    { "pos_in", "pos_mux", true, PARAM_CLASS, },
    { "param", "uint", PARAM_CLASS, },
    { "time",
        .init = time_init,
        .parse_register = default_parse_register,
        .validate = default_validate,
        .get = time_get,
        .put = time_put,
        .change_set = time_change_set,
        .attrs = (struct attr_methods[]) {
            { "RAW",
                .format = time_raw_format,
                .put = time_raw_put,
            },
            { "UNITS", true,
                .format = time_scale_format,
                .put = time_scale_put,
            },
        },
        .attr_count = 2,
    },
    { "bit_out", "bit",
        .init = bit_pos_out_init,
        .parse_register = bit_out_parse_register,
        .validate = bit_pos_out_validate,
        .read = bit_out_read, .refresh = bit_out_refresh,
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
    },
    { "pos_out", "position",
        .init = bit_pos_out_init,
        .parse_register = pos_out_parse_register,
        .validate = bit_pos_out_validate,
        .read = pos_out_read, .refresh = pos_out_refresh,
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
    },
    { "read", "uint",
        .init = read_init,
        .validate = default_validate,
        .parse_register = default_parse_register,
        .read = read_read, .refresh = read_refresh,
        .change_set = read_change_set,
    },
    { "write", "uint",
        .validate = default_validate,
        .parse_register = default_parse_register,
        .write = write_write,
    },
    { "table",
        .get = table_get,
        .put_table = table_put_table,
        .attrs = (struct attr_methods[]) {
            { "LENGTH", },
            { "B", },
            { "FIELDS", },
        },
        .attr_count = 2,
    },
};


/*****************************************************************************/
/* External API. */

/* Class field access. */

error__t class_read(
    struct class *class, unsigned int number, uint32_t *value, bool refresh)
{
    return
        TEST_OK_(class->methods->read, "Field not readable")  ?:
        IF(refresh  &&  class->methods->refresh,
            DO(class->methods->refresh(class, number)))  ?:
        DO(*value = class->methods->read(class, number));
}


error__t class_write(struct class *class, unsigned int number, uint32_t value)
{
    return
        TEST_OK_(class->methods->write, "Field not writeable")  ?:
        DO(class->methods->write(class, number, value));
}


error__t class_get(
    struct class *class, unsigned int number,
    struct connection_result *result)
{
    /* For the moment we delegate this method to class_read if there is a type.
     * This is going to be rewritten shortly. */
    if (class->type)
    {
        uint32_t value;
        return
            class_read(class, number, &value, true)  ?:
            type_format(
                class->type, number, value, result->string, result->length)  ?:
            DO(result->response = RESPONSE_ONE);
    }
    else
        return
            TEST_OK_(class->methods->get, "Field not readable")  ?:
            class->methods->get(class, number, result);
}


error__t class_put(
    struct class *class, unsigned int number, const char *string)
{
    /* Same story as for class_get */
    if (class->type)
    {
        uint32_t value;
        return
            type_parse(class->type, number, string, &value)  ?:
            class_write(class, number, value);
    }
    else
        return
            TEST_OK_(class->methods->put, "Field not writeable")  ?:
            class->methods->put(class, number, string);
}


error__t class_put_table(
    struct class *class, unsigned int number,
    bool append, struct put_table_writer *writer)
{
    return
        TEST_OK_(class->methods->put_table, "Field is not a table")  ?:
        class->methods->put_table(class, number, append, writer);
}


/* Change support. */

void refresh_class_changes(enum change_set change_set)
{
    if (change_set & CHANGES_BITS)
        bit_out_refresh(NULL, 0);
    if (change_set & CHANGES_POSITION)
        pos_out_refresh(NULL, 0);
}


void get_class_change_set(
    struct class *class, const uint64_t report_index[], bool changes[])
{
    if (class->methods->change_set)
        class->methods->change_set(class, report_index, changes);
    else
        memset(changes, 0, sizeof(bool) * class->count);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Class inititialisation. */


static error__t lookup_class(
    const char *name, const struct class_methods **result)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(classes_table); i ++)
    {
        const struct class_methods *methods = &classes_table[i];
        if (strcmp(name, methods->name) == 0)
        {
            *result = methods;
            return ERROR_OK;
        }
    }
    return FAIL_("Class %s not found", name);
}

static struct class *create_class_block(
    const struct class_methods *methods, unsigned int count, void *class_data)
{
    struct class *class = malloc(sizeof(struct class));
    *class = (struct class) {
        .methods = methods,
        .count = count,
        .block_base = UNASSIGNED_REGISTER,
        .field_register = UNASSIGNED_REGISTER,
        .class_data = class_data,
    };
    return class;
}

error__t create_class(
    const char *class_name, const char **line, unsigned int count,
    struct class **class)
{
    const struct class_methods *methods = NULL;
    void *class_data = NULL;
    const char *default_type;
    return
        lookup_class(class_name, &methods)  ?:
        DO(default_type = methods->default_type)  ?:
        IF(methods->init,
            DO(methods->init(count, &class_data)))  ?:
        DO(*class = create_class_block(methods, count, class_data))  ?:

        /* Figure out which type to generate.  If a type is specified and we
         * don't consume it then an error will be reported. */
        IF(default_type,
            /* If no type specified use the default. */
            IF(**line == '\0', DO(line = &default_type))  ?:
            create_type(line, methods->force_type, count, &(*class)->type));
}


void create_class_attributes(
    struct class *class, struct hash_table *attr_map)
{
    for (unsigned int i = 0; i < class->methods->attr_count; i ++)
        create_attribute(
            &class->methods->attrs[i], class, class->class_data,
            class->count, attr_map);
    if (class->type)
        create_type_attributes(class, class->type, attr_map);
}


error__t class_parse_attribute(struct class *class, const char **line)
{
    return
        TEST_OK_(class->type, "Cannot add attribute to this field")  ?:
        type_parse_attribute(class->type, line);
}


error__t class_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    return
        IF(class->methods->parse_register,
            class->methods->parse_register(
                class, block_name, field_name, line));
}


error__t validate_class(struct class *class, unsigned int block_base)
{
    class->block_base = block_base;
    return
        IF(class->methods->validate,
            class->methods->validate(class));
}

void describe_class(struct class *class, char *string, size_t length)
{
    size_t written =
        (size_t) snprintf(string, length, "%s", class->methods->name);
    if (class->type)
        snprintf(string + written, length - written, " %s",
            get_type_name(class->type));
}

void destroy_class(struct class *class)
{
    if (class->methods->destroy)
        class->methods->destroy(class);
    free(class->class_data);
    if (class->type)
        destroy_type(class->type);
    free(class);
}
