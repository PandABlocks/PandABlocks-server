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
#include "fields.h"
#include "config_server.h"

#include "config_command.h"




/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


/* This structure is used to hold the context require to complete an entity
 * operation. */
struct entity_context {
    /* Fields derived from parsing the entity target. */
    struct block *block;                    // Block database entry
    unsigned int number;                    // Block number, within valid range
    struct field *field;                    // Field database entry
    const struct attr *attr;                // Attribute data, may be absent

    struct config_connection *connection;   // Connection from request
};


/* This structure contains the implementations of the various entity operations,
 * depending on the exact structure of the entity request.  The fields in this
 * structure are a close reflection of the fields in config_command set. */
struct entity_actions {
    error__t (*get)(
        const struct entity_context *context,
        const struct connection_result *result);
    error__t (*put)(const struct entity_context *context, const char *value);
    error__t (*put_table)(
        const struct entity_context *context,
        bool append, struct put_table_writer *writer);
};


/* Implements  block.*?  command, returns list of fields. */
static error__t do_field_list_get(
    const struct entity_context *context,
    const struct connection_result *result)
{
    return field_list_get(context->block, context->connection, result);
}


static struct field_context create_field_context(
    const struct entity_context *context)
{
    return (struct field_context) {
        .field = context->field,
        .number = context->number,
        .connection = context->connection,
    };
}


/* Implements  block.field?  command. */
static error__t do_field_get(
    const struct entity_context *context,
    const struct connection_result *result)
{
    struct field_context field_context = create_field_context(context);
    return field_get(&field_context, result);
}


/* Implements  block.field=  command. */
static error__t do_field_put(
    const struct entity_context *context, const char *value)
{
    struct field_context field_context = create_field_context(context);
    return field_put(&field_context, value);
}


/* Implements  block.field<  command. */
static error__t do_field_put_table(
    const struct entity_context *context,
    bool append, struct put_table_writer *writer)
{
    struct field_context field_context = create_field_context(context);
    return field_put_table(&field_context, append, writer);
}


/* Implements  block.field.*?  command. */
static error__t do_attr_list_get(
    const struct entity_context *context,
    const struct connection_result *result)
{
    return attr_list_get(context->field, context->connection, result);
}


static struct attr_context create_attr_context(
    const struct entity_context *context)
{
    return (struct attr_context) {
        .field = context->field,
        .number = context->number,
        .connection = context->connection,
        .attr = context->attr,
    };
}


/* Implements  block.field.attr?  command. */
static error__t do_attr_get(
    const struct entity_context *context,
    const struct connection_result *result)
{
    struct attr_context attr_context = create_attr_context(context);
    return attr_get(&attr_context, result);
}


/* Implements  block.field.attr=  command. */
static error__t do_attr_put(
    const struct entity_context *context, const char *value)
{
    struct attr_context attr_context = create_attr_context(context);
    return attr_put(&attr_context, value);
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
            DO(*number_present = true)  ?:
            parse_uint(input, &context->number)  ?:
            TEST_OK_(context->number < *max_number, "Block number too large"),
        //else
            DO(*number_present = false; context->number = 0))  ?:

        /* Finally eat the trailing . */
        TEST_OK_(read_char(input, '.'), "Missing field name");
}


/* Block number must be present or defaulted for normal field and attribute
 * commands. */
static error__t check_block_number(
    unsigned int max_number, bool number_present)
{
    return TEST_OK_(number_present || max_number == 1, "Missing block number");
}


/* For meta-data queries we don't allow the number to be present. */
static error__t check_no_number(bool number_present)
{
    return TEST_OK_(!number_present, "Block number not allowed");
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
 *  block [number] "." ( "*" | field [ "." ( "*" | meta ) ]
 *
 * One of the following four commands is parsed and the *actions field is set
 * accordingly:
 *
 *  block.*                             field_list_actions
 *  block[number].field                 block_field_actions
 *  block.field.*                       attr_list_actions
 *  block[number].field.attr            field_attr_actions
 *
 * The number is only optional if there is only one instance of the block. */
static error__t compute_entity_handler(
    const char *input, struct entity_context *context,
    const struct entity_actions **actions)
{
    unsigned int max_number;
    bool number_present;
    return
        /* Consume the  block.  part of the input. */
        parse_block_name(&input, context, &max_number, &number_present)  ?:

        IF_ELSE(read_char(&input, '*'),
            /*  block.*  */
            check_no_number(number_present)  ?:
            DO(*actions = &field_list_actions),

        //else
            /* If not block meta-query then field name must follow. */
            parse_field_name(&input, context)  ?:
            IF_ELSE(read_char(&input, '.'),
                /* There's a further * or an attribute. */
                IF_ELSE(read_char(&input, '*'),
                    /*  block.field.*  */
                    check_no_number(number_present)  ?:
                    DO(*actions = &attr_list_actions),
                //else
                    /*  block.field.attr  */
                    check_block_number(max_number, number_present)  ?:
                    parse_attr_name(&input, context)  ?:
                    DO(*actions = &field_attr_actions)
                ),
            //else
                /*  block.field  */
                check_block_number(max_number, number_present)  ?:
                DO(*actions = &block_field_actions)
            )
        )  ?:

        /* Make sure there's nothing left over in the field. */
        parse_eos(&input);
}


/* Process  entity?  commands. */
static error__t process_entity_get(
    struct config_connection *connection, const char *name,
    const struct connection_result *result)
{
    struct entity_context context = { .connection = connection };
    const struct entity_actions *actions;
    return
        compute_entity_handler(name, &context, &actions)  ?:
        TEST_OK_(actions->get, "Field not readable")  ?:
        actions->get(&context, result);
}


/* Process  entity=value  commands. */
static error__t process_entity_put(
    struct config_connection *connection, const char *name, const char *value)
{
    struct entity_context context = { .connection = connection };
    const struct entity_actions *actions;
    return
        compute_entity_handler(name, &context, &actions)  ?:
        TEST_OK_(actions->put, "Field not writeable")  ?:
        actions->put(&context, value);
}


/* Process  entity<format  commands. */
static error__t process_entity_put_table(
    struct config_connection *connection, const char *name, bool append,
    struct put_table_writer *writer)
{
    struct entity_context context = { .connection = connection };
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
