/* Fields and field classes. */

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
#include "classes.h"

#include "fields.h"


#define MAX_VALUE_LENGTH    64

#define UNASSIGNED_REGISTER ((unsigned int) -1)


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Core structure definitions. */

/* The top level entities are blocks. Each block has a name, a number of
 * instances, a register base used for all block register methods, and a table
 * of fields. */
struct block {
    const char *name;           // Block name
    unsigned int count;         // Number of instances of this block
    unsigned int base;          // Block register base
    struct hash_table *fields;  // Map from field name to fields
};


/* The field structure contains all of the data associated with a field.  Each
 * field has a name and a class and (depending on the class) possibly a type.
 * There is also a register associated with each field, and depending on the
 * class there may be further type specific data. */
struct field {
    const struct block *block;      // Parent block
    const char *name;               // Field name

    /* These fields are used by {read,write}_field_register. */
    unsigned int reg;               // Register offset for this field
    uint64_t *change_index;         // A change counter for each field

    /* These fields are used by the class. */
    const struct field_class *class; // Implementation of field access methods
    const struct field_type *type;  // Implementation of type methods
    void *type_data;                // Data required for type support
};



/* Each field knows its register address. */
error__t read_field_register(
    const struct field *field, unsigned int number, unsigned int *result)
{
    printf("read_hw_register %d:%d:%d\n",
        field->block->base, number, field->reg);
    return
        TEST_OK_(field->reg != UNASSIGNED_REGISTER, "No register assigned")  ?:
        DO(*result = 0);
}

error__t write_field_register(
    const struct field *field, unsigned int number,
    unsigned int value, bool mark_changed)
{
    printf("write_hw_register %d:%d:%d <= %u\n",
        field->block->base, number, field->reg, value);
    return
        TEST_OK_(field->reg != UNASSIGNED_REGISTER, "No register assigned")  ?:
        DO(
            if (mark_changed)
                __sync_fetch_and_add(&field->change_index[number], 1));
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Method dispatch down to class. */

#define CALL_CLASS_METHOD(method, error, context, args...) \
    struct class_context class_context = { \
        .field = context->field, \
        .number = context->number, \
        .type = context->field->type, \
        .type_data = context->field->type_data, \
        .connection = context->connection, \
    }; \
    const struct class_access *access = \
        get_class_access(context->field->class); \
    return \
        TEST_OK_(access->method, "Cannot " error " field")  ?: \
        access->method(&class_context, args);

error__t field_get(
    const struct field_context *context,
    const struct connection_result *result)
{
    CALL_CLASS_METHOD(get, "read", context, result);
}


error__t field_put(const struct field_context *context, const char *value)
{
    CALL_CLASS_METHOD(put, "write", context, value);
}


error__t field_put_table(
    const struct field_context *context,
    bool append, const struct put_table_writer *writer)
{
    CALL_CLASS_METHOD(put_table, "write table to", context, append, writer);
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Top level block and field API. */

/* Map of block names. */
static struct hash_table *block_map;


const struct block *lookup_block(const char *name)
{
    return hash_table_lookup(block_map, name);
}


bool walk_blocks_list(int *ix, const struct block **block)
{
    const char *key;
    return hash_table_walk_const(
        block_map, ix, (const void **) &key, (const void **) block);
}


void get_block_info(
    const struct block *block, const char **name, unsigned int *count)
{
    if (name)
        *name = block->name;
    if (count)
        *count = block->count;
}


error__t create_block(
    struct block **block, const char *name,
    unsigned int count, unsigned int base)
{
    *block = malloc(sizeof(struct block));
    **block = (struct block) {
        .name = strdup(name),
        .count = count,
        .base = base,
        .fields = hash_table_create(false),
    };
    return TEST_OK_(
        hash_table_insert(block_map, (*block)->name, *block) == NULL,
        "Block %s already exists", name);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


const struct field *lookup_field(const struct block *block, const char *name)
{
    return hash_table_lookup(block->fields, name);
}


error__t block_fields_get(
    const struct block *block,
    struct config_connection *connection,
    const struct connection_result *result)
{
    int ix = 0;
    const struct field *field;
    const void *key;
    while (hash_table_walk_const(
              block->fields, &ix, &key, (const void **) &field))
    {
        char value[MAX_VALUE_LENGTH];
        snprintf(value, MAX_VALUE_LENGTH,
            "%s %s", field->name, get_class_name(field->class));
        result->write_many(connection, value);
    }
    result->write_many_end(connection);
    return ERROR_OK;
}


static struct field *create_field_block(
    const struct block *parent, const char *name,
    const struct field_class *class, const struct field_type *type)
{
    struct field *field = malloc(sizeof(struct field));
    *field = (struct field) {
        .block = parent,
        .name = strdup(name),
        .class = class,
        .type = type,
        .reg = UNASSIGNED_REGISTER,   // Placeholder for unassigned field
        .change_index = malloc(sizeof(uint64_t) * parent->count),
    };
    return field;
}


error__t create_field(
    struct field **field, const struct block *parent, const char *name,
    const char *class_name, const char *type_name)
{
    const struct field_class *class;
    const struct field_type *type = NULL;
    return
        /* Look up the field class. */
        lookup_class(class_name, &class)  ?:

//         /* If appropriate process the class initialisation. */
//         IF(class->init_type,
//             class->init_type(type_name, &type))  ?:

        /* Now we can create and initialise the field structure. */
        DO(*field = create_field_block(parent, name, class, type))  ?:

        /* Insert the field into the blocks map of fields. */
        TEST_OK_(
            hash_table_insert(parent->fields, (*field)->name, *field) == NULL,
            "Field %s.%s alread exists", parent->name, name)  ?:

//         initialise_class(*field, class);
ERROR_OK;

//         /* Finally finish off any class specific initialisation. */
//         IF(class->init_class,
//             class->init_class(*field));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

error__t initialise_fields(void)
{
    /* Top level block map.  This will be initialised from the configuration
     * file. */
    block_map = hash_table_create(false);
    return ERROR_OK;
}
