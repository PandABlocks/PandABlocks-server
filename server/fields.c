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
#include "hardware.h"

#include "fields.h"


#define MAX_VALUE_LENGTH    64

#define UNASSIGNED_REGISTER ((unsigned int) -1)


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Core structure definitions. */

/* The top level entities are blocks. Each block has a name, a number of
 * instances, a register base used for all block register methods, and a table
 * of fields. */
struct block {
    char *name;                 // Block name
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
    char *name;                     // Field name

    /* These fields are used by {read,write}_field_register. */
    unsigned int reg;               // Register offset for this field
    uint64_t *change_index;         // A change counter for each field

    /* These fields are used by the class. */
    const struct field_class *class; // Implementation of field access methods
    const struct field_type *type;  // Implementation of type methods
    void *type_data;                // Data required for type support
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Each field is backed by exactly one hardware register. */

/* Each field knows its register address. */
unsigned int read_field_register(
    const struct field *field, unsigned int number)
{
    printf("read_hw_register %d:%d:%d\n",
        field->block->base, number, field->reg);
    return hw_read_config(field->block->base, number, field->reg);
}


void write_field_register(
    const struct field *field, unsigned int number,
    unsigned int value, bool mark_changed)
{
    printf("write_hw_register %d:%d:%d <= %u\n",
        field->block->base, number, field->reg, value);
    hw_write_config(field->block->base, number, field->reg, value);
    if (mark_changed)
        __sync_fetch_and_add(&field->change_index[number], 1);
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Method dispatch down to class. */

static struct class_context create_class_context(
    const struct field_context *context)
{
    return (struct class_context) {
        .field = context->field,
        .number = context->number,
        .type = context->field->type,
        .type_data = context->field->type_data,
        .connection = context->connection,
    };
}


error__t field_get(
    const struct field_context *context,
    const struct connection_result *result)
{
    struct class_context class_context = create_class_context(context);
    const struct class_access *access = get_class_access(context->field->class);
    return
        TEST_OK_(access->get, "Field not readable")  ?:
        access->get(&class_context, result);
}


error__t field_put(const struct field_context *context, const char *value)
{
    struct class_context class_context = create_class_context(context);
    const struct class_access *access = get_class_access(context->field->class);
    return
        TEST_OK_(access->put, "Field not writeable")  ?:
        access->put(&class_context, value);
}


error__t field_put_table(
    const struct field_context *context,
    bool append, const struct put_table_writer *writer)
{
    struct class_context class_context = create_class_context(context);
    const struct class_access *access = get_class_access(context->field->class);
    return
        TEST_OK_(access->put_table, "Field is not a table")  ?:
        access->put_table(&class_context, append, writer);
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Top level block and field API. */

/* Map of block names. */
static struct hash_table *block_map;

/* A couple of helpers for walking the block and field maps. */
#define _FOR_EACH_TYPE(ix, key, type, test, map, value) \
    int ix = 0; \
    const void *key; \
    for (type value; \
         (test)  &&  hash_table_walk(map, &ix, &key, (void **) &value); )

#define FOR_EACH_TYPE(args...) \
    _FOR_EACH_TYPE(UNIQUE_ID(), UNIQUE_ID(), args)
#define FOR_EACH_BLOCK_WHILE(args...) FOR_EACH_TYPE(struct block *, args)
#define FOR_EACH_FIELD_WHILE(args...) FOR_EACH_TYPE(struct field *, args)
#define FOR_EACH_BLOCK(args...) FOR_EACH_BLOCK_WHILE(true, args)
#define FOR_EACH_FIELD(args...) FOR_EACH_FIELD_WHILE(true, args)


error__t lookup_block(const char *name, struct block **block)
{
    return TEST_OK_(*block = hash_table_lookup(block_map, name),
        "No such block");
}


unsigned int get_block_count(const struct block *block)
{
    return block->count;
}


error__t block_list_get(
    struct config_connection *connection,
    const struct connection_result *result)
{
    FOR_EACH_BLOCK(block_map, block)
    {
        char value[MAX_VALUE_LENGTH];
        snprintf(value, sizeof(value), "%s %d", block->name, block->count);
        result->write_many(connection, value);
    }
    result->write_many_end(connection);
    return ERROR_OK;
}


error__t create_block(
    struct block **block, const char *name, unsigned int count)
{
    *block = malloc(sizeof(struct block));
    **block = (struct block) {
        .name = strdup(name),
        .count = count,
        .base = UNASSIGNED_REGISTER,
        .fields = hash_table_create(false),
    };
    return TEST_OK_(
        hash_table_insert(block_map, (*block)->name, *block) == NULL,
        "Block %s already exists", name);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


error__t lookup_field(
    const struct block *block, const char *name, struct field **field)
{
    return TEST_OK_(*field = hash_table_lookup(block->fields, name),
        "No such field");
}


const struct block *field_parent(const struct field *field)
{
    return field->block;
}


error__t block_fields_get(
    const struct block *block,
    struct config_connection *connection,
    const struct connection_result *result)
{
    FOR_EACH_FIELD(block->fields, field)
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


static error__t assign_register(unsigned int *dest, unsigned int value)
{
    return
        TEST_OK_(*dest == UNASSIGNED_REGISTER, "Register already assigned")  ?:
        DO(*dest = value);
}

error__t block_set_base(struct block *block, unsigned int base)
{
    return assign_register(&block->base, base);
}

error__t field_set_reg(struct field *field, unsigned int reg)
{
    return assign_register(&field->reg, reg);
}

error__t mux_set_indices(struct field *field, unsigned int indices[])
{
    return class_add_indices(
        field->class, field->block->name, field->name,
        field->block->count, indices);
}


/* We ensure that every block and field have a valid register assigned. */
error__t validate_database(void)
{
    error__t error = ERROR_OK;

    FOR_EACH_BLOCK_WHILE(!error, block_map, block)
    {
        error = TEST_OK_(block->base != UNASSIGNED_REGISTER,
            "No base address for block %s", block->name);

        FOR_EACH_FIELD_WHILE(!error, block->fields, field)
            error = TEST_OK_(field->reg != UNASSIGNED_REGISTER,
                "No register for field %s.%s", block->name, field->name);
    }
    return error;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

error__t initialise_fields(void)
{
    /* Top level block map.  This will be initialised from the configuration
     * file. */
    block_map = hash_table_create(false);
    return ERROR_OK;
}


static void destroy_field(struct field *field)
{
    free(field->name);
    free(field->change_index);
    free(field->type_data);
    free(field);
}

static void destroy_block(struct block *block)
{
    FOR_EACH_FIELD(block->fields, field)
        destroy_field(field);
    hash_table_destroy(block->fields);
    free(block->name);
    free(block);
}

void terminate_fields(void)
{
    if (block_map)
    {
        FOR_EACH_BLOCK(block_map, block)
            destroy_block(block);
        hash_table_destroy(block_map);
    }
}
