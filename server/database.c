/* Entity definitions. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "error.h"
#include "hashtable.h"
#include "parse.h"

#include "database.h"




/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Config database parsing. */

/* Each field in an entity is of one of the following classes. */
enum field_class {
    FIELD_PARAM,            // Settable parameter
    FIELD_WRITE,            // Writeable action field
    FIELD_READ,             // Read only field
    FIELD_BIT_OUT,          // Bit on internal bit bus
    FIELD_BIT_IN,           // Multiplexer bit selection
    FIELD_POSITION_OUT,     // Position on internal position bus
    FIELD_POSITION_IN,      // Multiplexer position selection
    FIELD_TABLE,            // Table of values
};

/* The top level configuration database is a map from entity names to entities.
 * At present that's all we have. */
struct config_database {
    struct hash_table *map;             // Map from entity name to config block
};

/* Each entry in the entity map documents the entity. */
struct config_block {
    unsigned int count;                 // Number of entities of this kind
    struct hash_table *map;             // Map from field name to field
};

/* Each field has a detailed description. */
struct field_entry {
    enum field_class class;
    struct field_type *type;
};


static struct config_database config_database;



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Config database parsing. */


/* This array of class names is used to identify the appropriate field class
 * from an input string.  Note that the sequence of these names must match the
 * field_class definition. */
static const char *class_names[] = {
    "param",
    "write",
    "read",
    "bit_out",
    "bit_in",
    "pos_out",
    "pos_in",
    "table",
};

/* Converts class name to enum. */
static error__t lookup_class_name(const char *name, enum field_class *class)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(class_names); i ++)
        if (strcmp(name, class_names[i]) == 0)
        {
            *class = (enum field_class) i;
            return ERROR_OK;
        }
    /* Not present, fail. */
    return FAIL_("Field class %s not recognised", name);
}


static void *config_start(void)
{
    config_database.map = hash_table_create(true);
    return &config_database;
}


/* Parses a block definition header.  This is simply a name, optionally followed
 * by a number in square brackets, and should be followed by a number of field
 * definitions. */
static error__t config_parse_header_line(
    void *context, const char *line, void **indent_context)
{
    struct config_database *database = context;

    /* Parse input of form <name> [ "[" <count> "]" ]. */
    char name[MAX_NAME_LENGTH];
    unsigned int count = 1;
    error__t error =
        parse_name(&line, name, sizeof(name))  ?:
        IF(read_char(&line, '['),
            parse_uint(&line, &count)  ?:
            parse_char(&line, ']'))  ?:
        parse_eos(&line);

    if (!error)
    {
        /* Create a new configuration block with the computed name and count. */
        struct config_block *block = malloc(sizeof(struct config_block));
        block->count = count;
        block->map = hash_table_create(true);
        *indent_context = block;

        error = TEST_OK_(!hash_table_insert(database->map, name, block),
            "Entity name %s repeated", name);
    }
    return error;
}


/* Parses a field definition of the form:
 *      <class>     <name>      [<type>]
 * The type description is optional. */
static error__t config_parse_field_line(void *context, const char *line)
{
    struct config_block *block = context;

    char class_name[MAX_NAME_LENGTH];
    enum field_class class = 0;
    char field_name[MAX_NAME_LENGTH];
    error__t error =
        parse_name(&line, class_name, sizeof(class_name))  ?:
        lookup_class_name(class_name, &class)  ?:
        parse_whitespace(&line)  ?:
        parse_name(&line, field_name, sizeof(field_name))  ?:
        DO(line = skip_whitespace(line));

    if (!error)
    {
        struct field_entry *field = malloc(sizeof(struct field_entry));
        field->class = class;
        field->type = NULL;

        error = TEST_OK_(!hash_table_insert(block->map, field_name, field),
            "Field name %s repeated", field_name);
    }
    return error;
}


static error__t config_parse_line(
    unsigned int indent, void *context, const char *line, void **indent_context)
{
    switch (indent)
    {
        case 0:
            return config_parse_header_line(context, line, indent_context);
        case 1:
            *indent_context = NULL;
            return config_parse_field_line(context, line);
        default:
            /* Should not happen, we've set maximum indent in call to
             * parse_indented_file below. */
            ASSERT_FAIL();
    }
}


static const struct indent_parser config_indent_parser = {
    .start = config_start,
    .parse_line = config_parse_line,
};


static error__t load_config_database(const char *db_name)
{
    log_message("Loading configuration database from \"%s\"", db_name);
    return parse_indented_file(db_name, 1, &config_indent_parser);
}


bool walk_blocks_list(
    int *ix, const struct config_block **block,
    const char **name, unsigned int *count)
{
    bool ok = hash_table_walk_const(
        config_database.map, ix, (const void **) name, (const void **) block);
    if (ok)
        *count = (*block)->count;
    return ok;
}


const struct config_block *lookup_block(const char *name, unsigned int *count)
{
    const struct config_block *block =
        hash_table_lookup(config_database.map, name);
    if (block)
        *count = block->count;
    return block;
}


bool walk_fields_list(
    const struct config_block *block,
    int *ix, const struct field_entry **field, const char **name)
{
    return hash_table_walk_const(
        block->map, ix, (const void **) name, (const void **) field);
}


/* Looks up field by name. */
const struct field_entry *lookup_field(
    const struct config_block *block, const char *name)
{
    return hash_table_lookup(block->map, name);
}


/* Returns meta-data definition. */
const struct meta_field *lookup_meta(
    const struct field_entry *field, const char *name)
{
    printf("lookup_meta %p %s\n", field, name);
    return NULL;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static error__t load_types_database(const char *db)
{
    log_message("Loading types database from \"%s\"", db);
    return ERROR_OK;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static error__t load_register_database(const char *db)
{
    log_message("Loading register database from \"%s\"", db);
    return ERROR_OK;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

error__t load_config_databases(
    const char *config_db, const char *types_db, const char *register_db)
{
    return
        load_types_database(types_db)  ?:
        load_config_database(config_db)  ?:
        load_register_database(register_db);
}
