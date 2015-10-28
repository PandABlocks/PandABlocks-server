/* Interface for configuration commands. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "error.h"
#include "hashtable.h"
#include "database.h"
#include "parse.h"

#include "config_command.h"


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


/* This structure is used to hold the context require to complete an entity
 * operation. */
struct entity_context {
    /* Fields derived from parsing the entity target. */
    const struct config_block *block;       // Block database entry
    unsigned int number;                    // Block number, within valid range
    const struct field_entry *field;        // Field database entry
    const struct field_attr *attr;          // Attribute data, may be absent

    struct config_connection *connection;   // Connection from request
    const struct entity_actions *actions;   // Actions for this context
};


/* This structure contains the implementations of the various entity operations,
 * depending on the exact structure of the entity request.  The fields in this
 * structure are a close reflection of the fields in config_command set. */
struct entity_actions {
    error__t (*get)(
        struct entity_context *context,
        struct connection_result *result);
    error__t (*put)(struct entity_context *context, const char *value);
    error__t (*put_table)(
        struct entity_context *context,
        const unsigned int data[], size_t length, bool append);
};


/* Implements  block.*?  command, returns list of fields. */
static error__t block_meta_get(
    struct entity_context *context, struct connection_result *result)
{
    int ix = 0;
    const struct field_entry *field;
    const char *field_name;
    while (walk_fields_list(context->block, &ix, &field, &field_name))
    {
        char value[MAX_VALUE_LENGTH];
        snprintf(value, MAX_VALUE_LENGTH, "%s", field_name);
        result->write_many(context->connection, value, false);
    }
    result->write_many(context->connection, NULL, true);
    return ERROR_OK;
}


/* Implements  block.field?  command. */
static error__t block_field_get(
    struct entity_context *context, struct connection_result *result)
{
    return FAIL_("block.field? not implemented yet");
}


/* Implements  block.field=  command. */
static error__t block_field_put(
    struct entity_context *context, const char *value)
{
    return FAIL_("block.field= not implemented yet");
}


/* Implements  block.field<  command. */
static error__t block_field_put_table(
    struct entity_context *context,
    const unsigned int data[], size_t length, bool append)
{
printf("block_field_put_table %p %zu %d\n", data, length, append);
    return FAIL_("block.field< not implemented yet");
}


/* Implements  block.field.*?  command. */
static error__t field_meta_get(
    struct entity_context *context, struct connection_result *result)
{
    return FAIL_("block.field.*? not implemented yet");
}


/* Implements  block.field.attr?  command. */
static error__t field_attr_get(
    struct entity_context *context, struct connection_result *result)
{
    return FAIL_("block.field.attr? not implemented yet");
}


/* Implements  block.field.attr=  command. */
static error__t field_attr_put(
    struct entity_context *context, const char *value)
{
    return FAIL_("block.field.attr= not implemented yet");
}


/* Implements  block.*  commands. */
static const struct entity_actions block_meta_actions = {
    .get = block_meta_get,          // block.*?
};

/* Implements  block.field  commands. */
static const struct entity_actions block_field_actions = {
    .get = block_field_get,         // block.field?
    .put = block_field_put,         // block.field=value
    .put_table = block_field_put_table, // block.field<format
};

/* Implements  block.field.*  commands. */
static const struct entity_actions field_meta_actions = {
    .get = field_meta_get,          // block.field.*?
};

/* Implements  block.field.attr  commands. */
static const struct entity_actions field_attr_actions = {
    .get = field_attr_get,          // block.field.attr?
    .put = field_attr_put,          // block.field.attr=value
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
    char block[MAX_NAME_LENGTH];
    return
        /* Parse and look up the block name. */
        parse_name(input, block, MAX_NAME_LENGTH)  ?:
        TEST_OK_(context->block = lookup_block(block, max_number),
            "No such block")  ?:

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
        TEST_OK_(context->field = lookup_field(context->block, field),
            "No such field");
}


/* Parses  attr  part of entity name if present and looks it up. */
static error__t parse_attr_name(
    const char **input, struct entity_context *context)
{
    char attr[MAX_NAME_LENGTH];
    return
        parse_name(input, attr, MAX_NAME_LENGTH)  ?:
        TEST_OK_(context->attr = lookup_attr(context->field, attr),
            "No such attribute");
}


/* Parses name into two or three sub-fields separated by dots, according to the
 * following syntax:
 *
 *  block [number] "." ( "*" | field [ "." ( "*" | meta ) ]
 *
 * One of the following four commands is parsed and the context->actions field
 * is set accordingly:
 *
 *  block.*                             block_meta_actions
 *  block[number].field                 block_field_actions
 *  block.field.*                       field_meta_actions
 *  block[number].field.attr            field_attr_actions
 *
 * The number is only optional if there is only one instance of the block. */
static error__t compute_entity_handler(
    const char *input, struct entity_context *context)
{
    unsigned int max_number;
    bool number_present;
    return
        /* Consume the  block.  part of the input. */
        parse_block_name(&input, context, &max_number, &number_present)  ?:

        IF_ELSE(read_char(&input, '*'),
            /*  block.*  */
            check_no_number(number_present)  ?:
            DO(context->actions = &block_meta_actions),

        //else
            /* If not block meta-query then field name must follow. */
            parse_field_name(&input, context)  ?:
            IF_ELSE(read_char(&input, '.'),
                /* There's a further * or an attribute. */
                IF_ELSE(read_char(&input, '*'),
                    /*  block.field.*  */
                    check_no_number(number_present)  ?:
                    DO(context->actions = &field_meta_actions),
                //else
                    /*  block.field.attr  */
                    parse_attr_name(&input, context)  ?:
                    check_block_number(max_number, number_present)  ?:
                    DO(context->actions = &field_attr_actions)
                ),
            //else
                /*  block.field  */
                check_block_number(max_number, number_present)  ?:
                DO(context->actions = &block_field_actions)
            )
        )  ?:

        /* Make sure there's nothing left over in the field. */
        parse_eos(&input);
}


/* Process  entity?  commands. */
static error__t process_entity_get(
    struct config_connection *connection, const char *name,
    struct connection_result *result)
{
    struct entity_context context = { .connection = connection };
    return
        compute_entity_handler(name, &context)  ?:
        TEST_OK_(context.actions->get, "Field not readable")  ?:
        context.actions->get(&context, result);
}


/* Process  entity=value  commands. */
static error__t process_entity_put(
    struct config_connection *connection, const char *name, const char *value)
{
    struct entity_context context = { .connection = connection };
    return
        compute_entity_handler(name, &context)  ?:
        TEST_OK_(context.actions->put, "Field not writeable")  ?:
        context.actions->put(&context, value);
}


/* Process  entity<format  commands. */
static error__t process_entity_put_table(
    struct config_connection *connection, const char *name,
    const unsigned int data[], size_t length, bool append)
{
    struct entity_context context = { .connection = connection };
    return
        compute_entity_handler(name, &context)  ?:
        TEST_OK_(context.actions->put_table, "Field not a table")  ?:
        context.actions->put_table(&context, data, length, append);
}


const struct config_command_set entity_commands = {
    .get = process_entity_get,
    .put = process_entity_put,
    .put_table = process_entity_put_table,
};
