/* System commands.
 *
 * These all start with * and are generally for global configuration and status
 * interrogation. */

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
#include "socket_server.h"
#include "config_command.h"

#include "system_command.h"



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
    return block_list_get(connection, result);
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


/* *WHO?
 *
 * Returns list of connections. */

static error__t system_get_who(
    struct config_connection *connection, const char *command,
    const struct connection_result *result)
{
    generate_connection_list(connection, result);
    return ERROR_OK;
}


/* *CHANGES?
 * *CHANGES.CONFIG?
 * *CHANGES.BITS?
 * *CHANGES.POSN?
 * *CHANGES.READ?
 *
 * Returns list of changed fields and their value. */

static error__t lookup_change_set(
    const char *action, enum change_set *change_set)
{
    if      (strcmp(action, "CONFIG") == 0)   *change_set = CHANGES_CONFIG;
    else if (strcmp(action, "BITS"  ) == 0)   *change_set = CHANGES_BITS;
    else if (strcmp(action, "POSN"  ) == 0)   *change_set = CHANGES_POSITION;
    else if (strcmp(action, "READ"  ) == 0)   *change_set = CHANGES_READ;
    else
        return FAIL_("Unknown changes selection");
    return ERROR_OK;
}

static error__t system_get_changes(
    struct config_connection *connection, const char *command,
    const struct connection_result *result)
{
    enum change_set change_set = CHANGES_ALL;
    char action[MAX_NAME_LENGTH];
    return
        IF(read_char(&command, '.'),
            parse_name(&command, action, sizeof(action))  ?:
            lookup_change_set(action, &change_set))  ?:
        parse_eos(&command)  ?:
        DO(generate_change_sets(connection, result, change_set));
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
    { "WHO",        .get = system_get_who, },
    { "CHANGES",    .get = system_get_changes, .allow_arg = true },
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
    struct put_table_writer *writer)
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


void terminate_system_command(void)
{
    if (command_table)
        hash_table_destroy(command_table);
}
