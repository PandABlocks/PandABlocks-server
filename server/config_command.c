/* Interface for configuration commands. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "error.h"
#include "hashtable.h"
#include "parse.h"
#include "config_server.h"
#include "fields.h"
#include "attributes.h"

#include "config_command.h"




/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


/* This structure contains the implementations of the various entity operations,
 * depending on the exact structure of the entity request.  The fields in this
 * structure are a close reflection of the fields in config_command set. */
struct entity_actions {
    error__t (*get)(
        const struct entity_context *context,
        struct connection_result *result);
    error__t (*put)(const struct entity_context *context, const char *value);
    error__t (*put_table)(
        const struct entity_context *context,
        bool append, struct put_table_writer *writer);
};


/* Implements  block.*?  command, returns list of fields. */
static error__t do_field_list_get(
    const struct entity_context *context,
    struct connection_result *result)
{
    return field_list_get(context->block, result);
}


/* Implements  block.field?  command. */
static error__t do_field_get(
    const struct entity_context *context,
    struct connection_result *result)
{
    return field_get(context->field, context->number, result);
}


/* Implements  block.field=  command. */
static error__t do_field_put(
    const struct entity_context *context, const char *value)
{
    return field_put(context->field, context->number, value);
}


/* Implements  block.field<  command. */
static error__t do_field_put_table(
    const struct entity_context *context,
    bool append, struct put_table_writer *writer)
{
    return field_put_table(context->field, context->number, append, writer);
}


/* Implements  block.field.*?  command. */
static error__t do_attr_list_get(
    const struct entity_context *context,
    struct connection_result *result)
{
    return attr_list_get(context->field, result);
}


/* Implements  block.field.attr?  command. */
static error__t do_attr_get(
    const struct entity_context *context,
    struct connection_result *result)
{
    return attr_get(context->attr, context->number, result);
}


/* Implements  block.field.attr=  command. */
static error__t do_attr_put(
    const struct entity_context *context, const char *value)
{
    return attr_put(context->attr, context->number, value);
}


/* Implements  block.*  commands. */
static const struct entity_actions field_list_actions = {
    .get = do_field_list_get,           // block.*?
};

/* Implements  block.field  commands. */
static const struct entity_actions block_field_actions = {
    .get = do_field_get,                // block.field?
    .put = do_field_put,                // block.field=value
    .put_table = do_field_put_table,    // block.field<format
};

/* Implements  block.field.*  commands. */
static const struct entity_actions attr_list_actions = {
    .get = do_attr_list_get,               // block.field.*?
};

/* Implements  block.field.attr  commands. */
static const struct entity_actions field_attr_actions = {
    .get = do_attr_get,              // block.field.attr?
    .put = do_attr_put,              // block.field.attr=value
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Parsing and dispatch of entity set and get. */


/* Parses first part of entity target name:
 *
 *  block [number] "."
 *
 * and sets *number_present accordingly. */
static error__t parse_block_name(
    const char **input, struct entity_context *context,
    unsigned int *max_number, bool *number_present)
{
    char block_name[MAX_NAME_LENGTH];
    return
        /* Parse and look up the block name. */
        parse_name(input, block_name, sizeof(block_name))  ?:
        lookup_block(block_name, &context->block, max_number)  ?:

        /* Parse the number or flag its absence, and if present check that it's
         * in valid range. */
        IF_ELSE(isdigit(**input),
            parse_uint(input, &context->number)  ?:
            TEST_OK_(
                0 < context->number  &&  context->number <= *max_number,
                "Invalid block number") ?:
            DO( if (number_present) *number_present = true;
                context->number -= 1),
        //else
            DO( if (number_present) *number_present = false;
                context->number = 0));
}


/* Block number must be present or defaulted for normal field and attribute
 * commands. */
static error__t check_block_number(
    unsigned int max_number, bool *number_present)
{
    return
        IF(number_present,
            TEST_OK_(*number_present || max_number == 1,
                "Missing block number"));
}


/* This parses the  field  part of the entity name and does the final number
 * checking. */
static error__t parse_field_name(
    const char **input, struct entity_context *context)
{
    char field[MAX_NAME_LENGTH];
    return
        /* Process the field. */
        parse_name(input, field, MAX_NAME_LENGTH)  ?:
        lookup_field(context->block, field, &context->field);
}


/* Parses  attr  part of entity name if present and looks it up. */
static error__t parse_attr_name(
    const char **input, struct entity_context *context)
{
    char attr_name[MAX_NAME_LENGTH];
    return
        parse_name(input, attr_name, MAX_NAME_LENGTH)  ?:
        lookup_attr(context->field, attr_name, &context->attr);
}


/* Parses name into two or three sub-fields separated by dots, according to the
 * following syntax:
 *
 *  block [number] [ "." ( "*" | field [ "." ( "*" | meta ) ] ]
 *
 * The number is only optional if there is only one instance of the block. */
error__t parse_block_entity(
    const char **input, struct entity_context *parse,
    bool *number_present, bool *star_present)
{
    unsigned int max_number;

    *parse = (struct entity_context) {};
    if (star_present)
        *star_present = false;

    return
        /* Parse block name */
        parse_block_name(input, parse, &max_number, number_present)  ?:

        /* Check for field following. */
        IF(read_char(input, '.'),
            /* At this point we expect * or a field. */
            IF_ELSE(star_present  &&  read_char(input, '*'),
                /*  block.*  */
                DO(*star_present = true),
            //else
                /* Parse field name. */
                parse_field_name(input, parse)  ?:

                /* Now we have block.field, check for possible attribute. */
                IF_ELSE(read_char(input, '.'),
                    /* Again, check for * or name */
                    IF_ELSE(star_present  &&  read_char(input, '*'),
                        /*  block.field.*  */
                        DO(*star_present = true),
                    //else
                        /*  block.field.attr  */
                        check_block_number(max_number, number_present)  ?:
                        parse_attr_name(input, parse)),
                //else
                    /*  block.field  */
                    check_block_number(max_number, number_present))));
}


/* One of the following four commands is parsed and the *actions field is set
 * accordingly:
 *
 *  block.*                             field_list_actions
 *  block[number].field                 block_field_actions
 *  block.field.*                       attr_list_actions
 *  block[number].field.attr            field_attr_actions
 */
static error__t compute_entity_handler(
    const char *input, struct entity_context *context,
    const struct entity_actions **actions)
{
    bool number_present, star_present;
    return
        parse_block_entity(&input, context, &number_present, &star_present)  ?:
        parse_eos(&input)  ?:

        IF_ELSE(star_present,
            DO(
                if (context->field)
                    *actions = &attr_list_actions;      // block.field.*
                else
                    *actions = &field_list_actions;     // block.*
            ),
        //else
            TEST_OK_(context->field, "Missing field name")  ?:
            DO(
                if (context->attr)
                    *actions = &field_attr_actions;     // block.field.attr
                else
                    *actions = &block_field_actions;    // block.field
            ));
}


/* Process  entity?  commands. */
static error__t process_entity_get(
    const char *name, struct connection_result *result)
{
    struct entity_context context;
    const struct entity_actions *actions;
    return
        compute_entity_handler(name, &context, &actions)  ?:
        TEST_OK_(actions->get, "Field not readable")  ?:
        actions->get(&context, result);
}


/* Process  entity=value  commands. */
static error__t process_entity_put(
    struct connection_context *connection, const char *name, const char *value)
{
    struct entity_context context;
    const struct entity_actions *actions;
    return
        compute_entity_handler(name, &context, &actions)  ?:
        TEST_OK_(actions->put, "Field not writeable")  ?:
        actions->put(&context, value);
}


/* Process  entity<format  commands. */
static error__t process_entity_put_table(
    const char *name, bool append, struct put_table_writer *writer)
{
    struct entity_context context;
    const struct entity_actions *actions;
    return
        compute_entity_handler(name, &context, &actions)  ?:
        TEST_OK_(actions->put_table, "Field not a table")  ?:
        actions->put_table(&context, append, writer);
}


const struct config_command_set entity_commands = {
    .get = process_entity_get,
    .put = process_entity_put,
    .put_table = process_entity_put_table,
};
