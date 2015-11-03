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
struct class {
    const char *name;   // Name of this class
    const char *type;   // Default type to use if no type specified

    /* Flags controlling behaviour of this class. */
    bool get;           // Fields can be read
    bool put;           // Fields can be written
    bool table;         // Special table support class
    bool changes;       // Fields support parameter change set readout

    /* Special mux index initialiser for the two _out classes. */
    error__t (*add_indices)(
        const char *block_name, const char *field_name, unsigned int count,
        unsigned int indices[]);
};


struct class_data {
    const struct field *field;
    unsigned int count;
    const struct class *class;
    const struct type *type;
    struct type_data *type_data;
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Mux class support.  This may move to types. */


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



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Table specific methods. */


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
/* Class access methods. */


static struct type_context create_type_context(
    const struct class_context *context)
{
    return (struct type_context) {
        .number = context->number,
        .type = context->class_data->type,
        .type_data = context->class_data->type_data,
    };
}


/* General get method: retrieves value from register associated with field,
 * formats the result according to the type, and returns the result. */
static error__t value_get(
    const struct class_context *context, const struct connection_result *result)
{
    const struct class_data *class_data = context->class_data;
    unsigned int value;
    struct type_context type_context = create_type_context(context);
    char string[MAX_VALUE_LENGTH];
    return
        TEST_OK_(class_data->class->get, "Field not readable")  ?:
        DO(value = read_field_register(class_data->field, context->number))  ?:
        type_format(&type_context, value, string, sizeof(string))  ?:
        DO(result->write_one(context->connection, string));
}


/*  block[n].field?
 * Implements reading from a field. */
error__t class_get(
    const struct class_context *context,
    const struct connection_result *result)
{
    if (context->class_data->class->table)
        return table_get(context, result);
    else
        return value_get(context, result);
}


/*  block[n].field=value
 * Implements writing to the field. */
/* General put method: parses string according to type and writes register. */
error__t class_put(
    const struct class_context *context, const char *string)
{
    const struct class *class = context->class_data->class;
    struct type_context type_context = create_type_context(context);
    unsigned int value;
    return
        TEST_OK_(class->put, "Field not writeable")  ?:
        type_parse(&type_context, string, &value)  ?:
        DO(write_field_register(
            context->class_data->field, context->number, value));
}


/*  block[n].field<
 * Implements writing to a table, for the one class which support this. */
error__t class_put_table(
    const struct class_context *context,
    bool append, const struct put_table_writer *writer)
{
    const struct class *class = context->class_data->class;
    return
        TEST_OK_(class->table, "Field is not a table")  ?:
        table_put_table(context, append, writer);
}


error__t class_add_indices(
    const struct class_data *class_data,
    const char *block_name, const char *field_name, unsigned int count,
    unsigned int indices[])
{
    const struct class *class = class_data->class;
    return
        TEST_OK_(class->add_indices, "Class does not have indices")  ?:
        class->add_indices(block_name, field_name, count, indices);
}


bool is_config_class(const struct class_data *class_data)
{
    return class_data->class->changes;
}


void report_changed_value(
    const char *block_name, const char *field_name, unsigned int number,
    const struct class_data *class_data,
    struct config_connection *connection,
    const struct connection_result *result)
{
    char string[MAX_VALUE_LENGTH];
    size_t prefix = (size_t) snprintf(
        string, sizeof(string), "%s%d.%s=", block_name, number, field_name);
    unsigned int value = read_field_register(class_data->field, number);
    error__t error = type_format(
        &(struct type_context) {
            .number = number,
            .type = class_data->type,
            .type_data = class_data->type_data },
        value, string + prefix, sizeof(string) - prefix);

    if (error)
    {
        /* Alas it is possible for an error to be detected during formatting.
         * In this case overwrite the = with space and write an error mark. */
        prefix -= 1;
        snprintf(string + prefix, sizeof(string) - prefix, " (error)");
        ERROR_REPORT(error, "Unexpected error during report_changed_value");
    }
    result->write_many(connection, string);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Top level list of classes. */

static const struct class classes_table[] = {
    /* Class        default                             change
     *  name        type        get     put     table   set     indices */
    {  "bit_in",    "bit_mux",  true,   true,   false,  true,   },
    {  "pos_in",    "pos_mux",  true,   true,   false,  true,   },
    {  "bit_out",   "bit",      true,   false,  false,  false,
        bit_out_add_indices, },
    {  "pos_out",   "position", true,   false,  false,  false,
        pos_out_add_indices, },
    {  "param",     "int",      true,   true,   false,  true,   },
    {  "read",      "int",      true,   false,  false,  false,  },
    {  "write",     "int",      false,  true,   false,  false,  },
    {  "table",     "table",    true,   false,  true,   false,  },
};

/* Used for lookup during initialisation, initialised from table above. */
static struct hash_table *class_map;


const char *get_class_name(const struct class_data *class_data)
{
    return class_data->class->name;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation and shutdown. */


static struct class_data *create_class_data(
    const struct field *field, unsigned int count,
    const struct class *class,
    const struct type *type, struct type_data *type_data)
{
    struct class_data *class_data = malloc(sizeof(struct class_data));
    *class_data = (struct class_data) {
        .field = field,
        .count = count,
        .class = class,
        .type = type,
        .type_data = type_data,
    };
    return class_data;
}


error__t create_class(
    struct field *field, unsigned int count,
    const char *class_name, const char *type_name,
    struct class_data **class_data)
{
    const struct class *class;
    const struct type *type;
    struct type_data *type_data;
    return
        TEST_OK_(class = hash_table_lookup(class_map, class_name),
            "Invalid field class %s", class_name)  ?:
        /* If no type name has been specified, use the default type for the
         * class. */
        create_type(
            *type_name == '\0' ? class->type : type_name, &type, &type_data)  ?:
        /* Finally create the class data structure. */
        DO(*class_data =
            create_class_data(field, count, class, type, type_data));
}


void destroy_class(struct class_data *class_data)
{
    destroy_type(class_data->type, class_data->type_data);
    free(class_data);
}


error__t initialise_classes(void)
{
    /* Class lookup map.  Used during configuration file loading. */
    class_map = hash_table_create(false);
    for (unsigned int i = 0; i < ARRAY_SIZE(classes_table); i ++)
    {
        const struct class *class = &classes_table[i];
        hash_table_insert_const(class_map, class->name, class);
    }

    return ERROR_OK;
}


void terminate_classes(void)
{
    if (class_map)
        hash_table_destroy(class_map);
}
