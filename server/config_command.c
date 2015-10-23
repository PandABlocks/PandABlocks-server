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


struct multiline_state {
    config_command_get_more_t *get_more;
    int index;
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


/* This structure is used to hold the context require to complete an entity
 * operation. */
struct entity_context {
    /* Fields derived from parsing the entity target. */
    const struct config_block *block;       // Block database entry
    unsigned int number;                    // Block number, within valid range
    const struct field_entry *field;        // Field database entry
    const struct meta_field *meta;          // Meta data, may be absent

    struct config_connection *connection;   // Connection from request
    const struct entity_actions *actions;   // Actions for this context
};


/* This structure contains the implementations of the various entity operations,
 * depending on the exact structure of the entity request.  The fields in this
 * structure are a close reflection of the fields in config_command set. */
struct entity_actions {
    error__t (*put)(
        struct entity_context *context,
        const char *value, command_error_t *command_error);
    void (*get)(
        struct entity_context *context,
        char result[], command_error_t *command_error, void **multiline);
    bool (*get_more)(struct entity_context *context, char result[]);
};


static const struct entity_actions block_meta_actions = {
};

static const struct entity_actions field_actions = {
};

static const struct entity_actions field_meta_actions = {
};

static const struct entity_actions meta_actions = {
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


/* Parses name into two or three sub-fields separated by dots, according to the
 * following syntax:
 *
 *  block [count] "." ( "*" | field [ "." ( "*" | meta ) ]
 *
 * Each sub-field must be no longer than MAX_NAME_LENGTH characters long, and
 * missing sub-fields are set to an empty string. */
static command_error_t parse_entity_name(
    const char *name,
    char block[], unsigned int *number, bool *number_present,
    char field[], char meta[])
{
    *number_present = true;
    meta[0] = '\0';
    return
        /* There must be a block name. */
        parse_name(&name, block, MAX_NAME_LENGTH)  ?:
        /* The block is optionally followed by a number. */
        IF_ELSE(isdigit(*name),
            parse_uint(&name, number),
        // else
            DO(*number_present = false))  ?:
        /* The field name or * follows preceded by a dot. */
        parse_char(&name, '.')  ?:
        IF_ELSE(read_char(&name, '*'),
            DO(*field = '*'),
        //else
            parse_name(&name, field, MAX_NAME_LENGTH)  ?:
            /* There may be a meta-data field name following another dot. */
            IF(read_char(&name, '.'),
                IF_ELSE(read_char(&name, '*'),
                    DO(*meta = '*'),
                //else
                    parse_name(&name, meta, MAX_NAME_LENGTH)))  ?:
            /* Check that we've parsed everything. */
            parse_eos(&name));
}


static command_error_t compute_field_handler(
    bool number_present, unsigned int max_number, const char field[],
    struct entity_context *context)
{
    return
        IF_ELSE(number_present,
            /* If the number is present it must be within bounds. */
            TEST_OK_(context->number < max_number, "Block index too high"),
        // else
            TEST_OK_(max_number == 1, "Missing block number")  ?:
            DO(context->number = 0))  ?:
        /* Block and number valid, look up field. */
        TEST_OK_(context->field = lookup_field(context->block, field),
            "No such field");
}


static command_error_t compute_meta_handler(
    const char meta[], struct entity_context *context)
{
    return
        /* Nearly there.  Need to check for meta field. */
        IF_ELSE(*meta,
            /* No meta-field, return simple field actions. */
            DO(context->actions = &field_actions),

        //else
            /* Meta-data field is present, check for .* or field. */
            IF_ELSE(*meta == '*',
                /* Request for field meta data. */
DO(printf("field_meta_actions\n")) ?:
                DO(context->actions = &field_meta_actions),
            //else
                /* Lookup up meta-field. */
                TEST_OK_(
                    context->meta = lookup_meta(context->field, meta),
                    "Meta-field not found")  ?:
DO(printf("meta_actions\n")) ?:
                DO(context->actions = &field_meta_actions)));
}


static command_error_t compute_entity_handler(
    const char *name, struct entity_context *context)
{
    char block[MAX_NAME_LENGTH];
    char field[MAX_NAME_LENGTH] = {};
    char meta[MAX_NAME_LENGTH] = {};
    unsigned int max_number;
    bool number_present;

    return
        /* Split "block index.field.meta" into its components. */
        parse_entity_name(
            name, block, &context->number, &number_present, field, meta)  ?:
DO(printf("%s %u %d %s %s\n", 
block, context->number, number_present, field, meta))  ?:
        /* The block *must* be present. */
        TEST_OK_(context->block = lookup_block(block, &max_number),
            "No such block")  ?:

        /* The * field gets special treatment. */
        IF_ELSE(*field == '*',
            /* There can't be a number present for .* requests. */
            TEST_OK_(!number_present, "Malformed field list request")  ?:
DO(printf("block_meta_actions\n")) ?:
            DO(context->actions = &block_meta_actions),

        //else
            compute_field_handler(
                number_present, max_number, field, context)  ?:

            compute_meta_handler(meta, context));
}


static error__t process_entity_put(
    struct config_connection *connection,
    const char *name, const char *value, command_error_t *command_error)
{
    printf("process_entity_put %s %s\n", name, value);

    struct entity_context context;
    *command_error =
        compute_entity_handler(name, &context)  ?:
        TEST_OK_(context.actions->put, "Field not writeable");
    return IF(!*command_error,
        context.actions->put(&context, value, command_error));
}


static void process_entity_get(
    struct config_connection *connection, const char *name,
    char result[], command_error_t *command_error, void **multiline)
{
    printf("process_entity_get %s\n", name);

    struct entity_context context;
    *command_error =
        compute_entity_handler(name, &context)  ?:
        TEST_OK_(context.actions->get, "Field not readable")  ?:
        DO(context.actions->get(&context, result, command_error, multiline));
}


static bool process_entity_get_more(void *multiline, char result[])
{
    ASSERT_FAIL();
}


const struct config_command_set entity_commands = {
    .put = process_entity_put,
    .get = process_entity_get,
    .get_more = process_entity_get_more,
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* System commands. */


/* *IDN?
 *
 * Returns simple system identification. */

static void system_get_idn(
    struct config_connection *connection, const char *name,
    char result[], command_error_t *command_error, void **multiline)
{
    strcpy(result, "PandA");
}


/* *BLOCKS?
 *
 * Returns formatted list of all the blocks in the system. */

static bool system_get_more_blocks(void *multiline, char result[])
{
    struct multiline_state *state = multiline;

    const struct config_block *block;
    const char *name;
    unsigned int count;
    if (walk_blocks_list(&state->index, &block, &name, &count))
    {
        snprintf(result, MAX_VALUE_LENGTH, "%s %d", name, count);
        return true;
    }
    else
    {
        free(state);
        return false;
    }
}

static void system_get_blocks(
    struct config_connection *connection, const char *name,
    char result[], command_error_t *command_error, void **multiline)
{
    struct multiline_state *state = malloc(sizeof(struct multiline_state));
    *state = (struct multiline_state) {
        .get_more = system_get_more_blocks,
        .index = 0, };

    if (system_get_more_blocks(state, result))
        *multiline = state;
    else
        *command_error = FAIL_("No blocks found!");
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* System commands dispatch. */


struct command_table_entry {
    const char *name;
    struct config_command_set command;
};


static const struct command_table_entry command_table_list[] = {
    { "IDN", { .get = system_get_idn } },
    { "BLOCKS", {
        .get = system_get_blocks, .get_more = system_get_more_blocks } },
};

static struct hash_table *command_table;



static error__t process_system_put(
    struct config_connection *connection,
    const char *name, const char *value, command_error_t *command_error)
{
    printf("process_system_put %s %s\n", name, value);
    const struct config_command_set *commands =
        hash_table_lookup(command_table, name);
    if (commands  &&  commands->put)
        return commands->put(connection, name, value, command_error);
    else
    {
        *command_error = FAIL_("Unknown target");
        return ERROR_OK;
    }
}


static void process_system_get(
    struct config_connection *connection, const char *name,
    char result[], command_error_t *command_error, void **multiline)
{
    printf("process_system_get %s\n", name);
    const struct config_command_set *commands =
        hash_table_lookup(command_table, name);
    if (commands  &&  commands->get)
        return commands->get(
            connection, name, result, command_error, multiline);
    else
        *command_error = FAIL_("Unknown value");
}


static bool process_system_get_more(void *multiline, char result[])
{
    struct multiline_state *state = multiline;
    return state->get_more(multiline, result);
}


const struct config_command_set system_commands = {
    .put = process_system_put,
    .get = process_system_get,
    .get_more = process_system_get_more,
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

error__t initialise_config_command(void)
{
    /* Don't need to copy the keys, they're in the command table! */
    command_table = hash_table_create(false);
    for (unsigned int i = 0; i < ARRAY_SIZE(command_table_list); i ++)
    {
        const struct command_table_entry *entry = &command_table_list[i];
        hash_table_insert_const(command_table, entry->name, &entry->command);
    }
    return ERROR_OK;
}
