/* Code for loading configuration and register databases during startup. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "error.h"
#include "hashtable.h"
#include "parse.h"
#include "config_server.h"
#include "fields.h"

#include "database.h"



/* Parses a block definition header.  This is simply a name, optionally followed
 * by a number in square brackets, and should be followed by a number of field
 * definitions. */
static error__t config_parse_header_line(
    void *context, const char *line, void **indent_context)
{
    /* Parse input of form <name> [ "[" <count> "]" ]. */
    char block_name[MAX_NAME_LENGTH];
    unsigned int count = 1;
    return
        parse_name(&line, block_name, sizeof(block_name))  ?:
        IF(read_char(&line, '['),
            parse_uint(&line, &count)  ?:
            parse_char(&line, ']'))  ?:
        parse_eos(&line)  ?:
        create_block((struct block **) indent_context, block_name, count);
}


/* Parses a field definition of the form:
 *      <class>     <name>      [<type>]
 * The type description is optional. */
static error__t config_parse_field_line(
    void *context, const char *line, void **indent_context)
{
    struct block *block = context;

    char class_name[MAX_NAME_LENGTH];
    char field_name[MAX_NAME_LENGTH];
    return
        parse_name(&line, class_name, sizeof(class_name))  ?:
        parse_whitespace(&line)  ?:
        parse_name(&line, field_name, sizeof(field_name))  ?:
        DO(line = skip_whitespace(line))  ?:
        create_field(
            (struct field **) indent_context,
            block, field_name, class_name, &line)  ?:
        parse_eos(&line);
}


/* For some types we can add extra attribute lines.  In particular, enumerations
 * are defined as attribute lines. */
static error__t config_parse_attribute(
    void *context, const char *line, void **indent_context)
{
    struct field *field = context;
    return
        field_parse_attribute(field, &line)  ?:
        parse_eos(&line);
}


static error__t config_parse_line(
    unsigned int indent, void *context, const char *line, void **indent_context)
{
    switch (indent)
    {
        case 0:
            return config_parse_header_line(context, line, indent_context);
        case 1:
            return config_parse_field_line(context, line, indent_context);
        case 2:
            return config_parse_attribute(context, line, indent_context);
        default:
            /* Should not happen, we've set maximum indent in call to
             * parse_indented_file below. */
            ASSERT_FAIL();
    }
}


static const struct indent_parser config_indent_parser = {
    .parse_line = config_parse_line,
};

static error__t load_config_database(const char *db_name)
{
    log_message("Loading configuration database from \"%s\"", db_name);
    return parse_indented_file(db_name, 2, &config_indent_parser);
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Register database loading. */


/* A block line just specifies block name and base address. */
static error__t register_parse_header_line(
    void *context, const char *line, void **indent_context)
{
    char block_name[MAX_NAME_LENGTH];
    unsigned int base;
    struct block *block;
    return
        parse_name(&line, block_name, sizeof(block_name))  ?:
        parse_whitespace(&line)  ?:
        parse_uint(&line, &base)  ?:
        parse_eos(&line)  ?:

        lookup_block(block_name, &block, NULL)  ?:
        DO(*indent_context = block)  ?:
        block_set_register(block, base);
}


/* We push most of the register line parsing down to the field implementation,
 * all we do here is look up the field name and skip whitespace. */
static error__t register_parse_field_line(
    void *context, const char *line, void **indent_context)
{
    struct block *block = context;
    char field_name[MAX_NAME_LENGTH];
    struct field *field;
    return
        parse_name(&line, field_name, sizeof(field_name))  ?:
        lookup_field(block, field_name, &field)  ?:
        field_parse_register(field, &line)  ?:
        parse_eos(&line);
}


static error__t register_parse_line(
    unsigned int indent, void *context, const char *line, void **indent_context)
{
    switch (indent)
    {
        case 0:
            return register_parse_header_line(context, line, indent_context);
        case 1:
            return register_parse_field_line(context, line, indent_context);
        default:
            /* Should not happen, we've set maximum indent in call to
             * parse_indented_file below. */
            ASSERT_FAIL();
    }
}


static const struct indent_parser register_indent_parser = {
    .parse_line = register_parse_line,
};

static error__t load_register_database(const char *db_name)
{
    log_message("Loading register database from \"%s\"", db_name);
    return parse_indented_file(db_name, 1, &register_indent_parser);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static error__t load_description_database(const char *db_name)
{
    log_message("Loading description database from \"%s\"", db_name);
    return ERROR_OK;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

error__t load_config_databases(
    const char *config_db, const char *register_db,
    const char *description_db)
{
    return
        load_config_database(config_db)  ?:
        load_register_database(register_db)  ?:
        load_description_database(description_db)  ?:
        validate_database();
}


void terminate_databases(void)
{
    /* Seems to be nothing to do. */
}
