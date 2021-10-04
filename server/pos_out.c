/* Position output support. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "error.h"
#include "hardware.h"
#include "hashtable.h"
#include "parse.h"
#include "config_server.h"
#include "data_server.h"
#include "attributes.h"
#include "fields.h"
#include "types.h"
#include "enums.h"
#include "pos_mux.h"
#include "output.h"
#include "locking.h"

#include "pos_out.h"


enum pos_out_capture {
    POS_OUT_CAPTURE_NONE,
    POS_OUT_CAPTURE_VALUE,
    POS_OUT_CAPTURE_DIFF,
    POS_OUT_CAPTURE_SUM,
    POS_OUT_CAPTURE_MEAN,
    POS_OUT_CAPTURE_MIN,
    POS_OUT_CAPTURE_MAX,
    POS_OUT_CAPTURE_MIN_MAX,
    POS_OUT_CAPTURE_MIN_MAX_MEAN,
};

struct pos_out {
    pthread_mutex_t mutex;
    unsigned int count;                 // Number of values
    struct attr *capture_attr;          // CAPTURE attribute for change notify
    struct pos_out_field {
        /* Position scaling and units. */
        double scale;
        double offset;
        char *units;

        unsigned int capture_index;     // position bus capture index
        enum pos_out_capture capture;   // Capture state
    } values[];
};


/******************************************************************************/
/* Reading values. */

/* Interlock for position values and update indices. */
static pthread_mutex_t update_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Current values and update indices for each output field. */
static uint32_t pos_value[POS_BUS_COUNT];
static uint64_t pos_update_index[] = { [0 ... POS_BUS_COUNT-1] = 1 };



/* The refresh methods are called when we need a fresh value.  We retrieve
 * values and changed bits from the hardware and update settings accordingly. */

void do_pos_out_refresh(uint64_t change_index)
{
    WITH_MUTEX(update_mutex)
    {
        bool changes[POS_BUS_COUNT];
        hw_read_positions(pos_value, changes);
        for (unsigned int i = 0; i < POS_BUS_COUNT; i ++)
            if (changes[i]  &&  change_index > pos_update_index[i])
                pos_update_index[i] = change_index;
    }
}


/* Class method for value refresh. */
static void pos_out_refresh(void *class_data, unsigned int number)
{
    do_pos_out_refresh(get_change_index());
}


/* Single word read for type interface.  For pos_out values the first entry in
 * the index array is the position bus offset. */
static int read_pos_out_value(struct pos_out *pos_out, unsigned int number)
{
    uint32_t result;
    WITH_MUTEX(update_mutex)
    {
        unsigned int capture_index = pos_out->values[number].capture_index;
        result = pos_value[capture_index];
    }
    return (int) result;
}


/* When reading just return the current value from our static state. */
static error__t pos_out_get(
    void *class_data, unsigned int number, char result[], size_t length)
{
    struct pos_out *pos_out = class_data;
    return format_string(
        result, length, "%d", read_pos_out_value(pos_out, number));
}


/* Computation of change set. */
static void pos_out_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct pos_out *pos_out = class_data;
    WITH_MUTEX(update_mutex)
        for (unsigned int i = 0; i < pos_out->count; i ++)
        {
            unsigned int capture_index = pos_out->values[i].capture_index;
            changes[i] = pos_update_index[capture_index] > report_index;
        }
}


/******************************************************************************/
/* Attributes. */


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Scaling and units. */


static error__t pos_out_scaled_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct pos_out *pos_out = data;
    struct pos_out_field *field = &pos_out->values[number];

    double scale, offset;
    WITH_MUTEX(pos_out->mutex)
    {
        scale = field->scale;
        offset = field->offset;
    }

    int value = read_pos_out_value(pos_out, number);
    return format_double(result, length, value * scale + offset);
}


static error__t pos_out_scale_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct pos_out *pos_out = data;
    struct pos_out_field *field = &pos_out->values[number];

    double scale;
    WITH_MUTEX(pos_out->mutex)
        scale = field->scale;

    return format_double(result, length, scale);
}

static error__t pos_out_scale_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct pos_out *pos_out = data;
    struct pos_out_field *field = &pos_out->values[number];

    double scale;
    return
        parse_double(&value, &scale)  ?:
        parse_eos(&value)  ?:
        DO(WITH_MUTEX(pos_out->mutex) field->scale = scale);
}

static error__t pos_out_offset_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct pos_out *pos_out = data;
    struct pos_out_field *field = &pos_out->values[number];

    double offset;
    WITH_MUTEX(pos_out->mutex)
        offset = field->offset;

    return format_double(result, length, offset);
}

static error__t pos_out_offset_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct pos_out *pos_out = data;
    struct pos_out_field *field = &pos_out->values[number];
    double offset;
    return
        parse_double(&value, &offset)  ?:
        parse_eos(&value)  ?:
        DO(WITH_MUTEX(pos_out->mutex) field->offset = offset);
}


static error__t pos_out_units_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct pos_out *pos_out = data;
    struct pos_out_field *field = &pos_out->values[number];

    return ERROR_WITH_MUTEX(pos_out->mutex,
        format_string(result, length, "%s", field->units ?: ""));
}

static error__t pos_out_units_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct pos_out *pos_out = data;
    struct pos_out_field *field = &pos_out->values[number];

    const char *units;
    error__t error = parse_utf8_string(&value, &units);
    if (!error)
    {
        WITH_MUTEX(pos_out->mutex)
        {
            free(field->units);
            field->units = strdup(units);
        }
    }
    return error;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Capture control. */

static const struct enum_set pos_out_capture_enum_set = {
    .enums = (struct enum_entry[]) {
        { POS_OUT_CAPTURE_NONE,         "No", },
        { POS_OUT_CAPTURE_VALUE,        "Value", },
        { POS_OUT_CAPTURE_DIFF,         "Diff", },
        { POS_OUT_CAPTURE_SUM,          "Sum", },
        { POS_OUT_CAPTURE_MEAN,         "Mean", },
        { POS_OUT_CAPTURE_MIN,          "Min", },
        { POS_OUT_CAPTURE_MAX,          "Max", },
        { POS_OUT_CAPTURE_MIN_MAX,      "Min Max", },
        { POS_OUT_CAPTURE_MIN_MAX_MEAN, "Min Max Mean", },
    },
    .count = 9,
};

static const struct enumeration *pos_out_capture_enum;


static error__t pos_out_capture_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct pos_out *pos_out = data;
    struct pos_out_field *field = &pos_out->values[number];

    enum pos_out_capture capture;
    WITH_MUTEX(pos_out->mutex)
        capture = field->capture;

    return format_enumeration(pos_out_capture_enum, capture, result, length);
}


static error__t pos_out_capture_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct pos_out *pos_out = data;
    struct pos_out_field *field = &pos_out->values[number];
    unsigned int capture = 0;
    return
        TEST_OK_(enum_name_to_index(pos_out_capture_enum, value, &capture),
            "Invalid capture option")  ?:
        DO(WITH_MUTEX(pos_out->mutex) field->capture = capture);
}


static const struct enumeration *pos_out_capture_get_enumeration(void *data)
{
    return pos_out_capture_enum;
}


void reset_pos_out_capture(struct pos_out *pos_out, unsigned int number)
{
    WITH_MUTEX(pos_out->mutex)
    {
        struct pos_out_field *field = &pos_out->values[number];
        if (field->capture != POS_OUT_CAPTURE_NONE)
        {
            field->capture = POS_OUT_CAPTURE_NONE;
            attr_changed(pos_out->capture_attr, number);
        }
    }
}


bool get_pos_out_capture(
    struct pos_out *pos_out, unsigned int number, const char **string)
{
    struct pos_out_field *field = &pos_out->values[number];
    enum pos_out_capture capture = field->capture;
    *string = pos_out_capture_enum_set.enums[capture].name;
    return capture != POS_OUT_CAPTURE_NONE;
}


/* This attribute needs to be added separately so that we can hang onto the
 * attribute so that we can implement the reset_pos_out_capture method. */
static const struct attr_methods pos_out_capture_attr = {
    "CAPTURE", "Capture options",
    .in_change_set = true,
    .format = pos_out_capture_format, .put = pos_out_capture_put,
    .get_enumeration = pos_out_capture_get_enumeration,
};


/******************************************************************************/
/* Field info. */

static void get_capture_info(
    struct capture_info *capture_info, struct pos_out_field *field,
    struct capture_index capture_index, enum capture_mode capture_mode,
    const char *capture_string)
{
    *capture_info = (struct capture_info) {
        .capture_index = capture_index,
        .capture_mode = capture_mode,
        .capture_string = capture_string,
        .scale = field->scale,
        .offset = field->offset,
    };
    snprintf(
        capture_info->units, sizeof(capture_info->units), "%s", field->units);
}


unsigned int get_pos_out_capture_info(
    struct pos_out *pos_out, unsigned int number,
    struct capture_info capture_info[])
{
    struct pos_out_field *field = &pos_out->values[number];
    unsigned int capture_count = 0;

    /* This macro assembles the appropriate capture information into the given
     * capture_info[] array according to the field capture configuration. */
    #define POS_FIELD_UNUSED    0
    #define GET_CAPTURE_INFO(reg1, reg2, mode, name) \
        do { \
            struct capture_index capture_index = { \
                .index = { \
                    CAPTURE_POS_BUS(field->capture_index, POS_FIELD_##reg1), \
                    CAPTURE_POS_BUS(field->capture_index, POS_FIELD_##reg2), \
                }, \
            }; \
            get_capture_info( \
                &capture_info[capture_count], field, capture_index, \
                CAPTURE_MODE_##mode, name); \
            capture_count += 1; \
        } while (0)

    WITH_MUTEX(pos_out->mutex)
    {
        switch (field->capture)
        {
            case POS_OUT_CAPTURE_NONE:
                break;
            case POS_OUT_CAPTURE_VALUE:
                GET_CAPTURE_INFO(VALUE,   UNUSED,   SCALED32, "Value");
                break;
            case POS_OUT_CAPTURE_DIFF:
                GET_CAPTURE_INFO(DIFF,    UNUSED,   SCALED32, "Diff");
                break;
            case POS_OUT_CAPTURE_SUM:
                GET_CAPTURE_INFO(SUM_LOW, SUM_HIGH, SCALED64, "Sum");
                break;
            case POS_OUT_CAPTURE_MEAN:
                GET_CAPTURE_INFO(SUM_LOW, SUM_HIGH, AVERAGE,  "Mean");
                break;
            case POS_OUT_CAPTURE_MIN:
                GET_CAPTURE_INFO(MIN,     UNUSED,   SCALED32, "Min");
                break;
            case POS_OUT_CAPTURE_MAX:
                GET_CAPTURE_INFO(MAX,     UNUSED,   SCALED32, "Max");
                break;
            case POS_OUT_CAPTURE_MIN_MAX:
                GET_CAPTURE_INFO(MIN,     UNUSED,   SCALED32, "Min");
                GET_CAPTURE_INFO(MAX,     UNUSED,   SCALED32, "Max");
                break;
            case POS_OUT_CAPTURE_MIN_MAX_MEAN:
                GET_CAPTURE_INFO(MIN,     UNUSED,   SCALED32, "Min");
                GET_CAPTURE_INFO(MAX,     UNUSED,   SCALED32, "Max");
                GET_CAPTURE_INFO(SUM_LOW, SUM_HIGH, AVERAGE,  "Mean");
                break;
            default:
                ASSERT_FAIL();
        }
    }
    return capture_count;
}


/******************************************************************************/
/* Initialisation and shutdown. */

/* pos_out initialisation. */


/* This array of booleans is used to detect overlapping capture bus indexes. */
static bool pos_bus_index_used[POS_BUS_COUNT];


static struct pos_out *create_pos_out(
    unsigned int count, struct hash_table *attr_map,
    double scale, double offset, const char *units)
{
    struct pos_out *pos_out = malloc(
        sizeof(struct pos_out) + count * sizeof(struct pos_out_field));
    *pos_out = (struct pos_out) {
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .count = count,
        .capture_attr = add_one_attribute(
            &pos_out_capture_attr, NULL, pos_out, count, attr_map),
    };
    for (unsigned int i = 0; i < count; i ++)
        pos_out->values[i] = (struct pos_out_field) {
            .scale = scale,
            .offset = offset,
            .units = units ? strdup(units) : NULL,
        };
    return pos_out;
}


static error__t pos_out_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data,
    struct indent_parser *parser)
{
    double scale = 1.0;
    double offset = 0.0;
    const char *units = NULL;
    return
        /* The pos_out can optionally be followed by a scale, offset, and units.
         * If present these are used as defaults for the created pos_out. */
        IF(read_char(line, ' '),
            parse_double(line, &scale)  ?:
            IF(read_char(line, ' '),
                parse_double(line, &offset)  ?:
                IF(read_char(line, ' '),
                    parse_utf8_string(line, &units))))  ?:
        DO(*class_data = create_pos_out(count, attr_map, scale, offset, units));
}


static void pos_out_destroy(void *class_data)
{
    struct pos_out *pos_out = class_data;
    for (unsigned int i = 0; i < pos_out->count; i ++)
        free(pos_out->values[i].units);
    free(pos_out);
}


static error__t assign_capture_values(
    struct pos_out *pos_out, unsigned int values[])
{
    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < pos_out->count; i ++)
        error =
            TEST_OK_(values[i] < POS_BUS_COUNT,
                "Capture index out of range")  ?:
            TEST_OK_(!pos_bus_index_used[values[i]],
                "Capture index %u already used", values[i])  ?:
            DO( pos_out->values[i].capture_index = values[i];
                pos_bus_index_used[values[i]] = true);
    return error;
}


static error__t pos_out_parse_register(
    void *class_data, struct field *field, unsigned int block_base,
    const char **line)
{
    struct pos_out *pos_out = class_data;
    unsigned int registers[pos_out->count];
    return
        /* Parse and record position bus assignments */
        parse_uint_array(line, registers, pos_out->count)  ?:
        assign_capture_values(pos_out, registers)  ?:
        /* Add all positions to the list of pos_mux options. */
        add_pos_mux_index(field, registers, pos_out->count)  ?:
        /* Register this as an output source. */
        register_pos_out(pos_out, field, pos_out->count);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* File initialisation */

error__t initialise_pos_out(void)
{
    pos_out_capture_enum = create_static_enumeration(&pos_out_capture_enum_set);
    return ERROR_OK;
}


void terminate_pos_out(void)
{
    if (pos_out_capture_enum)
        destroy_enumeration(pos_out_capture_enum);
}


/******************************************************************************/

const struct class_methods pos_out_class_methods = {
    "pos_out",
    .init = pos_out_init,
    .parse_register = pos_out_parse_register,
    .destroy = pos_out_destroy,
    .get = pos_out_get, .refresh = pos_out_refresh,
    .change_set = pos_out_change_set,
    .change_set_index = CHANGE_IX_POSITION,
    .attrs = DEFINE_ATTRIBUTES(
        { "SCALED", "Value with scaling applied",
            .format = pos_out_scaled_format, },
        { "SCALE", "Scale factor",
            .in_change_set = true,
            .format = pos_out_scale_format, .put = pos_out_scale_put, },
        { "OFFSET", "Offset",
            .in_change_set = true,
            .format = pos_out_offset_format, .put = pos_out_offset_put, },
        { "UNITS", "Units string",
            .in_change_set = true,
            .format = pos_out_units_format,
            .put = pos_out_units_put,
        },
        // "CAPTURE" added in constructor
    ),
};
