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


/* *IDN?
 *
 * Returns simple system identification. */

static command_error_t system_get_idn(
    struct config_connection *connection, const char *name,
    struct connection_result *result)
{
    result->write_one(connection, "PandA");
    return ERROR_OK;
}


/* *BLOCKS?
 *
 * Returns formatted list of all the blocks in the system. */

static command_error_t system_get_blocks(
    struct config_connection *connection, const char *name,
    struct connection_result *result)
{
    const struct config_block *block;
    const char *block_name;
    unsigned int count;
    int ix = 0;
    while (walk_blocks_list(&ix, &block, &block_name, &count))
    {
        char value[MAX_VALUE_LENGTH];
        snprintf(value, MAX_VALUE_LENGTH, "%s %d", block_name, count);
        result->write_many(connection, value, false);
    }
    result->write_many(connection, NULL, true);
    return ERROR_OK;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* System command dispatch. */


struct command_table_entry {
    const char *name;
    struct config_command_set command;
};

static const struct command_table_entry command_table_list[] = {
    { "IDN",        { .get = system_get_idn } },
    { "BLOCKS",     { .get = system_get_blocks, } },
};

static struct hash_table *command_table;



/* Process  *command?  commands. */
static command_error_t process_system_get(
    struct config_connection *connection, const char *name,
    struct connection_result *result)
{
    const struct config_command_set *commands =
        hash_table_lookup(command_table, name);
    return
        TEST_OK_(commands  &&  commands->get, "Unknown value")  ?:
        commands->get(connection, name, result);
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
    struct config_connection *connection, const char *name,
    const unsigned int data[], size_t length, bool append)
{
    return FAIL_("Not a table");
}


/* Command interface from server. */
const struct config_command_set system_commands = {
    .get = process_system_get,
    .put = process_system_put,
    .put_table = process_system_put_table,
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
