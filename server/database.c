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
/* Register database. */

/* We need to check the hardware register setup before loading normal blocks. */
static bool hw_checked = false;


static error__t register_parse_special_field(
    void *context, const char **line, struct indent_parser *parser)
{
    error__t (*set_named_register)(const char *, unsigned int) = context;

    char reg_name[MAX_NAME_LENGTH];
    unsigned int reg;
    return
        parse_name(line, reg_name, sizeof(reg_name))  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &reg)  ?:
        parse_eos(line)  ?:

        IF(set_named_register, set_named_register(reg_name, reg));
}


/* After completing *REG block record that we seen one and check the register
 * validation. */
static error__t register_parse_reg_end(void *context)
{
    hw_checked = true;
    return hw_validate();
}


static error__t register_parse_special_header(
    void *context, const char **line, struct indent_parser *parser)
{
    parser->parse_line = register_parse_special_field;
    char block_name[MAX_NAME_LENGTH];
    unsigned int base;
    error__t error =
        parse_char(line, '*')  ?:
        parse_name(line, block_name, sizeof(block_name))  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &base)  ?:
        parse_eos(line);

    if (!error)
    {
        if (strcmp(block_name, "REG") == 0)
        {
            /* *REG block, for special hardware registers.  We parse this
             * normally and assign registers. */
            hw_set_block_base(base);
            parser->end = register_parse_reg_end;
            parser->context = hw_set_named_register;
        }
        else if (strcmp(block_name, "DRV") == 0)
        {
            /* *DRV block, for kernel driver registers.  We parse this but throw
             * the results away. */
        }
        else
            error = FAIL_("Invalid special block");
    }
    return error;
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
        parse_alphanum_name(line, field_name, sizeof(field_name))  ?:
        lookup_field(block, field_name, &field)  ?:
        field_parse_registers(field, line);
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
/* Description database. */


static error__t description_parse_field_line(
    void *context, const char **line, struct indent_parser *parser)
{
    struct block *block = context;
    char field_name[MAX_NAME_LENGTH];
    struct field *field;
    const char *description;
    return
        parse_alphanum_name(line, field_name, sizeof(field_name))  ?:
        lookup_field(block, field_name, &field)  ?:
        parse_whitespace(line)  ?:
        parse_utf8_string(line, &description)  ?:
        field_set_description(field, description);
}


static error__t description_parse_block_line(
    void *context, const char **line, struct indent_parser *parser)
{
    parser->parse_line = description_parse_field_line;
    char block_name[MAX_NAME_LENGTH];
    struct block *block;
    const char *description;
    return
        parse_name(line, block_name, sizeof(block_name))  ?:
        lookup_block(block_name, &block, NULL)  ?:
        DO(parser->context = block)  ?:
        parse_whitespace(line)  ?:
        parse_utf8_string(line, &description)  ?:
        block_set_description(block, description);
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
