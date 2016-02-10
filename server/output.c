#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "error.h"
#include "hardware.h"
#include "parse.h"
#include "config_server.h"
#include "data_server.h"
#include "fields.h"
#include "classes.h"
#include "attributes.h"
#include "types.h"
#include "enums.h"
#include "time_position.h"
#include "bit_out.h"
#include "prepare.h"
#include "locking.h"

#include "output.h"


/* Interlock for position values and update indices. */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/* Current values and update indices for each output field. */
static uint32_t pos_value[POS_BUS_COUNT];
static uint64_t pos_update_index[POS_BUS_COUNT];

/* Map between field names and bit bus indexes. */
static struct enumeration *pos_mux_lookup;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Defining structures. */

/* This enumeration is used to manage the list of possible output types. */
enum output_type {
    OUTPUT_POSN,            // pos_out
    OUTPUT_CONST,           // pos_out const
    OUTPUT_POSN_ENCODER,    // pos_out encoder
    OUTPUT_ADC,             // pos_out adc
    OUTPUT_EXT,             // ext_out
    OUTPUT_TIMESTAMP,       // ext_out timestamp
    OUTPUT_OFFSET,          // ext_out offset
    OUTPUT_ADC_COUNT,       // ext_out adc_count
    OUTPUT_BITS,            // ext_out bits <n>
};


/* This structure is shared between pos_out and ext_out implementations and
 * instances are registered with data capture. */
struct output {
    const struct output_class *output_class;    // Controlling structure
    unsigned int count;                 // Number of instances
    enum output_type output_type;
    struct field *field;                // Needed for name formatting!

    struct type *type;                  // Only for pos_out
    struct position_state *position;    // Also only for pos_out
    unsigned int bit_group;             // Only for ext_out bit <group>

    struct output_value {
        unsigned int capture_index[2];  // Associated capture registers
        unsigned int capture_state;     // Current capture selection
    } values[];
};


/* This structure is used to determine the core behaviour of the output
 * instance. */
struct output_class {
    /* These fields determine the behaviour of the output field. */
    const struct enum_set enum_set;
    const struct output_options {
        enum capture_mode capture_mode;
        enum framing_mode framing_mode;
        bool zero_offset;
    } *output_options;
    bool scaling;

    /* This enumeration is filled in during initialisation. */
    const struct enumeration *enumeration;
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Reading values. */


/* The refresh methods are called when we need a fresh value.  We retrieve
 * values and changed bits from the hardware and update settings accordingly. */

void do_pos_out_refresh(uint64_t change_index)
{
    LOCK(mutex);
    bool changes[POS_BUS_COUNT];
    hw_read_positions(pos_value, changes);
    for (unsigned int i = 0; i < POS_BUS_COUNT; i ++)
        if (changes[i]  &&  change_index > pos_update_index[i])
            pos_update_index[i] = change_index;
    UNLOCK(mutex);
}


/* Class method for value refresh. */
static void pos_out_refresh(void *class_data, unsigned int number)
{
    do_pos_out_refresh(get_change_index());
}


/* Single word read for type interface.  For pos_out values the first entry in
 * the index array is the position bus offset. */
static error__t register_read(
    void *reg_data, unsigned int number, uint32_t *result)
{
    struct output *output = reg_data;
    LOCK(mutex);
    unsigned int capture_index = output->values[number].capture_index[0];
    *result = pos_value[capture_index];
    UNLOCK(mutex);
    return ERROR_OK;
}

static const struct register_methods register_methods = {
    .read = register_read,
};


/* When reading just return the current value from our static state. */
static error__t output_get(
    void *class_data, unsigned int number, struct connection_result *result)
{
    struct output *output = class_data;
    return type_get(output->type, number, result);
}


/* Computation of change set. */
static void pos_out_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct output *output = class_data;
    LOCK(mutex);
    for (unsigned int i = 0; i < output->count; i ++)
    {
        unsigned int capture_index = output->values[i].capture_index[0];
        changes[i] = pos_update_index[capture_index] > report_index;
    }
    UNLOCK(mutex);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


/* Capture enumeration. */
void report_capture_positions(struct connection_result *result)
{
    write_enum_labels(pos_mux_lookup, result);
}


void reset_capture_list(void)
{
    ASSERT_FAIL();
//     pos_capture_mask = 0;
}


static void get_output_scaling(
    const struct output *output, unsigned int number,
    struct scaling *scaling)
{
    const char *units;
    get_position_info(
        output->position, number, &scaling->scale, &scaling->offset, &units);
}


void format_output_name(
    const struct output *output, unsigned int number,
    char string[], size_t length)
{
    format_field_name(string, length, output->field, NULL, number, '\0');
}


enum capture_mode get_capture_mode(
    const struct output *output, unsigned int number)
{
    unsigned int capture_state = output->values[number].capture_state;
    if (capture_state == 0)
        return CAPTURE_OFF;
    else
    {
        const struct output_options *options =
            &output->output_class->output_options[capture_state - 1];
        return options->capture_mode;
    }
}


enum framing_mode get_capture_info(
    const struct output *output, unsigned int number, struct scaling *scaling)
{
    unsigned int capture_state = output->values[number].capture_state;
    /* !!!!!!!!!!!!!!!!!!!!!!!!!
     * Need a lock to guard against this! */
    ASSERT_OK(capture_state > 0);

    const struct output_class *output_class = output->output_class;
    const struct output_options *options =
        &output_class->output_options[capture_state - 1];
    if (output_class->scaling)
    {
        get_output_scaling(output, number, scaling);
        if (options->zero_offset)
            scaling->offset = 0.0;
    }
    return options->framing_mode;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* CAPTURE attribute. */


static error__t capture_format(
    void *owner, void *data, unsigned int number, char result[], size_t length)
{
    struct output *output = data;
    const char *string;
    return
        TEST_OK(string = enum_index_to_name(
            output->output_class->enumeration,
            output->values[number].capture_state))  ?:
        format_string(result, length, "%s", string);
}


static error__t capture_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct output *output = data;
    return TEST_OK_(
        enum_name_to_index(
            output->output_class->enumeration, value,
            &output->values[number].capture_state),
        "Not a valid capture option");
}


static const struct enumeration *capture_get_enumeration(void *data)
{
    struct output *output = data;
    return output->output_class->enumeration;
}


static const struct attr_methods capture_attr_methods = {
    "CAPTURE", "Configure capture for this field",
    .in_change_set = true,
    .format = capture_format,
    .put = capture_put,
    .get_enumeration = capture_get_enumeration,
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* BITS attribute, only meaningful for ext_out bits. */


static error__t bits_get_many(
    void *owner, void *data, unsigned int number,
    struct connection_result *result)
{
    struct output *output = data;
    report_capture_bits(result, output->bit_group);
    return ERROR_OK;
}


static const struct attr_methods bits_attr_methods = {
    "BITS", "Enumerate bits captured in this word",
    .get_many = bits_get_many,
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* CAPTURE options. */

/* The field class and sub-class determine the possible capture enumerations and
 * their possible meanings as returned by get_capture_mode.
 *
 *                      CAPTURE_                FRAMING_
 *  pos_out
 *      Triggered       SCALED32
 *      Difference      SCALED32 (offset=0)     FRAME
 *
 *  pos_out encoder
 *      Triggered       SCALED32
 *      Difference      SCALED32 (offset=0)     FRAME
 *      Average         SCALED32                SPECIAL
 *      Extended        SCALED64
 *
 *  pos_out adc
 *      Triggered       SCALED32
 *      Average         ADC_MEAN                FRAME
 *
 *  pos_out const
 *      (not capturable)
 *
 *  ext_out timestamp
 *      Capture         TS_NORMAL
 *      Frame           TS_OFFSET
 *
 *  ext_out
 *  ext_out offset
 *  ext_out adc_count
 *  ext_out bits <group>
 *      Capture         UNSCALED
 *
 * The following structures define output behaviour according to this table. */

/*  pos_out
 *      Triggered       SCALED32
 *      Difference      SCALED32 (offset=0)     FRAME   */
static struct output_class pos_out_output_class = {
    .enum_set = {
        .enums = (struct enum_entry[]) {
            { 0, "No", },
            { 1, "Triggered", },
            { 2, "Difference", },
        },
        .count = 3,
    },
    .output_options = (struct output_options[]) {
        { CAPTURE_SCALED32, },                      // Triggered
        { CAPTURE_SCALED32, FRAMING_FRAME, true, }, // Average
    },
    .scaling = true,
};

/*  pos_out encoder
 *      Triggered       SCALED32
 *      Difference      SCALED32 (offset=0)     FRAME
 *      Average         SCALED32                SPECIAL
 *      Extended        SCALED64                        */
static struct output_class pos_out_encoder_output_class = {
    .enum_set = {
        .enums = (struct enum_entry[]) {
            { 0, "No", },
            { 1, "Triggered", },
            { 2, "Difference", },
            { 3, "Average", },
            { 4, "Extended", },
        },
        .count = 5,
    },
    .output_options = (struct output_options[]) {
        { CAPTURE_SCALED32, },                      // Triggered
        { CAPTURE_SCALED32, FRAMING_FRAME, true, }, // Difference
        { CAPTURE_SCALED32, FRAMING_SPECIAL, },     // Average
        { CAPTURE_SCALED64, },                      // Extended
    },
    .scaling = true,
};

/*  pos_out adc
 *      Triggered       SCALED32
 *      Average         ADC_MEAN                FRAME   */
static struct output_class pos_out_adc_output_class = {
    .enum_set = {
        .enums = (struct enum_entry[]) {
            { 0, "No", },
            { 1, "Triggered", },
            { 2, "Average", },
        },
        .count = 3,
    },
    .output_options = (struct output_options[]) {
        { CAPTURE_SCALED32, },                      // Triggered
        { CAPTURE_ADC_MEAN, FRAMING_FRAME, },       // Average
    },
    .scaling = true,
};

/*  ext_out
 *  ext_out offset
 *  ext_out adc_count
 *  ext_out bits <group>
 *      Capture         UNSCALED                        */
static struct output_class ext_out_output_class = {
    .enum_set = {
        .enums = (struct enum_entry[]) {
            { 0, "No", },
            { 1, "Capture", },
        },
        .count = 2,
    },
    .output_options = (struct output_options[]) {
        { CAPTURE_UNSCALED, },                     // Capture
    },
};

/*  ext_out timestamp
 *      Capture         TS_NORMAL
 *      Frame           TS_OFFSET                       */
static struct output_class ext_out_timestamp_output_class = {
    .enum_set = {
        .enums = (struct enum_entry[]) {
            { 0, "No", },
            { 1, "Capture", },
            { 2, "Frame", },
        },
        .count = 3,
    },
    .output_options = (struct output_options[]) {
        { CAPTURE_TS_NORMAL, },                     // Capture
        { CAPTURE_TS_OFFSET, },                     // Frame
    },
};


/* Gather all the output classes together to help with initialisation. */
static struct output_class *output_classes[] = {
    &pos_out_output_class,           // pos_out
    &pos_out_encoder_output_class,   // pos_out encoder
    &pos_out_adc_output_class,       // pos_out adc
    &ext_out_output_class,           // ext_out [offset|adc_count|bits <n>]
    &ext_out_timestamp_output_class, // ext_out timestamp
};


/* Mapping from output type to associated info. */
static const struct output_type_info {
    const char *description;
    const struct output_class *output_class;
    enum prepare_class prepare_class;
    bool pos_type;
    bool extra_values;
} output_type_info[] = {
    [OUTPUT_POSN] = {
        .description = "pos_out",
        .output_class = &pos_out_output_class,
        .prepare_class = PREPARE_CLASS_NORMAL,
        .pos_type = true,
    },
    [OUTPUT_CONST] = {
        .description = "pos_out const",
        .output_class = NULL,
        .prepare_class = PREPARE_CLASS_NORMAL,
        .pos_type = true,
    },
    [OUTPUT_POSN_ENCODER] = {
        .description = "pos_out encoder",
        .output_class = &pos_out_encoder_output_class,
        .prepare_class = PREPARE_CLASS_NORMAL,
        .pos_type = true,
        .extra_values = true,
    },
    [OUTPUT_ADC] = {
        .description = "pos_out adc",
        .output_class = &pos_out_adc_output_class,
        .prepare_class = PREPARE_CLASS_NORMAL,
        .pos_type = true,
        .extra_values = true,
    },
    [OUTPUT_EXT] = {
        .description = "ext_out",
        .output_class = &ext_out_output_class,
        .prepare_class = PREPARE_CLASS_NORMAL,
        .pos_type = false,
    },
    [OUTPUT_TIMESTAMP] = {
        .description = "ext_out timestamp",
        .output_class = &ext_out_timestamp_output_class,
        .prepare_class = PREPARE_CLASS_TIMESTAMP,
        .pos_type = false,
    },
    [OUTPUT_OFFSET] = {
        .description = "ext_out offset",
        .output_class = &ext_out_output_class,
        .prepare_class = PREPARE_CLASS_TS_OFFSET,
        .pos_type = false,
    },
    [OUTPUT_ADC_COUNT] = {
        .description = "ext_out adc_count",
        .output_class = &ext_out_output_class,
        .prepare_class = PREPARE_CLASS_ADC_COUNT,
        .pos_type = false,
    },
    [OUTPUT_BITS] = {
        .description = "ext_out bits",
        .output_class = &ext_out_output_class,
        .prepare_class = PREPARE_CLASS_NORMAL,
        .pos_type = false,
    },
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Register parsing. */


error__t add_mux_indices(
    struct enumeration *lookup, struct field *field,
    const unsigned int array[], size_t length)
{
    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < length; i ++)
    {
        char name[MAX_NAME_LENGTH];
        format_field_name(name, sizeof(name), field, NULL, i, '\0');
        error = add_enumeration(lookup, name, array[i]);
    }
    return error;
}


static error__t assign_capture_values(
    struct output *output,
    unsigned int offset, unsigned int values[], unsigned int limit)
{
    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < output->count; i ++)
        error =
            TEST_OK_(values[i] < limit, "Capture index out of range")  ?:
            DO(output->values[i].capture_index[offset] = values[i]);
    return error;
}


static void register_this_output(struct output *output)
{
    unsigned int capture_index[output->count][2];
    for (unsigned int i = 0; i < output->count; i ++)
        memcpy(capture_index[i], output->values[i].capture_index,
            sizeof(capture_index[i]));
    enum prepare_class prepare_class =
        output_type_info[output->output_type].prepare_class;
    register_outputs(output, output->count, prepare_class, capture_index);
}


static error__t parse_timestamp_register(
    const char **line, struct output *output)
{
    return
        parse_whitespace(line)  ?:
        parse_uint_array(line, output->values[0].capture_index, 2);
}


static error__t parse_registers(
    const char **line, struct output *output, struct field *field)
{
    const struct output_type_info *info =
        &output_type_info[output->output_type];
    unsigned int values[output->count];
    unsigned int bus_limit =
        info->pos_type ? POS_BUS_COUNT : CAPTURE_BUS_COUNT;
    return
        /* First parse a position bus or extension bus index for each
         * instance and assign to our instances. */
        parse_whitespace(line)  ?:
        parse_uint_array(line, values, output->count)  ?:
        assign_capture_values(output, 0, values, bus_limit)  ?:

        /* If these are position bus indices then add to the lookup list. */
        IF(info->pos_type,
            add_mux_indices(
                pos_mux_lookup, field, values, output->count))  ?:

        /* Finally parse any extra values if requred.  These are separated
         * by a / and fill in the second values of each instance. */
        IF(info->extra_values,
            parse_whitespace(line)  ?:
            parse_char(line, '/')  ?:
            parse_whitespace(line)  ?:
            parse_uint_array(line, values, output->count)  ?:
            assign_capture_values(output, 1, values, CAPTURE_BUS_COUNT));
}


static error__t output_parse_register(
    void *class_data, struct field *field, unsigned int block_base,
    const char **line)
{
    struct output *output = class_data;
    output->field = field;
    return
        IF_ELSE(output->output_type == OUTPUT_TIMESTAMP,
            parse_timestamp_register(line, output),
        //else
            parse_registers(line, output, field))  ?:
        DO(register_this_output(output));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation. */


static struct output *create_output(
    unsigned int count, enum output_type output_type, unsigned int bit_group)
{
    size_t value_size = count * sizeof(struct output_value);
    struct output *output = malloc(sizeof(struct output) + value_size);
    *output = (struct output) {
        .output_class = output_type_info[output_type].output_class,
        .count = count,
        .output_type = output_type,
        .bit_group = bit_group,
    };
    memset(output->values, 0, value_size);
    return output;
}


static error__t complete_create_output(
    struct output *output, struct hash_table *attr_map, void **class_data)
{
    *class_data = output;

    if (output->output_type == OUTPUT_BITS)
        create_attributes(
            &bits_attr_methods, 1, NULL, output, output->count, attr_map);
    if (output->output_type != OUTPUT_CONST)
        create_attributes(
            &capture_attr_methods, 1, NULL, output, output->count, attr_map);
    return ERROR_OK;
}


/* Valid pos_out output types are default, adc, encoder, or const. */
static error__t parse_pos_out_type(
    const char **line, enum output_type *output_type)
{
    if (**line == '\0')
        *output_type = OUTPUT_POSN;
    else
    {
        char type_name[MAX_NAME_LENGTH];
        error__t error =
            parse_whitespace(line)  ?:
            parse_name(line, type_name, sizeof(type_name));
        if (error)
            return error;
        else if (strcmp(type_name, "adc") == 0)
            *output_type = OUTPUT_ADC;
        else if (strcmp(type_name, "const") == 0)
            *output_type = OUTPUT_CONST;
        else if (strcmp(type_name, "encoder") == 0)
            *output_type = OUTPUT_POSN_ENCODER;
        else
            return FAIL_("Unknown pos_out type");
    }
    return ERROR_OK;
}


static error__t pos_out_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data)
{
    enum output_type output_type = 0;
    const char *empty_line = "";
    struct output *output;
    return
        parse_pos_out_type(line, &output_type)  ?:
        DO(output = create_output(count, output_type, 0))  ?:
        create_type(
            &empty_line, "position", count, &register_methods, output,
            attr_map, &output->type)  ?:
        DO(output->position = get_type_state(output->type))  ?:
        complete_create_output(output, attr_map, class_data);
}


/* Quite a few more options for an ext_out type! */
static error__t parse_ext_out_type(
    const char **line, enum output_type *output_type, unsigned int *bit_group)
{
    if (**line == '\0')
        *output_type = OUTPUT_EXT;
    else
    {
        char type_name[MAX_NAME_LENGTH];
        error__t error =
            parse_whitespace(line)  ?:
            parse_name(line, type_name, sizeof(type_name));

        if (error)
            return error;
        else if (strcmp(type_name, "timestamp") == 0)
            *output_type = OUTPUT_TIMESTAMP;
        else if (strcmp(type_name, "offset") == 0)
            *output_type = OUTPUT_OFFSET;
        else if (strcmp(type_name, "bits") == 0)
        {
            *output_type = OUTPUT_BITS;
            error =
                parse_whitespace(line)  ?:
                parse_uint(line, bit_group);
        }
        else if (strcmp(type_name, "adc_count") == 0)
            *output_type = OUTPUT_ADC_COUNT;
        else
            return FAIL_("Unknown ext_out type");
    }
    return ERROR_OK;
}


static error__t ext_out_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data)
{
    enum output_type output_type = 0;
    unsigned int bit_group = 0;
    return
        parse_ext_out_type(line, &output_type, &bit_group)  ?:
        complete_create_output(
            create_output(count, output_type, bit_group), attr_map, class_data);
}


static void pos_out_destroy(void *class_data)
{
    struct output *output = class_data;
    destroy_type(output->type);
    free(output);
}


static const char *output_describe(void *class_data)
{
    struct output *output = class_data;
    return output_type_info[output->output_type].description;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Startup and shutdown. */


error__t initialise_output(void)
{
    pos_mux_lookup = create_dynamic_enumeration(POS_BUS_COUNT);

    for (unsigned int i = 0; i < ARRAY_SIZE(output_classes); i ++)
    {
        struct output_class *output_class = output_classes[i];
        output_class->enumeration =
            create_static_enumeration(&output_class->enum_set);
    }
    return ERROR_OK;
}


void terminate_output(void)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(output_classes); i ++)
    {
        struct output_class *output_class = output_classes[i];
        if (output_class->enumeration)
            destroy_enumeration(output_class->enumeration);
    }
    if (pos_mux_lookup)
        destroy_enumeration(pos_mux_lookup);
}


/* pos_mux type initialisation and destruction.  We only need the pos_mux
 * destructor to protect the shared pos_mux_lookup from being deleted. */

static error__t pos_mux_init(
    const char **string, unsigned int count, void **type_data)
{
    *type_data = pos_mux_lookup;
    return ERROR_OK;
}

static void pos_mux_destroy(void *type_data, unsigned int count)
{
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Published class definitions. */

const struct type_methods pos_mux_type_methods = {
    "pos_mux",
    .init = pos_mux_init,
    .destroy = pos_mux_destroy,
    .parse = enum_parse,
    .format = enum_format,
    .get_enumeration = enum_get_enumeration,
};

const struct class_methods pos_out_class_methods = {
    "pos_out",
    .init = pos_out_init,
    .parse_register = output_parse_register,
    .destroy = pos_out_destroy,
    .get = output_get, .refresh = pos_out_refresh,
    .describe = output_describe,
    .change_set = pos_out_change_set,
    .change_set_index = CHANGE_IX_POSITION,
};

const struct class_methods ext_out_class_methods = {
    "ext_out",
    .init = ext_out_init,
    .parse_register = output_parse_register,
    .describe = output_describe,
};
