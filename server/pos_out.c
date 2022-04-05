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


/******************************************************************************/
/* Common definitions and structures. */

/* Bit offset definitions into capture mask.  The ordering matters slightly,
 * it affects the ordering of the nominal enums that are generated, which must
 * be preserved for backwards compatibility. */
enum {
    POS_OUT_CAPTURE_VALUE,
    POS_OUT_CAPTURE_DIFF,
    POS_OUT_CAPTURE_SUM,
    POS_OUT_CAPTURE_MIN,
    POS_OUT_CAPTURE_MAX,
    POS_OUT_CAPTURE_MEAN,
    POS_OUT_CAPTURE_STDDEV,
};

/* Corresponding capture mask bits. */
#define CAPTURE_VALUE_BIT       (1 << POS_OUT_CAPTURE_VALUE)
#define CAPTURE_DIFF_BIT        (1 << POS_OUT_CAPTURE_DIFF)
#define CAPTURE_SUM_BIT         (1 << POS_OUT_CAPTURE_SUM)
#define CAPTURE_MIN_BIT         (1 << POS_OUT_CAPTURE_MIN)
#define CAPTURE_MAX_BIT         (1 << POS_OUT_CAPTURE_MAX)
#define CAPTURE_MEAN_BIT        (1 << POS_OUT_CAPTURE_MEAN)
#define CAPTURE_STDDEV_BIT      (1 << POS_OUT_CAPTURE_STDDEV)


/* Used to define behaviour of each capture option above. */
struct capture_option_info {
    const char *option_name;
    enum capture_mode capture_mode;
    struct capture_index capture_index;
};

#define CAPTURE_INFO(name, mode, index...) \
    { \
        .option_name = name, \
        .capture_mode = mode, \
        .capture_index = { { index } }, \
    }

static const struct capture_option_info capture_option_info[] = {
    [POS_OUT_CAPTURE_VALUE] =
        CAPTURE_INFO("Value", CAPTURE_MODE_SCALED32,
            POS_FIELD_VALUE),
    [POS_OUT_CAPTURE_DIFF] =
        CAPTURE_INFO("Diff", CAPTURE_MODE_SCALED32,
            POS_FIELD_DIFF),
    [POS_OUT_CAPTURE_SUM] =
        CAPTURE_INFO("Sum", CAPTURE_MODE_SCALED64,
            POS_FIELD_SUM_LOW, POS_FIELD_SUM_HIGH),
    [POS_OUT_CAPTURE_MEAN] =
        CAPTURE_INFO("Mean", CAPTURE_MODE_AVERAGE,
            POS_FIELD_SUM_LOW, POS_FIELD_SUM_HIGH),
    [POS_OUT_CAPTURE_MIN] =
        CAPTURE_INFO("Min", CAPTURE_MODE_SCALED32,
            POS_FIELD_MIN),
    [POS_OUT_CAPTURE_MAX] =
        CAPTURE_INFO("Max", CAPTURE_MODE_SCALED32,
            POS_FIELD_MAX),
    [POS_OUT_CAPTURE_STDDEV] =
        CAPTURE_INFO("StdDev", CAPTURE_MODE_STDDEV,
            POS_FIELD_SUM_LOW, POS_FIELD_SUM_HIGH,
            POS_FIELD_SUM2_LOW, POS_FIELD_SUM2_MID, POS_FIELD_SUM2_HIGH),
};

/* Ensure the number of capture options matches the table above. */
STATIC_COMPILE_ASSERT(MAX_POS_OUT_CAPTURE == ARRAY_SIZE(capture_option_info));


/* This array of capture masks is used to initialise the default nominal names
 * available through the *ENUMS? request. */
static const unsigned int nominal_capture_masks[] = {
    0,
    CAPTURE_VALUE_BIT,
    CAPTURE_DIFF_BIT,
    CAPTURE_SUM_BIT,
    CAPTURE_MEAN_BIT,
    CAPTURE_MIN_BIT,
    CAPTURE_MAX_BIT,
    CAPTURE_MIN_BIT | CAPTURE_MAX_BIT,
    CAPTURE_MIN_BIT | CAPTURE_MAX_BIT | CAPTURE_MEAN_BIT,
    CAPTURE_STDDEV_BIT,
    CAPTURE_MEAN_BIT | CAPTURE_STDDEV_BIT,
};


/* Definition of pos_out field. */
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
        unsigned int capture_mask;      // Mask of required captures
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


/* Format current capture mask into given buffer. */
static error__t format_capture_string(
    unsigned int capture_mask, char buffer[], size_t length)
{
    size_t formatted = 0;
    for (size_t i = 0; i < ARRAY_SIZE(capture_option_info); i ++)
        if (capture_mask & (1U << i))
        {
            const char *option = capture_option_info[i].option_name;
            size_t option_length = strlen(option);

            error__t error = TEST_OK_(
                formatted + (formatted > 0) + option_length + 1 < length,
                "Unexpected buffer overflow in pos_out format");
            if (error)
            {
                /* This really should not happen, looks like we're overflowing a
                 * buffer somewhere.  Convert the result into an error, log
                 * an error message, and bail out. */
                strncpy(buffer, "(overflow)", length);
                return error;
            }

            /* Add separator when required. */
            if (formatted > 0)
            {
                buffer[formatted] = ' ';
                formatted += 1;
            }

            /* This is safe, we've already checked. */
            strcpy(&buffer[formatted], option);
            formatted += option_length;
        }

    /* Finally if nothing was formatted (empty capture mask), use "No". */
    if (formatted == 0)
        strncpy(buffer, "No", 3);

    return ERROR_OK;
}


static error__t pos_out_capture_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct pos_out *pos_out = data;
    struct pos_out_field *field = &pos_out->values[number];

    unsigned int capture_mask;
    WITH_MUTEX(pos_out->mutex)
        capture_mask = field->capture_mask;
    return format_capture_string(capture_mask, result, length);
}


static const struct enumeration *lookup_capture_option;

static error__t pos_out_capture_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct pos_out *pos_out = data;
    struct pos_out_field *field = &pos_out->values[number];

    unsigned int capture_mask = 0;
    error__t error = ERROR_OK;
    /* The value "No" is treated the same as an empty option string: disable
     * capture.  Otherwise we parse the available options. */
    if (strcmp(value, "No") != 0)
    {
        while (!error  &&  *value != '\0')
        {
            char option[MAX_NAME_LENGTH];
            unsigned int capture_index = 0;
            error =
                parse_name(&value, option, sizeof(option))  ?:
                DO(value = skip_whitespace(value))  ?:
                TEST_OK_(enum_name_to_index(
                    lookup_capture_option, option, &capture_index),
                    "Unknown capture option \"%s\"", option)  ?:
                DO(capture_mask |= 1U << capture_index);
        }
    }

    if (!error)
        WITH_MUTEX(pos_out->mutex)
            field->capture_mask = capture_mask;
    return error;
}


/* This enumeration only exists in order to implement the *ENUMS? option for
 * this attribute for backwards compatibility, and is initialised with the
 * nominal standard list of capture options. */
static const struct enumeration *pos_out_capture_enum;

static const struct enumeration *pos_out_capture_get_enumeration(void *data)
{
    return pos_out_capture_enum;
}


error__t get_capture_options(struct connection_result *result)
{
    bool enable_std_dev = hw_read_fpga_capabilities() & FPGA_CAPABILITY_STDDEV;
    result->response = RESPONSE_MANY;
    for (unsigned int i = 0; i < ARRAY_SIZE(capture_option_info); i ++)
        /* Only report StdDev available if enabled. */
        if (enable_std_dev  ||  i != POS_OUT_CAPTURE_STDDEV)
            result->write_many(result->write_context,
                capture_option_info[i].option_name);
    return ERROR_OK;
}


void reset_pos_out_capture(struct pos_out *pos_out, unsigned int number)
{
    WITH_MUTEX(pos_out->mutex)
    {
        struct pos_out_field *field = &pos_out->values[number];
        if (field->capture_mask != 0)
        {
            field->capture_mask = 0;
            attr_changed(pos_out->capture_attr, number);
        }
    }
}


void report_pos_out_capture(
    struct pos_out *pos_out, unsigned int number,
    const char *field_name, struct connection_result *result)
{
    struct pos_out_field *field = &pos_out->values[number];
    unsigned int capture_mask;
    WITH_MUTEX(pos_out->mutex)
        capture_mask = field->capture_mask;
    if (field->capture_mask)
    {
        size_t written = (size_t) snprintf(
            result->string, result->length, "%s ", field_name);
        ERROR_REPORT(format_capture_string(capture_mask,
            result->string + written, result->length - written),
            "Unexpected formatting error in pos_out");
        result->write_many(result->write_context, result->string);
    }
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


unsigned int get_pos_out_capture_info(
    struct pos_out *pos_out, unsigned int number,
    struct capture_info capture_info[])
{
    struct pos_out_field *field = &pos_out->values[number];
    unsigned int capture_count = 0;

    /* Generate a capture info entry for each enabled capture option. */
    for (unsigned int i = 0; i < ARRAY_SIZE(capture_option_info); i ++)
    {
        if (field->capture_mask & (1U << i))
        {
            const struct capture_option_info *info = &capture_option_info[i];
            *capture_info = (struct capture_info) {
                .capture_mode = info->capture_mode,
                .capture_string = info->option_name,
                .scale = field->scale,
                .offset = field->offset,
            };
            for (unsigned int k = 0; k < CAPTURE_INDEX_SIZE; k ++)
                /* Tie each index to the appropriate field. */
                capture_info->capture_index.index[k] = CAPTURE_POS_BUS(
                    field->capture_index, info->capture_index.index[k]);
            snprintf(
                capture_info->units, sizeof(capture_info->units),
                "%s", field->units);

            capture_info += 1;
            capture_count += 1;
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

/* Populate the keywork option lookup table. */
static error__t initialise_keyword_lookup(
    const struct enumeration **result, bool enable_std_dev)
{
    error__t error = ERROR_OK;
    struct enumeration *enumeration = create_dynamic_enumeration();
    for (unsigned int i = 0;
         !error  &&  i < ARRAY_SIZE(capture_option_info); i ++)
    {
        /* Special guard: don't add std dev option if not enabled. */
        if (enable_std_dev  ||  i != POS_OUT_CAPTURE_STDDEV)
            error = add_enumeration(
                enumeration, capture_option_info[i].option_name, i);
    }
    if (error)
        destroy_enumeration(enumeration);
    else
        *result = enumeration;
    return error;
}


/* Now populate the list of available capture strings; this has to be an enum
 * because of the inquiry interface. */
static error__t initialise_available_enums(
    const struct enumeration **result, bool enable_std_dev)
{
    error__t error = ERROR_OK;
    struct enumeration *enumeration = create_dynamic_enumeration();
    for (unsigned int i = 0;
         !error  &&  i < ARRAY_SIZE(nominal_capture_masks); i ++)
    {
        /* Special guard: if std dev in mask and not enabled then ignore this
         * option. */
        unsigned int capture_mask = nominal_capture_masks[i];
        if (enable_std_dev  ||  !(capture_mask & CAPTURE_STDDEV_BIT))
        {
            char capture_string[64];
            error =
                format_capture_string(
                    capture_mask, capture_string, sizeof(capture_string))  ?:
                add_enumeration(enumeration, capture_string, i);
        }
    }
    if (error)
        destroy_enumeration(enumeration);
    else
        *result = enumeration;
    return error;
}


error__t initialise_pos_out(void)
{
    /* Ask FPGA whether it supports standard deviation capture.  It's enough to
     * exclude these strings from the lookup and enum lists. */
    bool enable_std_dev = hw_read_fpga_capabilities() & FPGA_CAPABILITY_STDDEV;

    return
        initialise_keyword_lookup(&lookup_capture_option, enable_std_dev)  ?:
        initialise_available_enums(&pos_out_capture_enum, enable_std_dev);
}


void terminate_pos_out(void)
{
    if (pos_out_capture_enum)
        destroy_enumeration(pos_out_capture_enum);
    if (lookup_capture_option)
        destroy_enumeration(lookup_capture_option);
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
