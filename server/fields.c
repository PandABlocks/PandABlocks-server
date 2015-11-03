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

    /* Data needed by the class. */
    struct class_data *class_data;
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Each field is backed by exactly one hardware register. */


/* This number is used to work out which fields have changed since we last
 * looked.  This is incremented on every write. */
static uint64_t change_index;


/* Allocates and returns a fresh change index. */
static uint64_t get_change_index(void)
{
    return __sync_add_and_fetch(&change_index, 1);
}


unsigned int read_field_register(
    const struct field *field, unsigned int number)
{
    return hw_read_config(field->block->base, number, field->reg);
}


void write_field_register(
    const struct field *field, unsigned int number, unsigned int value)
{
    hw_write_config(field->block->base, number, field->reg, value);
    field->change_index[number] = get_change_index();
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Method dispatch down to class. */


/* Helper method to convert incoming field_context into a class_context suitable
 * for calling the appropriate class method.  We pass down quite a lot of
 * structure from the field. */
static struct class_context create_class_context(
    const struct field_context *context)
{
    return (struct class_context) {
        .number = context->number,
        .connection = context->connection,
        .class_data = context->field->class_data,
    };
}


error__t field_get(
    const struct field_context *context,
    const struct connection_result *result)
{
    struct class_context class_context = create_class_context(context);
    return class_get(&class_context, result);
}


error__t field_put(const struct field_context *context, const char *value)
{
    struct class_context class_context = create_class_context(context);
    return class_put(&class_context, value);
}


error__t field_put_table(
    const struct field_context *context,
    bool append, const struct put_table_writer *writer)
{
    struct class_context class_context = create_class_context(context);
    return class_put_table(&class_context, append, writer);
}


error__t attr_list_get(
    const struct field *field,
    struct config_connection *connection,
    const struct connection_result *result)
{
    return class_attr_list_get(field->class_data, connection, result);
}


static struct class_attr_context create_class_attr_context(
    const struct attr_context *context)
{
    return (struct class_attr_context) {
        .number = context->number,
        .connection = context->connection,
        .class_data = context->field->class_data,
        .attr = context->attr,
    };
}


/* Retrieves current value of field:  block<n>.field?  */
error__t attr_get(
    const struct attr_context *context,
    const struct connection_result *result)
{
    struct class_attr_context attr_context = create_class_attr_context(context);
    return class_attr_get(&attr_context, result);
}


/* Writes value to field:  block<n>.field=value  */
error__t attr_put(const struct attr_context *context, const char *value)
{
    struct class_attr_context attr_context = create_class_attr_context(context);
    return class_attr_put(&attr_context, value);
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
        char value[MAX_RESULT_LENGTH];
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


error__t lookup_attr(
    const struct field *field, const char *name, const struct attr **attr)
{
    return class_lookup_attr(field->class_data, name, attr);
}


const struct block *field_parent(const struct field *field)
{
    return field->block;
}


error__t field_list_get(
    const struct block *block,
    struct config_connection *connection,
    const struct connection_result *result)
{
    FOR_EACH_FIELD(block->fields, field)
    {
        char value[MAX_RESULT_LENGTH];
        snprintf(value, MAX_RESULT_LENGTH, "%s %s %s",
            field->name, get_class_name(field->class_data),
            get_type_name(field->class_data));
        result->write_many(connection, value);
    }
    result->write_many_end(connection);
    return ERROR_OK;
}


static void destroy_field(struct field *field)
{
    free(field->name);
    free(field->change_index);
    destroy_class(field->class_data);
    free(field);
}


error__t create_field(
    struct field **field, const struct block *parent, const char *name,
    const char *class_name, const char *type_name)
{
    *field = malloc(sizeof(struct field));
    **field = (struct field) {
        .block = parent,
        .name = strdup(name),
        .reg = UNASSIGNED_REGISTER,
        .change_index = calloc(sizeof(uint64_t), parent->count),
    };

    return
        /* Initialise the class specific field handling. */
        TRY_CATCH(
            create_class(
                *field, parent->count, class_name, type_name,
                &(*field)->class_data)  ?:
            /* Insert the field into the blocks map of fields. */
            TEST_OK_(
                hash_table_insert(
                    parent->fields, (*field)->name, *field) == NULL,
                "Field %s.%s alread exists", parent->name, name),

        //catch
            /* If field initialisation failed then discard it. */
            destroy_field(*field));
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
        field->class_data, field->block->name, field->name,
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


/* Walks all fields and generates a change event for all changed fields. */
void generate_changes_list(
    struct config_connection *connection,
    const struct connection_result *result)
{
    /* Get the change index for this connection and update it so the next
     * changes request will be up to date.  Use a fresh index for this. */
    uint64_t connection_index =
        update_connection_index(connection, get_change_index());
    /* Work through all fields in all blocks. */
    FOR_EACH_BLOCK(block_map, block)
    {
        FOR_EACH_FIELD(block->fields, field)
            if (is_config_class(field->class_data))
                for (unsigned int i = 0; i < block->count; i ++)
                    if (field->change_index[i] >= connection_index)
                        report_changed_value(
                            block->name, field->name, i, field->class_data,
                            connection, result);
    }
    result->write_many_end(connection);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation and shutdown. */


error__t initialise_fields(void)
{
    /* Top level block map.  This will be initialised from the configuration
     * file. */
    block_map = hash_table_create(false);
    return ERROR_OK;
}


/* We implement orderly shutdown so that we can easily detect memory leaks
 * during development. */

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
