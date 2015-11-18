#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

#include "error.h"
#include "hashtable.h"
#include "parse.h"
#include "mux_lookup.h"
#include "config_server.h"
#include "fields.h"
#include "types.h"
#include "hardware.h"

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
    void (*init)(
        const struct class_methods *methods, unsigned int count,
        void **class_data);

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

    /* Access to table data. */
    error__t (*get_many)(
        struct class *class, unsigned int ix,
        const struct connection_result *result);
    error__t (*put_table)(
        struct class *class, unsigned int ix,
        bool append, struct put_table_writer *writer);

    /* Field attributes. */
    const struct attr *attrs;
    unsigned int attr_count;
};


struct class {
    const struct class_methods *methods;    // Class implementation
    unsigned int count;             // Number of instances of this block
    unsigned int block_base;        // Register base for block
    unsigned int field_register;    // Register for field (if required)
    void *class_data;               // Class specific data
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
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
    const struct connection_result *result)
{
    return FAIL_("Not implemented");
}


error__t class_put_table(
    struct class *class, unsigned int number,
    bool append, struct put_table_writer *writer)
{
    return FAIL_("Not implemented");
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Change support. */

/* This number is used to work out which fields have changed since we last
 * looked.  This is incremented on every update. */
static uint64_t global_change_index = 0;


/* Allocates and returns a fresh change index. */
uint64_t get_change_index(void)
{
    return __sync_add_and_fetch(&global_change_index, 1);
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
/* Attribute access. */

void class_attr_list_get(
    const struct class *class,
    const struct connection_result *result)
{
//     if (class->methods->attrs)
//         for (unsigned int i = 0; i < class->methods->attr_count; i ++)
//             result->write_many(connection, class->methods->attrs[i].name);
}


const struct attr *class_lookup_attr(struct class *class, const char *name)
{
//     const struct attr *attrs = class->methods->attrs;
//     if (attrs)
//         for (unsigned int i = 0; i < class->methods->attr_count; i ++)
//             if (strcmp(name, attrs[i].name) == 0)
//                 return &attrs[i];
    return NULL;
}


error__t class_attr_get(
    const struct class_attr_context *context,
    const struct connection_result *result)
{
    return FAIL_("Not implemented");
}


error__t class_attr_put(
    const struct class_attr_context *context, const char *value)
{
    return FAIL_("Not implemented");
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* bit_out and pos_out classes. */

/* For bit and pos out classes we read all the values together and record the
 * corresponding change indexes.  This means we need a global state structure to
 * record the last reading, together with index information for each class index
 * to identify the corresponding fields per class. */

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK(x)     ASSERT_PTHREAD(pthread_mutex_lock(&state_mutex))
#define UNLOCK(x)   ASSERT_PTHREAD(pthread_mutex_unlock(&state_mutex))

static struct bit_out_state {
    bool bits[BIT_BUS_COUNT];
    uint64_t change_index[BIT_BUS_COUNT];
} bit_out_state = { };

static struct pos_out_state {
    uint32_t positions[POS_BUS_COUNT];
    uint64_t change_index[POS_BUS_COUNT];
} pos_out_state = { };


/* The class specific state is just the index array. */
static void bit_pos_out_init(
    const struct class_methods *methods, unsigned int count, void **class_data)
{
    unsigned int *index_array = malloc(count * sizeof(unsigned int));
    for (unsigned int i = 0; i < count; i ++)
        index_array[i] = UNASSIGNED_REGISTER;
    *class_data = index_array;
}


/* For validation ensure that an index has been assigned to each field. */
static error__t bit_pos_out_validate(struct class *class)
{
    unsigned int *index_array = class->class_data;
    for (unsigned int i = 0; i < class->count; i ++)
        if (index_array[i] == UNASSIGNED_REGISTER)
            return FAIL_("Output selector not assigned");
    return ERROR_OK;
}


/* We fill in the index array and create name lookups at the same time. */

static error__t check_out_unassigned(
    unsigned int *index_array, unsigned int count)
{
    /* Check that we're starting with an unassigned field set. */
    for (unsigned int i = 0; i < count; i ++)
        if (index_array[i] != UNASSIGNED_REGISTER)
            return FAIL_("Output selection already assigned");
    return ERROR_OK;
}

static error__t parse_out_registers(
    const char **line, unsigned int count, unsigned int limit,
    unsigned int indices[])
{
    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < count; i ++)
        error =
            parse_whitespace(line)  ?:
            parse_uint(line, &indices[i])  ?:
            TEST_OK_(indices[i] < limit, "Mux index out of range");
    return error;
}

static error__t bit_pos_out_parse_register(
    struct mux_lookup *lookup, unsigned int limit,
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    unsigned int *index_array = class->class_data;
    return
        check_out_unassigned(index_array, class->count)  ?:
        parse_out_registers(line, class->count, limit, index_array) ?:
        add_mux_indices(
            lookup, block_name, field_name, class->count, index_array);
}

static error__t bit_out_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    return bit_pos_out_parse_register(
        bit_mux_lookup, BIT_BUS_COUNT, class, block_name, field_name, line);
}

static error__t pos_out_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    return bit_pos_out_parse_register(
        pos_mux_lookup, POS_BUS_COUNT, class, block_name, field_name, line);
}


/* The refresh method is called when we need a fresh value.  We retrieve values
 * and changed bits from the hardware and update settings accordingly. */

static void bit_out_refresh(struct class *class, unsigned int number)
{
    LOCK();
    uint64_t change_index = get_change_index();
    bool changes[BIT_BUS_COUNT];
    hw_read_bits(bit_out_state.bits, changes);
    for (unsigned int i = 0; i < BIT_BUS_COUNT; i ++)
        if (changes[i])
            bit_out_state.change_index[i] = change_index;
    UNLOCK();
}

static void pos_out_refresh(struct class *class, unsigned int number)
{
    LOCK();
    uint64_t change_index = get_change_index();
    bool changes[POS_BUS_COUNT];
    hw_read_positions(pos_out_state.positions, changes);
    for (unsigned int i = 0; i < POS_BUS_COUNT; i ++)
        if (changes[i])
            pos_out_state.change_index[i] = change_index;
    UNLOCK();
}


/* When reading just return the current value from our static state. */

static uint32_t bit_out_read(struct class *class, unsigned int number)
{
    unsigned int *index_array = class->class_data;
    return bit_out_state.bits[index_array[number]];
}

static uint32_t pos_out_read(struct class *class, unsigned int number)
{
    unsigned int *index_array = class->class_data;
    return pos_out_state.positions[index_array[number]];
}


/* Computation of change set. */
static void bit_pos_change_set(
    struct class *class, const uint64_t change_index[],
    const uint64_t report_index, bool changes[])
{
    unsigned int *index_array = class->class_data;
    for (unsigned int i = 0; i < class->count; i ++)
        changes[i] = change_index[index_array[i]] >= report_index;
}

static void bit_out_change_set(
    struct class *class, const uint64_t report_index[], bool changes[])
{
    bit_pos_change_set(
        class, bit_out_state.change_index,
        report_index[CHANGE_IX_BITS], changes);
}

static void pos_out_change_set(
    struct class *class, const uint64_t report_index[], bool changes[])
{
    bit_pos_change_set(
        class, pos_out_state.change_index,
        report_index[CHANGE_IX_POSITION], changes);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Parameters. */

/* All of bit_in, pos_in and param have very similar behaviour: values are
 * written to a register, the written value is cached for readback, and we keep
 * track of the report index. */

struct param_state {
    uint32_t value;
    uint64_t update_index;
};

static void param_init(
    const struct class_methods *methods, unsigned int count, void **class_data)
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


static void read_init(
    const struct class_methods *methods, unsigned int count, void **class_data)
{
    *class_data = calloc(count, sizeof(struct read_state));
}

static uint32_t read_read(struct class *class, unsigned int number)
{
    struct read_state *state = class->class_data;
    uint32_t result =
        hw_read_register(class->block_base, number, class->field_register);
    if (result != state[number].value)
    {
        state[number].value = result;
        state[number].update_index = get_change_index();
    }
    return result;
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
/* Table definitions. */

/* The register assignment for tables doesn't include a field register. */
static error__t table_validate(struct class *class)
{
    return ERROR_OK;
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


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Top level list of classes. */

static const struct class_methods classes_table[] = {
    { "bit_in", "bit_mux", true, PARAM_CLASS, },
    { "pos_in", "pos_mux", true, PARAM_CLASS, },
    { "param", "uint", PARAM_CLASS, },
    { "bit_out", "bit",
        .init = bit_pos_out_init,
        .parse_register = bit_out_parse_register,
        .validate = bit_pos_out_validate,
        .read = bit_out_read, .refresh = bit_out_refresh,
        .change_set = bit_out_change_set,
    },
    { "pos_out", "position",
        .init = bit_pos_out_init,
        .parse_register = pos_out_parse_register,
        .validate = bit_pos_out_validate,
        .read = pos_out_read, .refresh = pos_out_refresh,
        .change_set = pos_out_change_set,
    },
    { "read", "uint",
        .init = read_init,
        .validate = default_validate,
        .parse_register = default_parse_register,
        .read = read_read,
        .change_set = read_change_set,
    },
    { "write", "uint",
        .validate = default_validate,
        .parse_register = default_parse_register,
        .write = write_write,
    },
    { "table",
        .validate = table_validate,
    },
};


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
    const struct class_methods *methods,
    unsigned int count, unsigned int block_base, void *class_data)
{
    struct class *class = malloc(sizeof(struct class));
    *class = (struct class) {
        .methods = methods,
        .count = count,
        .block_base = block_base,
        .field_register = UNASSIGNED_REGISTER,
        .class_data = class_data,
    };
    return class;
}

error__t create_class(
    const char *class_name, const char **line,
    unsigned int block_base, unsigned int count,
    struct class **class, struct type **type)
{
    const struct class_methods *methods = NULL;
    void *class_data = NULL;
    const char *default_type;
    return
        lookup_class(class_name, &methods)  ?:
        DO(default_type = methods->default_type)  ?:
        IF(methods->init,
            DO(methods->init(methods, count, &class_data)))  ?:
        DO(*class = create_class_block(
            methods, count, block_base, class_data))  ?:

        /* Figure out which type to generate.  If a type is specified and we
         * don't consume it then an error will be reported. */
        IF(default_type,
            /* If no type specified use the default. */
            IF(**line == '\0', DO(line = &default_type))  ?:
            create_type(line, methods->force_type, count, type));
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

error__t validate_class(struct class *class)
{
    return
        IF(class->methods->validate,
            class->methods->validate(class));
}

const char *get_class_name(struct class *class)
{
    return class->methods->name;
}

void destroy_class(struct class *class)
{
    if (class->methods->destroy)
        class->methods->destroy(class);
    free(class->class_data);
    free(class);
}

error__t initialise_classes(void)
{
    return ERROR_OK;
}

void terminate_classes(void)
{
}
