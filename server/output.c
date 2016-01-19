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
#include "fields.h"
#include "classes.h"
#include "attributes.h"
#include "types.h"
#include "locking.h"
#include "mux_lookup.h"
#include "enums.h"
#include "capture.h"

#include "output.h"


/* The global state consists of capture configuration and most recently read
 * values for bit and position fields.  All bits, and separately all positions,
 * are updated in a single operation, so access to this state is protected by a
 * mutex. */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/* Capture masks.  These encode the current capture selection for each output
 * field.  For bits there is a complication: although capture can be configured
 * for each bit output, capture actually occurs in blocks of 32 bits.  Thus the
 * hardware capture mask will be reduced from bit_capture[] when written. */
static uint32_t bit_capture[BIT_BUS_COUNT / 32];
static uint32_t pos_capture_mask;
static uint32_t pos_framing_mask;
static uint32_t pos_extension_mask;

/* Capture indices for the four captured *BITS fields. */
static int bit_capture_index[BIT_BUS_COUNT / 32];
static int pos_capture_index[POS_BUS_COUNT];

/* Current values for each output field. */
static bool bit_value[BIT_BUS_COUNT];
static uint32_t pos_value[POS_BUS_COUNT];

/* Update indices for output fields. */
static uint64_t bit_update_index[BIT_BUS_COUNT];
static uint64_t pos_update_index[POS_BUS_COUNT];



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Capture enumeration. */

/* Update the bit and pos capture index arrays.  This needs to be called each
 * time either capture mask is written. */
static void update_capture_index(void)
{
    int capture_index = 0;

    /* Position capture. */
    for (unsigned int i = 0; i < POS_BUS_COUNT; i ++)
        if (pos_capture_mask & (1U << i))
            pos_capture_index[i] = capture_index++;
        else
            pos_capture_index[i] = -1;

    /* Bit capture. */
    for (unsigned int i = 0; i < BIT_BUS_COUNT / 32; i ++)
        if (bit_capture[i])
            bit_capture_index[i] = capture_index++;
        else
            bit_capture_index[i] = -1;
}


void report_capture_list(struct connection_result *result)
{
    /* Position capture. */
    for (unsigned int i = 0; i < POS_BUS_COUNT; i ++)
        if (pos_capture_mask & (1U << i))
            result->write_many(
                result->write_context,
                mux_lookup_get_name(&pos_mux_lookup, i));

    /* Bit capture. */
    for (unsigned int i = 0; i < BIT_BUS_COUNT / 32; i ++)
        if (bit_capture[i])
        {
            char string[MAX_NAME_LENGTH];
            snprintf(string, sizeof(string), "*BITS%d", i);
            result->write_many(result->write_context, string);
        }

    result->response = RESPONSE_MANY;
}


void report_capture_bits(struct connection_result *result, unsigned int group)
{
    for (unsigned int i = 0; i < 32; i ++)
        result->write_many(result->write_context,
            mux_lookup_get_name(&bit_mux_lookup, 32*group + i) ?: "");
    result->response = RESPONSE_MANY;
}


void report_capture_positions(struct connection_result *result)
{
    for (unsigned int i = 0; i < POS_BUS_COUNT; i ++)
        result->write_many(result->write_context,
            mux_lookup_get_name(&pos_mux_lookup, i) ?: "");
    result->response = RESPONSE_MANY;
}


void reset_capture_list(void)
{
    for (unsigned int i = 0; i < BIT_BUS_COUNT / 32; i ++)
        bit_capture[i] = 0;
    pos_capture_mask = 0;
    update_capture_index();
}


void write_capture_masks(void)
{
    uint32_t bit_capture_mask = 0;
    for (unsigned int i = 0; i < BIT_BUS_COUNT / 32; i ++)
        if (bit_capture[i])
            bit_capture_mask |= 1U << i;
    hw_write_capture_masks(
        bit_capture_mask, pos_capture_mask,
        pos_framing_mask, pos_extension_mask);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Value update. */

/* The refresh methods are called when we need a fresh value.  We retrieve
 * values and changed bits from the hardware and update settings accordingly. */

void do_bit_out_refresh(uint64_t change_index)
{
    LOCK(mutex);
    bool changes[BIT_BUS_COUNT];
    hw_read_bits(bit_value, changes);
    for (unsigned int i = 0; i < BIT_BUS_COUNT; i ++)
        if (changes[i]  &&  change_index > bit_update_index[i])
            bit_update_index[i] = change_index;
    UNLOCK(mutex);
}

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


static void bit_out_refresh(void *class_data, unsigned int number)
{
    do_bit_out_refresh(get_change_index());
}

static void pos_out_refresh(void *class_data, unsigned int number)
{
    do_pos_out_refresh(get_change_index());
}


/*****************************************************************************/
/* Bit/Position specific interface methods. */


/* This structure is used to help maintain a common implementation between
 * bit_out and pos_out classes. */
struct output_class_methods {
    void (*set_capture)(unsigned int ix, unsigned int value);
    unsigned int (*get_capture)(unsigned int ix);
    uint32_t (*get_value)(unsigned int ix);
    error__t (*format_index)(unsigned int ix, char result[], size_t length);
};


static void update_bit(uint32_t *target, unsigned int ix, bool value)
{
    *target = (*target & ~(1U << ix)) | ((uint32_t) value << ix);
}

static uint32_t get_bit(uint32_t *target, unsigned int ix)
{
    return (*target >> ix) & 1;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* bit methods. */

/* bit capture is controlled by the bit_capture[] array. */

static void bit_set_capture(unsigned int ix, unsigned int value)
{
    update_bit(&bit_capture[ix / 32], ix % 32, value & 1);
    update_capture_index();
}


static unsigned int bit_get_capture(unsigned int ix)
{
    return get_bit(&bit_capture[ix / 32], ix % 32);
}


static uint32_t bit_get_value(unsigned int ix)
{
    return bit_value[ix];
}


static error__t bit_format_index(
    unsigned int ix, char result[], size_t length)
{
    int capture_index = bit_capture_index[ix / 32];
    return
        IF_ELSE(capture_index >= 0,
            format_string(result, length, "%d:%d", capture_index, ix % 32),
        // else
            DO(*result = '\0'));
}


static const struct output_class_methods bit_output_methods = {
    .set_capture = bit_set_capture,
    .get_capture = bit_get_capture,
    .get_value = bit_get_value,
    .format_index = bit_format_index,
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* pos methods. */

/* position capture is controlled through the three capture masks, with the
 * corresponding bit numbers:
 *  0:  pos_capture_mask
 *  1:  pos_framing_mask
 *  2:  pos_extension_mask */

static void pos_set_capture(unsigned int ix, unsigned int value)
{
    update_bit(&pos_capture_mask, ix, value & 1);
    update_bit(&pos_framing_mask, ix, (value >> 1) & 1);
    update_bit(&pos_extension_mask, ix, (value >> 2) & 1);
    update_capture_index();
}


static unsigned int pos_get_capture(unsigned int ix)
{
    return
        (get_bit(&pos_capture_mask, ix) << 0) |
        (get_bit(&pos_framing_mask, ix) << 1) |
        (get_bit(&pos_extension_mask, ix) << 2);
}


static uint32_t pos_get_value(unsigned int ix)
{
    return pos_value[ix];
}


static error__t pos_format_index(
    unsigned int ix, char result[], size_t length)
{
    int capture_index = pos_capture_index[ix];
    return
        IF_ELSE(capture_index >= 0,
            format_string(result, length, "%d", capture_index),
        // else
            DO(*result = '\0'));
}


static const struct output_class_methods pos_output_methods = {
    .set_capture = pos_set_capture,
    .get_capture = pos_get_capture,
    .get_value = pos_get_value,
    .format_index = pos_format_index,
};


/******************************************************************************/
/* Individual field control. */

/* Bit and position outputs are read and managed through bit_out and pos_out
 * classes. */

/* Differing outputs have differing output options, this value is read from the
 * configuration file. */
enum output_type {
    OUTPUT_BIT,         // Single bit
    OUTPUT_POSN,        // Ordinary position
    OUTPUT_ADC,         // ADC => may have extended values
    OUTPUT_ENCODER,     // Encoders may have extended values
    OUTPUT_CONST,       // Constant value, cannot be captured

    OUTPUT_TYPE_SIZE    // Number of output type entries
};

static const char *output_type_names[OUTPUT_TYPE_SIZE] = {
    "bit", NULL, "adc", "encoder", "const",
};


/* We share the output state between bit and position classes to help with
 * sharing code, but the capture configuration for positions is somewhat more
 * complex. */
struct output_state {
    unsigned int count;             // Number of instances of this field
    enum output_type output_type;   // Selected output tupe
    const struct enumeration *enumeration; // Enumeration for CAPTURE control
    struct type *type;              // Type adapter for rendering value
    const struct output_class_methods *methods;   // Helper methods
    unsigned int index_array[];     // Field indices into global state
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Register access for type support: reads underlying value. */


static error__t register_read(
    void *reg_data, unsigned int number, uint32_t *result)
{
    struct output_state *state = reg_data;
    LOCK(mutex);
    *result = state->methods->get_value(state->index_array[number]);
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
    struct output_state *state = class_data;
    return type_get(state->type, number, result);
}


/* Computation of change set. */
static void bit_pos_change_set(
    struct output_state *state, const uint64_t update_index[],
    const uint64_t report_index, bool changes[])
{
    for (unsigned int i = 0; i < state->count; i ++)
        changes[i] = update_index[state->index_array[i]] > report_index;
}

static void bit_out_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    LOCK(mutex);
    bit_pos_change_set(class_data, bit_update_index, report_index, changes);
    UNLOCK(mutex);
}

static void pos_out_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    LOCK(mutex);
    bit_pos_change_set(class_data, pos_update_index, report_index, changes);
    UNLOCK(mutex);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Attributes: CAPTURE and CAPTURE_INDEX. */


/* Differing capture enumeration tables. */

static const struct enum_entry bit_capture_enums[] = {
    { 0, "No", },
    { 1, "Triggered", },
};

/* The position enumeration values are 3-bit masks corresponding to the
 * following fields (MSB to LSB):
 *      extension_mask, framing_mask, capture_mask  */

static const struct enum_entry position_capture_enums[] = {
    { 0, "No", },           // capture=0
    { 1, "Triggered", },    // capture=1 framing=0
    { 3, "Difference", },   // capture=1 framing=1
};

static const struct enum_entry adc_capture_enums[] = {
    { 0, "No", },           // capture=0
    { 1, "Triggered", },    // capture=1 framing=0 extension=0
    { 7, "Average", },      // capture=1 framing=1 extension=1
};

static const struct enum_entry encoder_capture_enums[] = {
    { 0, "No", },           // capture=0
    { 1, "Triggered", },    // capture=1 framing=0 extension=0
    { 3, "Difference", },   // capture=1 framing=1 extension=0
    { 5, "Extended", },     // capture=1 framing=0 extension=1
};

/* Array of enums indexed by output_type.  This must match the definitions of
 * output_type. */
#define ENUM_ENTRY(type) \
    { type##_capture_enums, ARRAY_SIZE(type##_capture_enums) }
static const struct enum_set capture_enum_sets[] = {
    ENUM_ENTRY(bit),
    ENUM_ENTRY(position),
    ENUM_ENTRY(adc),
    ENUM_ENTRY(encoder),
    { NULL, 0 },            // CONST: no enums defined
};

static const struct enumeration *capture_enums[OUTPUT_TYPE_SIZE];


static error__t capture_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct output_state *state = data;
    LOCK(mutex);
    unsigned int capture =
        state->methods->get_capture(state->index_array[number]);
    UNLOCK(mutex);

    const char *string;
    return
        TEST_OK(string = enum_index_to_name(state->enumeration, capture))  ?:
        format_string(result, length, "%s", string);
}


static error__t capture_put(
    void *owner, void *data, unsigned int number, const char *value)
{
    struct output_state *state = data;
    unsigned int capture;
    return
        TEST_OK_(
            enum_name_to_index(state->enumeration, value, &capture),
            "Not a valid capture option")  ?:

        /* Forbid any changes to the capture setup during capture. */
        WITH_CAPTURE_STATE(capture_state,
            TEST_OK_(capture_state != CAPTURE_ACTIVE, "Capture in progress")  ?:
            WITH_LOCK(mutex,
                DO(state->methods->set_capture(
                    state->index_array[number], capture))));
}


static error__t capture_index_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    struct output_state *state = data;
    return WITH_LOCK(mutex,
        state->methods->format_index(
            state->index_array[number], result, length));
}


static const struct enumeration *capture_get_enumeration(void *data)
{
    struct output_state *state = data;
    return state->enumeration;
}


static const struct attr_methods output_attr_methods[] =
{
    { "CAPTURE", "Configure capture for this field",
        .in_change_set = true,
        .format = capture_format,
        .put = capture_put,
        .get_enumeration = capture_get_enumeration,
    },
    { "CAPTURE_INDEX", "Position in output stream of this field",
        .format = capture_index_format,
    },
};



/******************************************************************************/
/* Initialisation. */


static error__t output_init(
    enum output_type output_type, const struct output_class_methods *methods,
    const char *type_name, unsigned int count,
    struct hash_table *attr_map, void **class_data)
{
    struct output_state *state =
        malloc(sizeof(struct output_state) + count * sizeof(unsigned int));

    *state = (struct output_state) {
        .count = count,
        .output_type = output_type,
        .methods = methods,
        .enumeration = capture_enums[output_type],
    };
    *class_data = state;

    const char *empty_line = "";
    return
        create_type(
            &empty_line, type_name, count, &register_methods, state,
            attr_map, &state->type)  ?:
        IF(output_type != OUTPUT_CONST,
            DO(create_attributes(
                output_attr_methods, ARRAY_SIZE(output_attr_methods),
                NULL, *class_data, count, attr_map)));
}


static error__t bit_out_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data)
{
    return output_init(
        OUTPUT_BIT, &bit_output_methods, "bit", count, attr_map, class_data);
}


/* Valid pos_out output types are default, adc, encoder, or const. */
static error__t parse_output_type(
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
            *output_type = OUTPUT_ENCODER;
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
        parse_output_type(line, &output_type)  ?:
        output_init(
            output_type, &pos_output_methods, "position",
            count, attr_map, class_data);
}


static void output_destroy(void *class_data)
{
    struct output_state *state = class_data;
    destroy_type(state->type);
    free(state);
}


static const char *pos_out_describe(void *class_data)
{
    struct output_state *state = class_data;
    return output_type_names[state->output_type];
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Register initialisation. */

static error__t parse_out_registers(
    struct output_state *state, const char **line, size_t limit)
{
    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < state->count; i ++)
        error =
            parse_whitespace(line)  ?:
            parse_uint(line, &state->index_array[i])  ?:
            TEST_OK_(state->index_array[i] < limit, "Mux index out of range");
    return error;
}

static error__t output_parse_register(
    struct mux_lookup *lookup, size_t length, struct output_state *state,
    struct field *field, const char **line)
{
    return
        parse_out_registers(state, line, length) ?:
        add_mux_indices(lookup, field, state->count, state->index_array);
}

static error__t bit_out_parse_register(
    void *class_data, struct field *field, unsigned int block_base,
    const char **line)
{
    return output_parse_register(
        &bit_mux_lookup, BIT_BUS_COUNT, class_data, field, line);
}

static error__t pos_out_parse_register(
    void *class_data, struct field *field, unsigned int block_base,
    const char **line)
{
    return output_parse_register(
        &pos_mux_lookup, POS_BUS_COUNT, class_data, field, line);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Startup and shutdown. */


error__t initialise_output(void)
{
    initialise_mux_lookup();
    update_capture_index();

    for (unsigned int i = 0; i < OUTPUT_TYPE_SIZE; i ++)
    {
        const struct enum_set *enum_set = &capture_enum_sets[i];
        if (enum_set->enums)
            capture_enums[i] = create_static_enumeration(enum_set);
        else
            capture_enums[i] = NULL;
    }
    return ERROR_OK;
}


void terminate_output(void)
{
    for (unsigned int i = 0; i < OUTPUT_TYPE_SIZE; i ++)
        if (capture_enums[i])
            destroy_enumeration(capture_enums[i]);
    terminate_mux_lookup();
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Published class definitions. */

const struct class_methods bit_out_class_methods = {
    "bit_out",
    .init = bit_out_init,
    .parse_register = bit_out_parse_register,
    .destroy = output_destroy,
    .get = output_get, .refresh = bit_out_refresh,
    .change_set = bit_out_change_set,
    .change_set_index = CHANGE_IX_BITS,
};

const struct class_methods pos_out_class_methods = {
    "pos_out",
    .init = pos_out_init,
    .parse_register = pos_out_parse_register,
    .destroy = output_destroy,
    .get = output_get, .refresh = pos_out_refresh,
    .describe = pos_out_describe,
    .change_set = pos_out_change_set,
    .change_set_index = CHANGE_IX_POSITION,
};
