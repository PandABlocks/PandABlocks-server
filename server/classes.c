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


#define MAX_VALUE_LENGTH    64


/* A field class is an abstract interface implementing access functions. */
struct field_class {
    const char *name;
    /* Class initialisation.  Not needed by most classes, so may be null. */
    error__t (*init_class)(struct field *field);
    /* Class type lookup.  This allows the class to make its own choices about
     * the appropriate type handler.  Also null if not needed. */
    error__t (*init_type)(const char *type_name, struct field_type **type);
    /* Field access methods. */
    struct class_access access;
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


const struct class_access *get_class_access(const struct field_class *class)
{
    return &class->access;
}

const char *get_class_name(const struct field_class *class)
{
    return class->name;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


#if 0
/* Adds mux entries for this field. */
static void mux_out_init_class(struct mux_lookup *lookup, struct field *field)
{
    /* Add mux entries for our instances. */
    for (int i = 0; i < field->count; i ++)
    {
        char name[MAX_NAME_LENGTH];
        snprintf(name, sizeof(name), "%s%d.%s",
            field->block->name, i, field->name);
        // what is ix ???????????
        mux_lookup_insert(lookup, ix, name);
    }
}
#endif


/* Class field access implementations for {bit,pos}_{in,out}. */

/* Converts register name to multiplexer name, or returns a placeholder if an
 * invalid value is read. */
static error__t mux_get(
    struct mux_lookup *mux_lookup,
    const struct class_context *context, const struct connection_result *result)
{
    unsigned int value;
    const char *string;
    return
        read_field_register(context->field, context->number, &value)  ?:
        DO(string = mux_lookup_index(mux_lookup, value))  ?:
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
        write_field_register(context->field, context->number, value, true);
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


#if 0
static void bit_out_init_class(struct field *field)
{
    mux_out_init_class(&bit_mux_lookup, field);
}

static void pos_out_init_class(struct field *field)
{
    mux_out_init_class(&pos_mux_lookup, field);
}
#endif


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* General get method: retrieves value from register associated with field,
 * formats the result according to the type, and returns the result.  Will
 * always succeed. */
static error__t value_get(
    const struct class_context *context, const struct connection_result *result)
{
printf("value_get %p %p\n", context, result);
    struct type_context type_context = {
        .type_data = context->type_data,
        .number = context->number, };
    const struct type_access *access;
    unsigned int value;
    return
        read_field_register(context->field, context->number, &value)  ?:
        get_type_access(context->type, &access)  ?:
        DO(
            char string[MAX_VALUE_LENGTH];
            access->format(&type_context, value, string, sizeof(string));
            result->write_one(context->connection, string));
}


/* General put method: parses string according to type and writes to register.
 * Depending on the class, a change notification is marked. */
static error__t value_put(
    const struct class_context *context, const char *string, bool mark_changed)
{
    struct type_context type_context = {
        .type_data = context->type_data,
        .number = context->number, };
    const struct type_access *access;
    unsigned int value;
    return
        get_type_access(context->type, &access)  ?:
        access->parse(&type_context, string, &value)  ?:
        write_field_register(
            context->field, context->number, value, mark_changed);
}


/* Updates current parameter setting. */
static error__t param_put(
    const struct class_context *context, const char *string)
{
    return value_put(context, string, true);
}

/* Writes to write only register. */
static error__t write_put(
    const struct class_context *context, const char *string)
{
    return value_put(context, string, false);
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
        .access = { .get = bit_in_get, .put = bit_in_put, } },
    { "pos_in",
        .access = { .get = pos_in_get, .put = pos_in_put, } },
    { "bit_out",
        .access = { .get = value_get, } },
    { "pos_out",
        .access = { .get = value_get, } },
    { "param",
        .access = { .get = value_get,  .put = param_put, } },
    { "read",
        .access = { .get = value_get, } },
    { "write",
        .access = { .put = write_put, } },
    { "table",
        .access = { .get = table_get, .put_table = table_put_table } },
};

/* Used for lookup during initialisation. */
static struct hash_table *class_map;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

error__t lookup_class(const char *name, const struct field_class **class)
{
    return TEST_OK_(
        *class = hash_table_lookup(class_map, name),
            "Invalid field class %s", name);
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
