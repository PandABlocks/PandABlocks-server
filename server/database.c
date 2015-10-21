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


/* We'll reject identifiers longer than this. */
#define MAX_NAME_LENGTH     20


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
static bool lookup_class_name(const char *name, enum field_class *class)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(class_names); i ++)
        if (strcmp(name, class_names[i]) == 0)
        {
            *class = (enum field_class) i;
            return true;
        }
    /* Not present, fail. */
    return FAIL_("Field class %s not recognised", name);
}


static void *config_start(void)
{
    printf("config_start\n");
    config_database.map = hash_table_create(true);
    return &config_database;
}


/* Parses a block definition header.  This is simply a name, optionally followed
 * by a number in square brackets, and should be followed by a number of field
 * definitions. */
static bool config_parse_header_line(
    void *context, const char *line, void **indent_context)
{
    struct config_database *database = context;

    /* Parse input of form <name> [ "[" <count> "]" ]. */
    char name[MAX_NAME_LENGTH];
    unsigned int count = 1;
    bool ok =
        parse_name(&line, name, sizeof(name))  &&
        IF(read_char(&line, '['),
            parse_uint(&line, &count)  &&
            parse_char(&line, ']'))  &&
        parse_eos(&line);

    if (ok)
    {
        /* Create a new configuration block with the computed name and count. */
        struct config_block *block = malloc(sizeof(struct config_block));
        block->count = count;
        block->map = hash_table_create(true);
        *indent_context = block;

        printf("Inserting %s[%d]\n", name, count);
        ok = TEST_OK_(!hash_table_insert(database->map, name, block),
            "Entity name %s repeated", name);
    }
    return ok;
}


/* Parses a field definition of the form:
 *      <class>     <name>      [<type>]
 * The type description is optional. */
static bool config_parse_field_line(void *context, const char *line)
{
    struct config_block *block = context;

    char class_name[MAX_NAME_LENGTH];
    enum field_class class;
    char field_name[MAX_NAME_LENGTH];
    bool ok =
        parse_name(&line, class_name, sizeof(class_name))  &&
        lookup_class_name(class_name, &class)  &&
        parse_whitespace(&line)  &&
        parse_name(&line, field_name, sizeof(field_name))  &&
        DO(line = skip_whitespace(line));

    if (ok)
    {
        struct field_entry *field = malloc(sizeof(struct field_entry));
        *field = (struct field_entry) {
            .class = class,
            .type = NULL,
        };
        ok = TEST_OK_(!hash_table_insert(block->map, field_name, field),
            "Field name %s repeated", field_name);
    }
    return ok;
}


static bool config_parse_line(
    unsigned int indent, void *context, const char *line, void **indent_context)
{
    printf("config_parse_line %d %p \"%s\"\n",
        indent, context, line);

    switch (indent)
    {
        case 0:
            return config_parse_header_line(
                context, line, indent_context);
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


static bool load_config_database(const char *db_name)
{
    log_message("Loading configuration database from \"%s\"", db_name);

    return parse_indented_file(db_name, 1, &config_indent_parser);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static bool load_types_database(const char *db)
{
    log_message("Loading types database from \"%s\"", db);
    return true;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static bool load_register_database(const char *db)
{
    log_message("Loading register database from \"%s\"", db);
    return true;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

bool load_config_databases(
    const char *config_db, const char *types_db, const char *register_db)
{
    return
        load_types_database(types_db)  &&
        load_config_database(config_db)  &&
        load_register_database(register_db)  &&
        FAIL_("ok");
}
