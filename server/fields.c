/* Fields and field classes. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "hashtable.h"
#include "parse.h"
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
    unsigned int sequence;          // Field sequence number

    struct class *class;            // Class defining hardware interface and
    struct type *type;              // Optional type handler
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Top level block and field API. */


/* Map of block names. */
static struct hash_table *block_map;


/* A couple of helpers for walking the block and field maps. */
#define _id_FOR_EACH_TYPE(ix, key, type, test, map, value) \
    int ix = 0; \
    const void *key; \
    for (type value; \
         (test)  &&  hash_table_walk(map, &ix, &key, (void **) &value); )
/* Called thus:
 *  FOR_EACH_TYPE(type, test, map, value) { loop }
 *
 * type     Type of values in map
 * test     Condition for testing loop: loops while test is true
 * map      Hash table containing set of value for iteration
 * value    Name of variable to which each value is assigned. */
#define FOR_EACH_TYPE(args...) \
    _id_FOR_EACH_TYPE(UNIQUE_ID(), UNIQUE_ID(), args)

#define FOR_EACH_BLOCK_WHILE(cond, block_var) \
    FOR_EACH_TYPE(struct block *, cond, block_map, block_var)
#define FOR_EACH_BLOCK(block_var) FOR_EACH_BLOCK_WHILE(true, block_var)

#define FOR_EACH_FIELD_WHILE(args...) FOR_EACH_TYPE(struct field *, args)
#define FOR_EACH_FIELD(args...) FOR_EACH_FIELD_WHILE(true, args)


error__t lookup_block(
    const char *name, struct block **block, unsigned int *count)
{
    return
        TEST_OK_(*block = hash_table_lookup(block_map, name),
            "No such block")  ?:
        DO(if (count)  *count = (*block)->count);
}


error__t lookup_field(
    const struct block *block, const char *name, struct field **field)
{
    return TEST_OK_(
        *field = hash_table_lookup(block->fields, name),
        "No such field");
}


error__t lookup_attr(
    const struct field *field, const char *name, const struct attr **attr)
{
    /* Both classes and types can have attributes.  Try the type attribute
     * first, fail if neither succeeds. */
    *attr = type_lookup_attr(field->type, name);
    if (*attr == NULL)
        *attr = class_lookup_attr(field->class, name);
    return TEST_OK_(*attr, "No such attribute");
}


error__t block_list_get(const struct connection_result *result)
{
    FOR_EACH_BLOCK(block)
    {
        char value[MAX_RESULT_LENGTH];
        snprintf(value, sizeof(value), "%s %d", block->name, block->count);
        result->write_many(result->connection, value);
    }
    result->write_many_end(result->connection);
    return ERROR_OK;
}


error__t field_list_get(
    const struct block *block, const struct connection_result *result)
{
    FOR_EACH_FIELD(block->fields, field)
    {
        char value[MAX_RESULT_LENGTH];
        int length = snprintf(value, sizeof(value), "%s %u %s",
            field->name, field->sequence, get_class_name(field->class));
        if (field->type)
            snprintf(value + length, sizeof(value) - (size_t) length, " %s",
                get_type_name(field->type));

        result->write_many(result->connection, value);
    }
    result->write_many_end(result->connection);
    return ERROR_OK;
}


/* Return union of type and class attributes. */
error__t attr_list_get(
    struct field *field,
    const struct connection_result *result)
{
    if (field->type)
        type_attr_list_get(field->type, result);
    class_attr_list_get(field->class, result);
    result->write_many_end(result->connection);
    return ERROR_OK;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Method dispatch down to class. */


error__t field_get(
    struct field *field, unsigned int number,
    const struct connection_result *result)
{
    /* We have two possible implementations: a single value get which we perform
     * by reading the register and formatting with the type, or else we need to
     * hand the implementation down to the class. */
    if (field->type)
    {
        uint32_t value;
        char string[MAX_RESULT_LENGTH];
        return
            class_read(field->class, &value, true)  ?:
            type_format(field->type, number, value, string, sizeof(string))  ?:
            DO(result->write_one(result->connection, string));
    }
    else
        return class_get(field->class, number, result);
}


error__t field_put(
    struct field *field, unsigned int number,
    const char *string)
{
    uint32_t value;
    return
        TEST_OK_(field->type, "Field not writeable")  ?:
        type_parse(field->type, number, string, &value)  ?:
        class_write(field->class, value);
}


error__t field_put_table(
    struct field *field, unsigned int number,
    bool append, struct put_table_writer *writer)
{
    return class_put_table(field->class, number, append, writer);
}


/* Retrieves current value of field:  block<n>.field?  */
error__t attr_get(
    const struct attr_context *context,
    const struct connection_result *result)
{
    return FAIL_("Not implemented");
//     struct class_attr_context attr_context = create_class_attr_context(context);
//     return class_attr_get(&attr_context, result);
}


/* Writes value to field:  block<n>.field=value  */
error__t attr_put(const struct attr_context *context, const char *value)
{
    return FAIL_("Not implemented");
//     struct class_attr_context attr_context = create_class_attr_context(context);
//     return class_attr_put(&attr_context, value);
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Change set management. */

static void report_changed_value(
    const struct field *field, unsigned int number,
    const struct connection_result *result)
{
    char string[MAX_RESULT_LENGTH];
    size_t prefix = (size_t) snprintf(
        string, sizeof(string), "%s%d.%s=",
        field->block->name, number, field->name);

    uint32_t value;
    error__t error =
        TEST_OK(field->type)  ?:        // A big surprise if this fails here
        class_read(field->class, &value, false)  ?:
        type_format(
            field->type, number, value,
            string + prefix, sizeof(string) - prefix);
    if (error)
    {
        /* Alas it is possible for an error to be detected during formatting.
         * In this case overwrite the = with space and write an error mark. */
        prefix -= 1;
        snprintf(string + prefix, sizeof(string) - prefix, " (error)");
        ERROR_REPORT(error, "Unexpected error during report_changed_value");
    }

    result->write_many(result->connection, string);
}


/* Walks all fields and generates a change event for all changed fields. */
void generate_change_sets(
    const struct connection_result *result, enum change_set change_set)
{
    /* Get the change index for this connection and update it so the next
     * changes request will be up to date.  Use a fresh index for this. */
    uint64_t report_index[CHANGE_SET_SIZE];
    update_change_index(
        result->connection, change_set, get_change_index(), report_index);

    /* Work through all fields in all blocks. */
    FOR_EACH_BLOCK(block)
    {
        FOR_EACH_FIELD(block->fields, field)
        {
            bool changes[block->count];
            memset(changes, 0, sizeof(changes));
            get_class_change_set(field->class, report_index, changes);
            for (unsigned int i = 0; i < block->count; i ++)
                if (changes[i])
                    report_changed_value(field, i, result);
        }
    }
    result->write_many_end(result->connection);
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

static void destroy_field(struct field *field)
{
    free(field->name);
    destroy_class(field->class);
    if (field->type)
        destroy_type(field->type);
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
        FOR_EACH_BLOCK(block)
            destroy_block(block);
        hash_table_destroy(block_map);
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Field and database creation and validation. */


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


error__t block_set_register(struct block *block, unsigned int base)
{
    return
        TEST_OK_(block->base == UNASSIGNED_REGISTER,
            "Register already assigned")  ?:
        DO(block->base = base);
}


static struct field *create_field_block(
    const struct block *block, const char *name)
{
    struct field *field = malloc(sizeof(struct field));
    *field = (struct field) {
        .block = block,
        .name = strdup(name),
        .sequence = (unsigned int) hash_table_count(block->fields),
    };
    return field;
}

error__t create_field(
    struct field **field, const struct block *block,
    const char *field_name, const char *class_name, const char **line)
{
    *field = create_field_block(block, field_name);
    return
        TRY_CATCH(
            create_class(class_name, line, block->count,
                &(*field)->class, &(*field)->type)  ?:
            /* Insert the field into the blocks map of fields. */
            TEST_OK_(
                hash_table_insert(
                    block->fields, (*field)->name, *field) == NULL,
                "Field %s.%s already exists", block->name, field_name),

        // catch
            destroy_field(*field)
        );
}


error__t field_parse_attribute(struct field *field, const char **line)
{
    return
        TEST_OK_(field->type, "Cannot add attribute to field")  ?:
        type_parse_attribute(field->type, line);
}


error__t field_parse_register(struct field *field, const char **line)
{
    return class_parse_register(
        field->class, field->block->name, field->name, line);
}


/* Ensure that every block and field has valid register assignments. */
error__t validate_database(void)
{
    error__t error = ERROR_OK;
    FOR_EACH_BLOCK_WHILE(!error, block)
    {
        error = TEST_OK_(block->base != UNASSIGNED_REGISTER,
            "No base address for block %s", block->name);
        FOR_EACH_FIELD_WHILE(!error, block->fields, field)
        {
            error = validate_class(field->class);
            if (error)
                error_extend(error,
                    "Checking field %s.%s", block->name, field->name);
        }
    }
    return error;
}
