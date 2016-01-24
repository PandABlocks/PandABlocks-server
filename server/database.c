/* Code for loading configuration and register databases during startup. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>

#include "error.h"
#include "hardware.h"
#include "hashtable.h"
#include "parse.h"
#include "config_server.h"
#include "fields.h"

#include "database.h"


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Config database. */


/* For some types we can add extra attribute lines.  In particular, enumerations
 * are defined as attribute lines. */
static error__t config_parse_attribute(
    void *context, const char **line, struct indent_parser *parser)
{
    struct field *field = context;
    return field_parse_attribute(field, line);
}


/* Parses a field definition of the form:
 *      <name>      <class>     [<type>]
 * The type description is optional. */
static error__t config_parse_field_line(
    void *context, const char **line, struct indent_parser *parser)
{
    struct block *block = context;
    parser->parse_line = config_parse_attribute;
    return create_field(line, (struct field **) &parser->context, block);
}


/* Parses a block definition header.  This is simply a name, optionally followed
 * by a number in square brackets, and should be followed by a number of field
 * definitions. */
static error__t config_parse_header_line(
    void *context, const char **line, struct indent_parser *parser)
{
    /* Parse input of form <name> [ "[" <count> "]" ]. */
    char block_name[MAX_NAME_LENGTH];
    unsigned int count = 1;
    parser->parse_line = config_parse_field_line;
    return
        parse_name(line, block_name, sizeof(block_name))  ?:
        IF(read_char(line, '['),
            parse_uint(line, &count)  ?:
            parse_char(line, ']'))  ?:
        parse_eos(line)  ?:

        create_block((struct block **) &parser->context, block_name, count);
}


static error__t load_config_database(const char *config_dir)
{
    char db_name[PATH_MAX];
    snprintf(db_name, sizeof(db_name), "%s/config", config_dir);
    log_message("Loading configuration database from \"%s\"", db_name);

    struct indent_parser parser = { .parse_line = config_parse_header_line, };
    return parse_indented_file(db_name, 2, &parser);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Register database loading. */

/* We need to check the hardware register setup before loading normal blocks. */
static bool hw_checked = false;


static error__t register_parse_special_field(
    void *context, const char **line, struct indent_parser *parser)
{
    char reg_name[MAX_NAME_LENGTH];
    unsigned int reg;
    return
        parse_name(line, reg_name, sizeof(reg_name))  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &reg)  ?:
        parse_eos(line)  ?:

        hw_set_named_register(reg_name, reg);
}


static error__t register_parse_special_end(void *context)
{
    hw_checked = true;
    return hw_validate();
}


static error__t register_parse_special_header(
    void *context, const char **line, struct indent_parser *parser)
{
    parser->parse_line = register_parse_special_field;
    parser->end = register_parse_special_end;
    char block_name[MAX_NAME_LENGTH];
    unsigned int base;
    return
        parse_char(line, '*')  ?:
        parse_name(line, block_name, sizeof(block_name))  ?:
        TEST_OK_(strcmp(block_name, "REG") == 0, "Invalid special block")  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &base)  ?:
        parse_eos(line)  ?:

        DO(hw_set_block_base(base));
}


/* We push most of the register line parsing down to the field implementation,
 * all we do here is look up the field name and skip whitespace. */
static error__t register_parse_normal_field(
    void *context, const char **line, struct indent_parser *parser)
{
    struct block *block = context;
    char field_name[MAX_NAME_LENGTH];
    struct field *field;
    return
        parse_name(line, field_name, sizeof(field_name))  ?:
        lookup_field(block, field_name, &field)  ?:
        field_parse_register(field, line);
}


/* A block line just specifies block name and base address. */
static error__t register_parse_normal_header(
    void *context, const char **line, struct indent_parser *parser)
{
    parser->parse_line = register_parse_normal_field;
    char block_name[MAX_NAME_LENGTH];
    unsigned int base;
    struct block *block;
    return
        TEST_OK_(hw_checked, "Must define *REG block first")  ?:
        parse_name(line, block_name, sizeof(block_name))  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &base)  ?:
        parse_eos(line)  ?:

        lookup_block(block_name, &block, NULL)  ?:
        DO(parser->context = block)  ?:
        block_set_register(block, base);
}


static error__t register_parse_line(
    void *context, const char **line, struct indent_parser *parser)
{
    if (**line == '*')
        return register_parse_special_header(context, line, parser);
    else
        return register_parse_normal_header(context, line, parser);
}


static error__t load_register_database(const char *config_dir)
{
    char db_name[PATH_MAX];
    snprintf(db_name, sizeof(db_name), "%s/registers", config_dir);
    log_message("Loading register database from \"%s\"", db_name);

    struct indent_parser parser = { .parse_line = register_parse_line, };
    return
        parse_indented_file(db_name, 1, &parser)  ?:
        TEST_OK_(hw_checked, "Nothing found in register file");
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


static error__t description_parse_field_line(
    void *context, const char **line, struct indent_parser *parser)
{
    struct block *block = context;
    char field_name[MAX_NAME_LENGTH];
    struct field *field;
    return
        parse_name(line, field_name, sizeof(field_name))  ?:
        lookup_field(block, field_name, &field)  ?:
        parse_whitespace(line)  ?:
        field_set_description(field, *line)  ?:
        DO(*line += strlen(*line));
}


static error__t description_parse_block_line(
    void *context, const char **line, struct indent_parser *parser)
{
    parser->parse_line = description_parse_field_line;
    char block_name[MAX_NAME_LENGTH];
    struct block *block;
    return
        parse_name(line, block_name, sizeof(block_name))  ?:
        lookup_block(block_name, &block, NULL)  ?:
        DO(parser->context = block)  ?:
        parse_whitespace(line)  ?:
        block_set_description(block, *line)  ?:
        DO(*line += strlen(*line));
}


static error__t load_description_database(const char *config_dir)
{
    char db_name[PATH_MAX];
    snprintf(db_name, sizeof(db_name), "%s/description", config_dir);
    log_message("Loading description database from \"%s\"", db_name);

    struct indent_parser parser = {
        .parse_line = description_parse_block_line, };
    return parse_indented_file(db_name, 1, &parser);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

error__t load_config_databases(const char *config_dir)
{
    return
        TEST_OK_(config_dir, "Must specify configuration directory")  ?:
        load_config_database(config_dir)  ?:
        load_register_database(config_dir)  ?:
        load_description_database(config_dir)  ?:
        validate_fields();
}


void terminate_databases(void)
{
    /* Seems to be nothing to do. */
}
