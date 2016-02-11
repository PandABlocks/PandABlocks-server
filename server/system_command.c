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
#include "socket_server.h"
#include "config_server.h"
#include "data_server.h"
#include "config_command.h"
#include "prepare.h"
#include "fields.h"
#include "output.h"
#include "classes.h"
#include "attributes.h"
#include "enums.h"
#include "version.h"

#include "system_command.h"



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Individual system commands. */


/* *IDN?
 *
 * Returns simple system identification. */
static error__t get_idn(const char *command, struct connection_result *result)
{
    return write_one_result(result, "%s %s", server_name, server_version);
}


/* *BLOCKS?
 *
 * Returns formatted list of all the blocks in the system. */
static error__t get_blocks(
    const char *command, struct connection_result *result)
{
    return block_list_get(result);
}


/* *ECHO echo string?
 *
 * Echos echo string back to caller. */
static error__t get_echo(const char *command, struct connection_result *result)
{
    return
        parse_char(&command, ' ') ?:
        write_one_result(result, "%s", command);
}


/* *WHO?
 *
 * Returns list of connections. */
static error__t get_who(const char *command, struct connection_result *result)
{
    generate_connection_list(result);
    return ERROR_OK;
}


/* *CHANGES?
 * *CHANGES.CONFIG?
 * *CHANGES.BITS?
 * *CHANGES.POSN?
 * *CHANGES.READ?
 * *CHANGES.ATTR?
 * *CHANGES.TABLE?
 *
 * Returns list of changed fields and their value. */

static error__t lookup_change_set(
    const char *action, enum change_set *change_set)
{
    if      (strcmp(action, "CONFIG") == 0)   *change_set = CHANGES_CONFIG;
    else if (strcmp(action, "BITS"  ) == 0)   *change_set = CHANGES_BITS;
    else if (strcmp(action, "POSN"  ) == 0)   *change_set = CHANGES_POSITION;
    else if (strcmp(action, "READ"  ) == 0)   *change_set = CHANGES_READ;
    else if (strcmp(action, "ATTR"  ) == 0)   *change_set = CHANGES_ATTR;
    else if (strcmp(action, "TABLE" ) == 0)   *change_set = CHANGES_TABLE;
    else
        return FAIL_("Unknown changes selection");
    return ERROR_OK;
}

static error__t parse_change_set(
    const char **command, enum change_set *change_set)
{
    *change_set = CHANGES_ALL;
    char action[MAX_NAME_LENGTH];
    return
        IF(read_char(command, '.'),
            parse_name(command, action, sizeof(action))  ?:
            lookup_change_set(action, change_set))  ?:
        parse_eos(command);
}

static error__t get_changes(
    const char *command, struct connection_result *result)
{
    enum change_set change_set;
    return
        parse_change_set(&command, &change_set)  ?:
        DO(generate_change_sets(result, change_set));
}


/* *CHANGES=
 * *CHANGES.CONFIG=
 * *CHANGES.BITS=
 * *CHANGES.POSN=
 * *CHANGES.READ=
 * *CHANGES.ATTR=
 * *CHANGES.TABLE=
 *
 * Resets change reporting for selected change set. */
static error__t put_changes(
    struct connection_context *connection,
    const char *command, const char *value)
{
    enum change_set change_set;
    return
        parse_change_set(&command, &change_set)  ?:
        parse_eos(&value)  ?:
        DO(reset_change_set(connection->change_set_context, change_set));
}


/* *DESC.block?
 * *DESC.block.field?
 *
 * Returns description field for block or field. */
static error__t get_desc(const char *command, struct connection_result *result)
{
    const char *string = NULL;
    struct entity_context parse;
    return
        parse_char(&command, '.')  ?:
        parse_block_entity(&command, &parse, NULL, NULL)  ?:
        parse_eos(&command)  ?:

        IF_ELSE(parse.attr,
            TEST_OK_(string = get_attr_description(parse.attr),
                "No description for attribute"),
        //else
            IF_ELSE(parse.field,
                /* Field follows: *DESC.block.field? */
                TEST_OK_(string = get_field_description(parse.field),
                    "No description set for field"),
            //else
                /* Just a block: *DESC.block? */
                TEST_OK_(string = get_block_description(parse.block),
                    "No description set for block")))  ?:
        write_one_result(result, "%s", string);
}


/* *CAPTURE?
 * *CAPTURE.*?
 *
 * Returns list of captured fields in capture order or list of fields that can
 * be selected for capture. */
static error__t get_capture(
    const char *command, struct connection_result *result)
{
    return
        IF_ELSE(read_string(&command, ".*"),
            parse_eos(&command)  ?:
            DO(report_capture_labels(result)),
        //else
            parse_eos(&command)  ?:
            DO(report_capture_list(result)));
}

/* *CAPTURE=
 *
 * Resets capture to empty. */
static error__t put_capture(
    struct connection_context *connection,
    const char *command, const char *value)
{
    return
        parse_eos(&command)  ?:
        parse_eos(&value)  ?:
        DO(reset_capture_list());
}


/* *POSITIONS?
 *
 * Returns list of bit field names for each bit capture block. */
static error__t get_positions(
    const char *command, struct connection_result *result)
{
    report_capture_positions(result);
    return ERROR_OK;
}


/* *VERBOSE=0/1
 *
 * Enables or disables detailed command logging. */
static error__t put_verbose(
    struct connection_context *connection,
    const char *command, const char *value)
{
    unsigned int verbose;
    return
        parse_uint(&value, &verbose)  ?:
        parse_eos(&value)  ?:
        DO(set_config_server_verbosity(verbose));
}


/* *ENUMS.block.field?
 * *ENUMS.block.field.attr?
 *
 * Returns list of enumeration labels, if appropriate. */
static error__t get_enums(const char *command, struct connection_result *result)
{
    struct entity_context parse;
    const struct enumeration *enumeration;
    return
        parse_char(&command, '.')  ?:
        parse_block_entity(&command, &parse, NULL, NULL)  ?:
        parse_eos(&command)  ?:

        TEST_OK_(parse.field, "Missing field name")  ?:
        IF_ELSE(parse.attr,
            TEST_OK_(enumeration = get_attr_enumeration(parse.attr),
                "Attribute is not an enumeration"),
        //else
            TEST_OK_(enumeration = get_field_enumeration(parse.field),
                "Field is not an enumeration"))  ?:
        DO(write_enum_labels(enumeration, result));
}


/* *PCAP control methods.
 *
 * *PCAP.ARM=
 * *PCAP.DISARM=
 * *PCAP.RESET=
 * *PCAP.STATUS?
 * *PCAP.WAITING?
 *
 * Manages and interrogates capture interface. */

static error__t lookup_pcap_put_action(
    const char *name, error__t (**action)(void))
{
    return
        IF_ELSE(strcmp(name, "ARM") == 0,
            DO(*action = arm_capture),
        //else
        IF_ELSE(strcmp(name, "DISARM") == 0,
            DO(*action = disarm_capture),
        //else
        IF_ELSE(strcmp(name, "RESET") == 0,
            DO(*action = reset_capture),
        //else
            FAIL_("Invalid *PCAP field"))));
}

static error__t put_pcap(
    struct connection_context *connection,
    const char *command, const char *value)
{
    char action_name[MAX_NAME_LENGTH];
    error__t (*action)(void) = NULL;
    return
        parse_char(&command, '.')  ?:
        parse_name(&command, action_name, sizeof(action_name))  ?:
        parse_eos(&value)  ?:

        lookup_pcap_put_action(action_name, &action)  ?:
        action();
}


static error__t lookup_pcap_get_action(
    const char *name, error__t (**action)(struct connection_result *))
{
    return
        IF_ELSE(strcmp(name, "STATUS") == 0,
            DO(*action = capture_status),
        //else
            FAIL_("Invalid *PCAP field"));
}

static error__t get_pcap(const char *command, struct connection_result *result)
{
    char action_name[MAX_NAME_LENGTH];
    error__t (*action)(struct connection_result *) = NULL;
    return
        parse_char(&command, '.')  ?:
        parse_name(&command, action_name, sizeof(action_name))  ?:

        lookup_pcap_get_action(action_name, &action)  ?:
        action(result);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* System command dispatch. */


struct command_table_entry {
    const char *name;
    bool allow_arg;
    error__t (*get)(const char *command, struct connection_result *result);
    error__t (*put)(
        struct connection_context *connection,
        const char *command, const char *value);
};

static const struct command_table_entry command_table_list[] = {
    { "IDN",        .get = get_idn, },
    { "BLOCKS",     .get = get_blocks, },
    { "ECHO",       .get = get_echo, .allow_arg = true },
    { "WHO",        .get = get_who, },
    { "CHANGES",    .get = get_changes, .allow_arg = true, .put = put_changes },
    { "DESC",       .get = get_desc, .allow_arg = true },
    { "CAPTURE",    .get = get_capture, .allow_arg = true,
        .put = put_capture, },
    { "POSITIONS",  .get = get_positions, },
    { "VERBOSE",    .put = put_verbose, },
    { "ENUMS",      .get = get_enums, .allow_arg = true, },
    { "PCAP",       .get = get_pcap, .allow_arg = true, .put = put_pcap, },
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
    const char *command, struct connection_result *result)
{
    const struct command_table_entry *command_set;
    return
        parse_system_command(&command, &command_set)  ?:
        TEST_OK_(command_set->get, "Command not readable")  ?:
        command_set->get(command, result);
}


/* Process  *command=value  commands. */
static error__t process_system_put(
    struct connection_context *connection,
    const char *command, const char *value)
{
    const struct command_table_entry *command_set;
    return
        parse_system_command(&command, &command_set)  ?:
        TEST_OK_(command_set->put, "Command not writeable")  ?:
        command_set->put(connection, command, value);
}


static error__t process_system_put_table(
    const char *command, bool append, struct put_table_writer *writer)
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
