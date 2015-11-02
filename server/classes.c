#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "hashtable.h"
#include "parse.h"
#include "mux_lookup.h"
#include "config_server.h"
#include "types.h"
#include "fields.h"

#include "classes.h"


#define MAX_NAME_LENGTH     64
#define MAX_VALUE_LENGTH    64


/* A field class is an abstract interface implementing access functions. */
struct field_class {
    const char *name;

    /* Special mux index initialiser for the two _out classes. */
    error__t (*add_indices)(
        const char *block_name, const char *field_name, unsigned int count,
        unsigned int indices[]);

    /*  block[n].field?
     * Implements reading from a field. */
    error__t (*get)(
        const struct class_context *context,
        const struct connection_result *result);

    /*  block[n].field=value
     * Implements writing to the field. */
    error__t (*put)(const struct class_context *context, const char *value);

    /*  block[n].field<
     * Implements writing to a table, for the one class which support this. */
    error__t (*put_table)(
        const struct class_context *context,
        bool append, const struct put_table_writer *writer);
};


struct class_data {
    const struct field *field;
    unsigned int count;
    struct field_class *class;
    const struct field_type *type;
    void *type_data;
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Mux class support. */


/* Adds each field name <-> index mapping to the appropriate multiplexer lookup
 * table. */
static error__t add_mux_indices(
    struct mux_lookup *mux_lookup,
    const char *block_name, const char *field_name, unsigned int count,
    unsigned int indices[])
{
    /* Add mux entries for our instances. */
    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < count; i ++)
    {
        char name[MAX_NAME_LENGTH];
        snprintf(name, sizeof(name), "%s%d.%s", block_name, i, field_name);
        error = mux_lookup_insert(mux_lookup, indices[i], name);
    }
    return error;
}


static error__t bit_out_add_indices(
    const char *block_name, const char *field_name, unsigned int count,
    unsigned int indices[])
{
    return add_mux_indices(
        bit_mux_lookup, block_name, field_name, count, indices);
}

static error__t pos_out_add_indices(
    const char *block_name, const char *field_name, unsigned int count,
    unsigned int indices[])
{
    return add_mux_indices(
        pos_mux_lookup, block_name, field_name, count, indices);
}


/* Class field access implementations for {bit,pos}_{in,out}. */

/* Converts register name to multiplexer name, or returns a placeholder if an
 * invalid value is read. */
static error__t mux_get(
    struct mux_lookup *mux_lookup,
    const struct class_context *context, const struct connection_result *result)
{
    unsigned int value = read_field_register(
        context->class_data->field, context->number);
    const char *string;
    return
        mux_lookup_index(mux_lookup, value, &string)  ?:
        DO(result->write_one(context->connection, string));
}


/* Converts multiplexer output name to index and writes to register. */
static error__t mux_put(
    struct mux_lookup *mux_lookup,
    const struct class_context *context, const char *name)
{
    unsigned int value;
    return
        mux_lookup_name(mux_lookup, name, &value)  ?:
        DO(write_field_register(
            context->class_data->field, context->number, value));
}


static error__t bit_in_get(
    const struct class_context *context, const struct connection_result *result)
{
    return mux_get(bit_mux_lookup, context, result);
}

static error__t bit_in_put(
    const struct class_context *context, const char *value)
{
    return mux_put(bit_mux_lookup, context, value);
}

static error__t pos_in_get(
    const struct class_context *context, const struct connection_result *result)
{
    return mux_get(pos_mux_lookup, context, result);
}

static error__t pos_in_put(
    const struct class_context *context, const char *value)
{
    return mux_put(pos_mux_lookup, context, value);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* General get method: retrieves value from register associated with field,
 * formats the result according to the type, and returns the result.  Will
 * always succeed. */
static error__t value_get(
    const struct class_context *context, const struct connection_result *result)
{
    const struct type_access *access;
    error__t error = get_type_access(context->class_data->type, &access);
    if (!error)
    {
        unsigned int value =
            read_field_register(context->class_data->field, context->number);

        struct type_context type_context = {
            .type_data = context->class_data->type_data,
            .number = context->number, };
        char string[MAX_VALUE_LENGTH];
        access->format(&type_context, value, string, sizeof(string));
        result->write_one(context->connection, string);
    }
    return error;
}


/* General put method: parses string according to type and writes to register.
 * Depending on the class, a change notification is marked. */
static error__t value_put(
    const struct class_context *context, const char *string)
{
    struct type_context type_context = {
        .type_data = context->class_data->type_data,
        .number = context->number, };
    const struct type_access *access;
    unsigned int value;
    return
        get_type_access(context->class_data->type, &access)  ?:
        access->parse(&type_context, string, &value)  ?:
        DO(write_field_register(
            context->class_data->field, context->number, value));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Reads current table. */
static error__t table_get(
    const struct class_context *context, const struct connection_result *result)
{
    return FAIL_("table_get not implemented");
}


/* Writes to table. */
static error__t table_put_table(
    const struct class_context *context, bool append,
    const struct put_table_writer *writer)
{
    return FAIL_("block.field< not implemented yet");
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Top level list of classes. */

static const struct field_class classes_table[] = {
    { "bit_in",
        .get = bit_in_get, .put = bit_in_put, },
    { "pos_in",
        .get = pos_in_get, .put = pos_in_put, },
    { "bit_out",
        .add_indices = bit_out_add_indices,
        .get = value_get, },
    { "pos_out",
        .add_indices = pos_out_add_indices,
        .get = value_get, },
    { "param",
        .get = value_get,  .put = value_put, },
    { "read",
        .get = value_get, },
    { "write",
        .put = value_put, },
    { "table",
        .get = table_get, .put_table = table_put_table },
};

/* Used for lookup during initialisation. */
static struct hash_table *class_map;


const char *get_class_name(const struct class_data *class_data)
{
    return class_data->class->name;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Class access methods. */


/*  block[n].field?
 * Implements reading from a field. */
error__t class_get(
    const struct class_context *context,
    const struct connection_result *result)
{
    return
        TEST_OK_(context->class_data->class->get, "Field not readable")  ?:
        context->class_data->class->get(context, result);
}


/*  block[n].field=value
 * Implements writing to the field. */
error__t class_put(const struct class_context *context, const char *value)
{
    return
        TEST_OK_(context->class_data->class->put, "Field not writeable")  ?:
        context->class_data->class->put(context, value);
}


/*  block[n].field<
 * Implements writing to a table, for the one class which support this. */
error__t class_put_table(
    const struct class_context *context,
    bool append, const struct put_table_writer *writer)
{
    return
        TEST_OK_(context->class_data->class->put_table,
            "Field is not a table")  ?:
        context->class_data->class->put_table(context, append, writer);
}


error__t class_add_indices(
    const struct class_data *class_data,
    const char *block_name, const char *field_name, unsigned int count,
    unsigned int indices[])
{
    return
        TEST_OK_(class_data->class->add_indices,
            "Class does not have indices")  ?:
        class_data->class->add_indices(block_name, field_name, count, indices);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation and shutdown. */


error__t create_class(
    struct field *field, unsigned int count,
    const char *class_name, const char *type_name,
    struct class_data **class_data)
{
    struct field_class *class;
    error__t error =
        TEST_OK_(class = hash_table_lookup(class_map, class_name),
            "Invalid field class %s", class_name);
    // At this point look up the type as well

    if (!error)
    {
        *class_data = malloc(sizeof(struct class_data));
        **class_data = (struct class_data) {
            .field = field,
            .count = count,
            .class = class,
        };
    }
    return error;
}


void destroy_class(struct class_data *class_data)
{
    free(class_data);
}


error__t initialise_classes(void)
{
    /* Class lookup map.  Used during configuration file loading. */
    class_map = hash_table_create(false);
    for (unsigned int i = 0; i < ARRAY_SIZE(classes_table); i ++)
    {
        const struct field_class *class = &classes_table[i];
        hash_table_insert_const(class_map, class->name, class);
    }

    return ERROR_OK;
}


void terminate_classes(void)
{
    if (class_map)
        hash_table_destroy(class_map);
}
