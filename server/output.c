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
static pthread_mutex_t update_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Current values and update indices for each output field. */
static uint32_t pos_value[POS_BUS_COUNT];
static uint64_t pos_update_index[] = { [0 ... POS_BUS_COUNT-1] = 1 };

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
    OUTPUT_EXT_ADC,         // ext_out adc
};


/* This structure is shared between pos_out and ext_out implementations and
 * instances are registered with data capture. */
struct output {
    const struct output_type_info *info;    // Controlling structure
    unsigned int count;                 // Number of instances

    pthread_mutex_t capture_mutex;      // Mutex for capture
    struct attr *capture;               // Capture attribute
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
struct output_type_info {
    const char *description;            // Extra description string
    const struct output_capture *capture;   // Capture enumeration
    enum prepare_class prepare_class;   // Output class for prepare
    bool pos_type;                      // If available on position bus
    bool extra_values;                  // If second group of registers used
    bool bit_group;                     // If bit group processing needed
    bool create_type;                   // If type helper wanted
};


/* This structure determines the possible capture options for the output. */
struct output_capture {
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
    LOCK(update_mutex);
    bool changes[POS_BUS_COUNT];
    hw_read_positions(pos_value, changes);
    for (unsigned int i = 0; i < POS_BUS_COUNT; i ++)
        if (changes[i]  &&  change_index > pos_update_index[i])
            pos_update_index[i] = change_index;
    UNLOCK(update_mutex);
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
    LOCK(update_mutex);
    unsigned int capture_index = output->values[number].capture_index[0];
    *result = pos_value[capture_index];
    UNLOCK(update_mutex);
    return ERROR_OK;
}

static const struct register_methods pos_register_methods = {
    .read = register_read,
};

static const struct register_methods ext_register_methods = {
};


/* When reading just return the current value from our static state. */
static error__t output_get(
    void *class_data, unsigned int number, char result[], size_t length)
{
    struct output *output = class_data;
    if (output->type)
        return type_get(output->type, number, result, length);
    else
        /* This is something of a hack.  Stricly speaking we should raise an
         * error, but in practice this will be the right result! */
        return format_string(result, length, "0");
}


/* Computation of change set. */
static void pos_out_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct output *output = class_data;
    LOCK(update_mutex);
    for (unsigned int i = 0; i < output->count; i ++)
    {
        unsigned int capture_index = output->values[i].capture_index[0];
        changes[i] = pos_update_index[capture_index] > report_index;
    }
    UNLOCK(update_mutex);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Capture control and state retrieval. */


/* Returns capture state after fetching it under a lock. */
static unsigned int get_capture_state(
    struct output *output, unsigned int number)
{
    LOCK(output->capture_mutex);
    unsigned int capture_state = output->values[number].capture_state;
    UNLOCK(output->capture_mutex);
    return capture_state;
}


enum capture_mode get_capture_info(
    struct output *output, unsigned int number, struct capture_info *info)
{
    unsigned int capture_state = get_capture_state(output, number);
    if (capture_state == 0)
        return CAPTURE_OFF;
    else
    {
        const struct output_capture *capture = output->info->capture;
        const struct output_options *options =
            &capture->output_options[capture_state - 1];

        info->scaled = capture->scaling;
        if (capture->scaling)
        {
            get_position_info(
                output->position, number,
                &info->scaling.scale, &info->scaling.offset,
                info->units, sizeof(info->units));
            if (options->zero_offset)
                info->scaling.offset = 0.0;
        }
        info->capture_string = capture->enum_set.enums[capture_state].name;
        info->framing_mode = options->framing_mode;
        info->capture_mode = options->capture_mode;
        return options->capture_mode;
    }
}


bool get_capture_enabled(
    struct output *output, unsigned int number, const char **capture)
{
    unsigned int capture_state = get_capture_state(output, number);
    *capture = output->info->capture->enum_set.enums[capture_state].name;
    return capture_state > 0;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* CAPTURE attribute. */


void reset_output_capture(struct output *output, unsigned int number)
{
    LOCK(output->capture_mutex);
    if (output->values[number].capture_state != 0)
    {
        output->values[number].capture_state = 0;
        attr_changed(output->capture, number);
    }
    UNLOCK(output->capture_mutex);
}


static error__t capture_format(
    void *owner, void *data, unsigned int number, char result[], size_t length)
{
    struct output *output = data;
    unsigned int capture_state = get_capture_state(output, number);
    const char *string;
    return
        TEST_OK(string = enum_index_to_name(
            output->info->capture->enumeration, capture_state))  ?:
        format_string(result, length, "%s", string);
}


static error__t capture_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct output *output = data;
    unsigned int capture_state;
    return
        TEST_OK_(
            enum_name_to_index(
                output->info->capture->enumeration, value, &capture_state),
            "Not a valid capture option")  ?:
        WITH_LOCK(output->capture_mutex,
            DO(output->values[number].capture_state = capture_state));
}


static const struct enumeration *capture_get_enumeration(void *data)
{
    struct output *output = data;
    return output->info->capture->enumeration;
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
 *  ext_out adc
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
static struct output_capture pos_out_output_capture = {
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
static struct output_capture pos_out_encoder_output_capture = {
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
 *  ext_out adc
 *      Triggered       SCALED32
 *      Average         ADC_MEAN                FRAME   */
static struct output_capture pos_out_adc_output_capture = {
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
static struct output_capture ext_out_output_capture = {
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
static struct output_capture ext_out_timestamp_output_capture = {
    .enum_set = {
        .enums = (struct enum_entry[]) {
            { 0, "No", },
            { 1, "Trigger", },
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
static struct output_capture *output_capture_set[] = {
    &pos_out_output_capture,           // pos_out
    &pos_out_encoder_output_capture,   // pos_out encoder
    &pos_out_adc_output_capture,       // pos_out adc
    &ext_out_output_capture,           // ext_out [offset|adc_count|bits <n>]
    &ext_out_timestamp_output_capture, // ext_out timestamp
};


/* Mapping from output type to associated info. */
static const struct output_type_info output_type_info[] = {
    [OUTPUT_POSN] = {
        .description = NULL,
        .capture = &pos_out_output_capture,
        .prepare_class = PREPARE_CLASS_NORMAL,
        .pos_type = true,
        .create_type = true,
    },
    [OUTPUT_CONST] = {
        .description = "const",
        .capture = NULL,
        .prepare_class = PREPARE_CLASS_NORMAL,
        .pos_type = true,
    },
    [OUTPUT_POSN_ENCODER] = {
        .description = "encoder",
        .capture = &pos_out_encoder_output_capture,
        .prepare_class = PREPARE_CLASS_NORMAL,
        .pos_type = true,
        .extra_values = true,
        .create_type = true,
    },
    [OUTPUT_ADC] = {
        .description = "adc",
        .capture = &pos_out_adc_output_capture,
        .prepare_class = PREPARE_CLASS_NORMAL,
        .pos_type = true,
        .extra_values = true,
        .create_type = true,
    },
    [OUTPUT_EXT] = {
        .description = NULL,
        .capture = &ext_out_output_capture,
        .prepare_class = PREPARE_CLASS_NORMAL,
        .pos_type = false,
    },
    [OUTPUT_TIMESTAMP] = {
        .description = "timestamp",
        .capture = &ext_out_timestamp_output_capture,
        .prepare_class = PREPARE_CLASS_TIMESTAMP,
        .pos_type = false,
        .extra_values = true,
    },
    [OUTPUT_OFFSET] = {
        .description = "offset",
        .capture = &ext_out_output_capture,
        .prepare_class = PREPARE_CLASS_TS_OFFSET,
        .pos_type = false,
    },
    [OUTPUT_ADC_COUNT] = {
        .description = "adc_count",
        .capture = &ext_out_output_capture,
        .prepare_class = PREPARE_CLASS_ADC_COUNT,
        .pos_type = false,
    },
    [OUTPUT_BITS] = {
        .description = "bits",
        .capture = &ext_out_output_capture,
        .prepare_class = PREPARE_CLASS_NORMAL,
        .pos_type = false,
        .bit_group = true,
    },
    [OUTPUT_EXT_ADC] = {
        .description = "adc",
        .capture = &pos_out_adc_output_capture,
        .prepare_class = PREPARE_CLASS_NORMAL,
        .pos_type = false,
        .extra_values = true,
        .create_type = true,
    },
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Register parsing. */


/* This array of booleans is used to detect overlapping capture bus indexes. */
static bool bus_index_used[CAPTURE_BUS_COUNT];


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
            TEST_OK_(!bus_index_used[values[i]],
                "Capture index %u already used", values[i])  ?:
            DO( output->values[i].capture_index[offset] = values[i];
                bus_index_used[values[i]] = true);
    return error;
}


static error__t parse_register_list(
    const char **line, struct output *output,
    unsigned int values[], unsigned int offset, unsigned int bus_limit)
{
    return
        parse_whitespace(line)  ?:
        parse_uint_array(line, values, output->count)  ?:
        assign_capture_values(output, offset, values, bus_limit);
}


/* Registers all instances of this output with prepare.c for capture. */
static error__t register_this_output(
    struct output *output, struct field *field)
{
    enum prepare_class prepare_class = output->info->prepare_class;

    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < output->count; i ++)
    {
        char name[MAX_NAME_LENGTH];
        format_field_name(name, sizeof(name), field, NULL, i, '\0');
        error = register_output(
            output, i, name, prepare_class, output->values[i].capture_index);
    }
    return error;
}


static error__t parse_registers(
    const char **line, struct output *output, struct field *field)
{
    const struct output_type_info *info = output->info;
    unsigned int values[output->count];
    unsigned int bus_limit =
        info->pos_type ? POS_BUS_COUNT : CAPTURE_BUS_COUNT;
    return
        /* First parse a position bus or extension bus index for each
         * instance and assign to our instances. */
        parse_register_list(line, output, values, 0, bus_limit)  ?:

        /* If these are position bus indices then add to the lookup list. */
        IF(info->pos_type,
            add_mux_indices(
                pos_mux_lookup, field, values, output->count))  ?:

        /* Finally parse any extra values if requred.  These are separated
         * by a / and fill in the second values of each instance. */
        IF(info->extra_values,
            parse_whitespace(line)  ?:
            parse_char(line, '/')  ?:
            parse_register_list(line, output, values, 1, CAPTURE_BUS_COUNT));
}


static void register_bit_group(struct field *field, struct output *output)
{
    for (unsigned int i = 0; i < output->count; i ++)
    {
        char name[MAX_NAME_LENGTH];
        format_field_name(name, sizeof(name), field, NULL, i, '\0');
        set_bit_group_name(output->bit_group, name);
    }
}


static error__t output_parse_register(
    void *class_data, struct field *field, unsigned int block_base,
    const char **line)
{
    struct output *output = class_data;
    return
        parse_registers(line, output, field)  ?:
        IF(output->capture,
            register_this_output(output, field))  ?:
        IF(output->info->bit_group,
            DO(register_bit_group(field, output)));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation. */


static struct output *create_output(
    unsigned int count, enum output_type output_type, unsigned int bit_group)
{
    size_t value_size = count * sizeof(struct output_value);
    struct output *output = malloc(sizeof(struct output) + value_size);
    *output = (struct output) {
        .info = &output_type_info[output_type],
        .count = count,
        .bit_group = bit_group,
        .capture_mutex = PTHREAD_MUTEX_INITIALIZER,
    };
    memset(output->values, 0, value_size);
    return output;
}


static error__t complete_create_output(
    unsigned int count, enum output_type output_type, unsigned int bit_group,
    const struct register_methods *register_methods,
    struct hash_table *attr_map, void **class_data)
{
    struct output *output = create_output(count, output_type, bit_group);
    *class_data = output;

    if (output->info->bit_group)
        create_attributes(
            &bits_attr_methods, 1, NULL, output, output->count, attr_map);
    if (output->info->capture)
    {
        output->capture = create_attribute(
            &capture_attr_methods, NULL, output, output->count);
        hash_table_insert(attr_map, capture_attr_methods.name, output->capture);
    }

    const char *empty_line = "";
    return
        IF(output->info->create_type,
            create_type(
                &empty_line, "position", count, register_methods, output,
                attr_map, &output->type)  ?:
            DO(output->position = get_type_state(output->type)));
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
    return
        parse_pos_out_type(line, &output_type)  ?:
        complete_create_output(
            count, output_type, 0, &pos_register_methods, attr_map, class_data);
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
        else if (strcmp(type_name, "adc") == 0)
            *output_type = OUTPUT_EXT_ADC;
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
            count, output_type, bit_group,
            &ext_register_methods, attr_map, class_data);
}


static void output_destroy(void *class_data)
{
    struct output *output = class_data;
    if (output->type)
        destroy_type(output->type);
    free(output);
}


static const char *output_describe(void *class_data)
{
    struct output *output = class_data;
    return output->info->description;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Startup and shutdown. */


error__t initialise_output(void)
{
    pos_mux_lookup = create_dynamic_enumeration(POS_BUS_COUNT);

    for (unsigned int i = 0; i < ARRAY_SIZE(output_capture_set); i ++)
    {
        struct output_capture *capture = output_capture_set[i];
        capture->enumeration = create_static_enumeration(&capture->enum_set);
    }
    return ERROR_OK;
}


void terminate_output(void)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(output_capture_set); i ++)
    {
        struct output_capture *capture = output_capture_set[i];
        if (capture->enumeration)
            destroy_enumeration(capture->enumeration);
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


/* Returns list of all position bus entries. */
void report_capture_positions(struct connection_result *result)
{
    write_enum_labels(pos_mux_lookup, result);
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
    .destroy = output_destroy,
    .get = output_get, .refresh = pos_out_refresh,
    .describe = output_describe,
    .change_set = pos_out_change_set,
    .change_set_index = CHANGE_IX_POSITION,
};

const struct class_methods ext_out_class_methods = {
    "ext_out",
    .init = ext_out_init,
    .parse_register = output_parse_register,
    .destroy = output_destroy,
    .describe = output_describe,
};
