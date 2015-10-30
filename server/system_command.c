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
#include "parse.h"
#include "config_server.h"
#include "fields.h"
#include "config_command.h"

#include "system_command.h"


#define MAX_VALUE_LENGTH    64
#define MAX_NAME_LENGTH     20


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Individual system commands. */


/* *IDN?
 *
 * Returns simple system identification. */

static error__t system_get_idn(
    struct config_connection *connection, const char *command,
    const struct connection_result *result)
{
    result->write_one(connection, "PandA");
    return ERROR_OK;
}


/* *BLOCKS?
 *
 * Returns formatted list of all the blocks in the system. */

static error__t system_get_blocks(
    struct config_connection *connection, const char *command,
    const struct connection_result *result)
{
    const struct block *block;
    int ix = 0;
    while (walk_blocks_list(&ix, &block))
    {
        const char *block_name;
        unsigned int count;
        get_block_info(block, &block_name, &count);
        char value[MAX_VALUE_LENGTH];
        snprintf(value, sizeof(value), "%s %d", block_name, count);
        result->write_many(connection, value);
    }
    result->write_many_end(connection);
    return ERROR_OK;
}


/* *ECHO echo string?
 *
 * Echos echo string back to caller. */

static error__t system_get_echo(
    struct config_connection *connection, const char *command,
    const struct connection_result *result)
{
    return
        parse_char(&command, ' ') ?:
        DO(result->write_one(connection, command));
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* System command dispatch. */


struct command_table_entry {
    const char *name;
    bool allow_arg;
    error__t (*get)(
        struct config_connection *connection, const char *name,
        const struct connection_result *result);
    error__t (*put)(
        struct config_connection *connection,
        const char *name, const char *value);
};

static const struct command_table_entry command_table_list[] = {
    { "IDN",        .get = system_get_idn, },
    { "BLOCKS",     .get = system_get_blocks, },
    { "ECHO",       .get = system_get_echo, .allow_arg = true },
};

static struct hash_table *command_table;


static error__t parse_system_command(
    const char **command, const struct command_table_entry **command_set)
{
    char name[MAX_NAME_LENGTH];
    return
        parse_name(command, name, MAX_NAME_LENGTH)  ?:
        TEST_OK_(*command_set = hash_table_lookup(command_table, name),
            "Unknown command")  ?:
        IF(!(*command_set)->allow_arg,
            parse_eos(command));
}

/* Process  *command?  commands. */
static error__t process_system_get(
    struct config_connection *connection, const char *command,
    const struct connection_result *result)
{
    const struct command_table_entry *command_set;
    return
        parse_system_command(&command, &command_set)  ?:
        TEST_OK_(command_set->get, "Command not readable")  ?:
        command_set->get(connection, command, result);
}


/* Process  *command=value  commands. */
static error__t process_system_put(
    struct config_connection *connection,
    const char *command, const char *value)
{
    const struct command_table_entry *command_set;
    return
        parse_system_command(&command, &command_set)  ?:
        TEST_OK_(command_set->put, "Command not writeable")  ?:
        command_set->put(connection, command, value);
}


static error__t process_system_put_table(
    struct config_connection *connection, const char *command, bool append,
    const struct put_table_writer *writer)
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
        hash_table_insert_const(command_table, entry->name, entry);
    }
    return ERROR_OK;
}
