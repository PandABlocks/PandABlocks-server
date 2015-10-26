/* System commands.
 *
 * These all start with * and are generally for global configuration and status
 * interrogation. */

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
#include "system_command.h"



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Individual system commands. */


struct multiline_state {
    config_command_get_more_t *get_more;
    int index;
};


/* *IDN?
 *
 * Returns simple system identification. */

static command_error_t system_get_idn(
    struct config_connection *connection, const char *name,
    char result[], void **multiline)
{
    strcpy(result, "PandA");
    return ERROR_OK;
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

static command_error_t system_get_blocks(
    struct config_connection *connection, const char *name,
    char result[], void **multiline)
{
    struct multiline_state *state = malloc(sizeof(struct multiline_state));
    *state = (struct multiline_state) {
        .get_more = system_get_more_blocks,
        .index = 0, };

    /* This will only fail if we failed to load any blocks, and this is already
     * checked during startup. */
    ASSERT_OK(system_get_more_blocks(state, result));
    *multiline = state;
    return ERROR_OK;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* System command dispatch. */


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



/* Process  *command?  commands. */
static command_error_t process_system_get(
    struct config_connection *connection, const char *name,
    char result[], void **multiline)
{
    const struct config_command_set *commands =
        hash_table_lookup(command_table, name);
    return
        TEST_OK_(commands  &&  commands->get, "Unknown value")  ?:
        commands->get(connection, name, result, multiline);
}


/* Process  *command=value  commands. */
static command_error_t process_system_put(
    struct config_connection *connection, const char *name, const char *value)
{
    const struct config_command_set *commands =
        hash_table_lookup(command_table, name);
    return
        TEST_OK_(commands  &&  commands->put, "Unknown target")  ?:
        commands->put(connection, name, value);
}


static command_error_t process_system_put_table(
    struct config_connection *connection,
    const char *name, const char *format, error__t *comms_error)
{
    return FAIL_("Not a table");
}


/* Processes multi-line  *command?  responses. */
static bool process_system_get_more(void *multiline, char result[])
{
    struct multiline_state *state = multiline;
    return state->get_more(multiline, result);
}


/* Command interface from server. */
const struct config_command_set system_commands = {
    .get = process_system_get,
    .put = process_system_put,
    .put_table = process_system_put_table,
    .get_more = process_system_get_more,
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

error__t initialise_system_command(void)
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
