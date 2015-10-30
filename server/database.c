/* Entity definitions. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "error.h"
#include "hashtable.h"
#include "parse.h"
#include "fields.h"

#include "database.h"



/* We'll reject identifiers longer than this. */
#define MAX_NAME_LENGTH     20


static void *config_start(void)
{
    return NULL;
}


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
        DO(line = skip_whitespace(line))  ?:
        parse_eos(&line)  ?:
        create_block((struct block **) indent_context, block_name, count, 0);
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
            block, field_name, class_name, line);
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
        default:
            /* Should not happen, we've set maximum indent in call to
             * parse_indented_file below. */
            ASSERT_FAIL();
    }
}


static error__t config_end_parse_line(
    unsigned int indent, void *indent_context)
{
    printf("config_end_parse_line %d %p\n", indent, indent_context);
    return ERROR_OK;
}


static const struct indent_parser config_indent_parser = {
    .start = config_start,
    .parse_line = config_parse_line,
    .end_parse_line = config_end_parse_line,
};


static error__t load_config_database(const char *db_name)
{
    log_message("Loading configuration database from \"%s\"", db_name);
    return
        parse_indented_file(db_name, 1, &config_indent_parser);
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
