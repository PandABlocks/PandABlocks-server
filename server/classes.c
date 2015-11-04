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



/* A field class is an abstract interface implementing access functions. */
struct class {
    const char *name;   // Name of this class
    const char *type;   // Default type to use if no type specified

    /* Flags controlling behaviour of this class. */
    bool force_type;    // If set only default type can be used
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
    void *type_data;
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
    char string[MAX_RESULT_LENGTH];
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


/*  block[n].field< */
error__t class_put_table(
    const struct class_context *context,
    bool append, const struct put_table_writer *writer)
{
    const struct class *class = context->class_data->class;
    return
        TEST_OK_(class->table, "Field is not a table")  ?:
        table_put_table(context, append, writer);
}


/*  block.field.*? */
error__t class_attr_list_get(
    const struct class_data *class_data,
    struct config_connection *connection,
    const struct connection_result *result)
{
    return type_attr_list_get(class_data->type, connection, result);
}


static struct type_attr_context create_type_attr_context(
    const struct class_attr_context *context)
{
    return (struct type_attr_context) {
        .number = context->number,
        .connection = context->connection,
        .field = context->class_data->field,
        .type_data = context->class_data->type_data,
        .attr = context->attr,
    };
}

/*  block[n].field.attr? */
error__t class_attr_get(
    const struct class_attr_context *context,
    const struct connection_result *result)
{
    struct type_attr_context attr_context = create_type_attr_context(context);
    return type_attr_get(&attr_context, result);
}


/*  block[n].field.attr=value */
error__t class_attr_put(
    const struct class_attr_context *context, const char *value)
{
    struct type_attr_context attr_context = create_type_attr_context(context);
    return type_attr_put(&attr_context, value);
}


error__t class_lookup_attr(
    const struct class_data *class_data, const char *name,
    const struct attr **attr)
{
    return type_lookup_attr(class_data->type, name, attr);
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


error__t class_add_attribute_line(
    const struct class_data *class_data, const char *line)
{
    return type_add_attribute_line(
        class_data->type, class_data->type_data, line);
}


bool is_config_class(const struct class_data *class_data)
{
    return class_data->class->changes;
}


/* To report a single changed value we have to re-do the normal formatting in
 * value_get, but we have one big issue: there's nowhere to report errors to, so
 * we have to handle them locally. */
void report_changed_value(
    const char *block_name, const char *field_name, unsigned int number,
    const struct class_data *class_data,
    struct config_connection *connection,
    const struct connection_result *result)
{
    char string[MAX_RESULT_LENGTH];
    size_t prefix = (size_t) snprintf(
        string, sizeof(string), "%s%d.%s=", block_name, number, field_name);

    /* Read and format register value into remainder of result string. */
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
    /* Class        default     force                   change
     *  name        type        class   get     put     table   set  */
    {  "bit_in",    "bit_mux",  true,   true,   true,   false,  true,   },
    {  "pos_in",    "pos_mux",  true,   true,   true,   false,  true,   },
    {  "bit_out",   "bit",      false,  true,   false,  false,  false,
        bit_out_add_indices, },
    {  "pos_out",   "position", false,  true,   false,  false,  false,
        pos_out_add_indices, },
    {  "param",     "uint",     false,  true,   true,   false,  true,   },
    {  "read",      "uint",     false,  true,   false,  false,  false,  },
    {  "write",     "uint",     false,  false,  true,   false,  false,  },
    {  "table",     "table",    true,   true,   false,  true,   false,  },
};

/* Used for lookup during initialisation, initialised from table above. */
static struct hash_table *class_map;


const char *get_class_name(const struct class_data *class_data)
{
    return class_data->class->name;
}

const char *get_type_name(const struct class_data *class_data)
{
    if (class_data->class->force_type)
        /* The name of forced types is concealed. */
        return "";
    else
        /* Ask the type for its name. */
        return type_get_type_name(class_data->type);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation and shutdown. */


static struct class_data *create_class_data(
    const struct field *field, unsigned int count,
    const struct class *class,
    const struct type *type, void *type_data)
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
    bool no_type_name = *type_name == '\0';

    const struct class *class;
    const struct type *type;
    void *type_data;
    return
        TEST_OK_(class = hash_table_lookup(class_map, class_name),
            "Invalid field class %s", class_name)  ?:
        /* Make sure no type name is specified if the type is forced. */
        TEST_OK_(no_type_name  ||  !class->force_type,
            "Cannot specify type for this class")  ?:
        /* If no type name has been specified, use the default type for the
         * class. */
        create_type(
            no_type_name ? class->type : type_name, class->force_type, count,
            &type, &type_data)  ?:
        /* Finally create the class data structure. */
        DO(*class_data =
            create_class_data(field, count, class, type, type_data));
}


void destroy_class(struct class_data *class_data)
{
    if (class_data)
    {
        destroy_type(
            class_data->type, class_data->type_data, class_data->count);
        free(class_data);
    }
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
