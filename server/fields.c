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

#include "fields.h"


#define MAX_VALUE_LENGTH    64


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

    unsigned int reg;               // Register offset for this field
    uint64_t *change_index;         // A change counter for each field

    const struct field_class *class; // Implementation of field access methods
    const struct field_type *type;  // Implementation of type methods
    void *type_data;                // Data required for type support
};


/* A field class is an abstract interface implementing access functions. */
struct field_class {
    const char *name;
    /* Class initialisation.  Not needed by most classes, so may be null. */
    error__t (*init_class)(struct field *field);
    /* Class type lookup.  This allows the class to make its own choices about
     * the appropriate type handler.  Also null if not needed. */
    error__t (*init_type)(const char *type_name, struct field_type **type);
    /* Field access methods. */
    struct field_access access;
};


/* Each field knows its register address. */
static unsigned int read_field_register(const struct field_context *context)
{
    const struct field *field = context->field;
    printf("read_hw_register %d:%d:%d\n",
        field->block->base, context->number, field->reg);
    return 0;
}

static void write_field_register(
    const struct field_context *context, unsigned int value, bool mark_changed)
{
    const struct field *field = context->field;
    printf("write_hw_register %d:%d:%d <= %u\n",
        field->block->base, context->number, field->reg, value);
    if (mark_changed)
        __sync_fetch_and_add(&field->change_index[context->number], 1);
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
    const struct field_context *context, const struct connection_result *result)
{
    unsigned int value = read_field_register(context);
    result->write_one(context->connection, mux_lookup_index(mux_lookup, value));
    return ERROR_OK;
}


/* Converts multiplexer output name to index and writes to register. */
static error__t mux_put(
    struct mux_lookup *mux_lookup,
    const struct field_context *context, const char *name)
{
    unsigned int value;
    return
        mux_lookup_name(mux_lookup, name, &value)  ?:
        DO(write_field_register(
            context, (unsigned int) (uintptr_t) value, true));
}


static error__t bit_in_get(
    const struct field_context *context, const struct connection_result *result)
{
    return mux_get(bit_mux_lookup, context, result);
}

static error__t bit_in_put(
    const struct field_context *context, const char *value)
{
    return mux_put(bit_mux_lookup, context, value);
}

static error__t pos_in_get(
    const struct field_context *context, const struct connection_result *result)
{
    return mux_get(pos_mux_lookup, context, result);
}

static error__t pos_in_put(
    const struct field_context *context, const char *value)
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
    const struct field_context *context, const struct connection_result *result)
{
printf("value_get %p %p\n", context, result);
    struct type_context type_context = {
        .type_data = context->field->type_data,
        .number = context->number, };
    const struct type_access *access;
    return
        get_type_access(context->field->type, &access)  ?:
        DO(
            unsigned int value = read_field_register(context);
            char string[MAX_VALUE_LENGTH];
            access->format(&type_context, value, string, sizeof(string));
            result->write_one(context->connection, string));
}


/* General put method: parses string according to type and writes to register.
 * Depending on the class, a change notification is marked. */
static error__t value_put(
    const struct field_context *context, const char *string, bool mark_changed)
{
    struct type_context type_context = {
        .type_data = context->field->type_data,
        .number = context->number, };
    const struct type_access *access;
    unsigned int value;
    return
        get_type_access(context->field->type, &access)  ?:
        access->parse(&type_context, string, &value)  ?:
        DO(write_field_register(context, value, mark_changed));
}


/* Updates current parameter setting. */
static error__t param_put(
    const struct field_context *context, const char *string)
{
    return value_put(context, string, true);
}

/* Writes to write only register. */
static error__t write_put(
    const struct field_context *context, const char *string)
{
    return value_put(context, string, false);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Reads current table. */
static error__t table_get(
    const struct field_context *context, const struct connection_result *result)
{
    return FAIL_("table_get not implemented");
}


/* Writes to table. */
static error__t table_put_table(
    const struct field_context *context, bool append,
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


const struct field_access *get_field_access(const struct field *field)
{
    return &field->class->access;
}


bool walk_fields_list(
    const struct block *block, int *ix, const struct field **field)
{
    const char *key;
    return hash_table_walk_const(
        block->fields, ix, (const void **) &key, (const void **) field);
}


void get_field_info(
    const struct field *field,
    const char **field_name, const char **class_name)
{
    *field_name = field->name;
    *class_name = field->class->name;
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
        .reg = (unsigned int) -1,   // Placeholder for unassigned field
        .change_index = malloc(sizeof(uint64_t) * parent->count),
    };
    return field;
}


error__t create_field(
    struct field **field, const struct block *parent, const char *name,
    const char *class_name, const char *type_name)
{
    struct field_class *class;
    struct field_type *type = NULL;
    return
        /* Look up the field class. */
        TEST_OK_(class = hash_table_lookup(class_map, class_name),
            "Invalid field class %s", class_name)  ?:

        /* If appropriate process the class initialisation. */
        IF(class->init_type,
            class->init_type(type_name, &type))  ?:

        /* Now we can create and initialise the field structure. */
        DO(*field = create_field_block(parent, name, class, type))  ?:

        /* Insert the field into the blocks map of fields. */
        TEST_OK_(
            hash_table_insert(parent->fields, (*field)->name, *field) == NULL,
            "Field %s.%s alread exists", parent->name, name)  ?:

        /* Finally finish off any class specific initialisation. */
        IF(class->init_class,
            class->init_class(*field));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

error__t initialise_fields(void)
{
    /* Top level block map.  This will be initialised from the configuration
     * file. */
    block_map = hash_table_create(false);

    /* Class lookup map.  Used during configuration file loading. */
    class_map = hash_table_create(false);
    for (unsigned int i = 0; i < ARRAY_SIZE(classes_table); i ++)
    {
        const struct field_class *class = &classes_table[i];
        hash_table_insert_const(class_map, class->name, class);
    }

    return ERROR_OK;
}
