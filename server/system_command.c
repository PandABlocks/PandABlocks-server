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
#include <limits.h>

#include "error.h"
#include "hashtable.h"
#include "parse.h"
#include "hardware.h"
#include "socket_server.h"
#include "config_server.h"
#include "data_server.h"
#include "config_command.h"
#include "prepare.h"
#include "attributes.h"
#include "fields.h"
#include "pos_mux.h"
#include "pos_out.h"
#include "bit_out.h"
#include "output.h"
#include "enums.h"
#include "version.h"
#include "metadata.h"
#include "table.h"
#include "persistence.h"

#include "system_command.h"


static const char *rootfs_version;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Individual system commands. */


/* *IDN?
 *
 * Returns simple system identification. */
static error__t get_idn(const char *command, struct connection_result *result)
{
    uint32_t fpga_version, fpga_build, user_version;
    hw_read_versions(&fpga_version, &fpga_build, &user_version);
    /* Convert fpga version into a string.  The format is four bytes:
     *      customer.major.minor.point
     * where the customer version is only printed if non-zero, and is printed
     * last. */
    char fpga_string[MAX_NAME_LENGTH];
    int len = sprintf(fpga_string, "%u.%u.%u",
        (fpga_version >> 16) & 0xFF, (fpga_version >> 8) & 0xFF,
        fpga_version & 0xFF);
    if (fpga_version >> 24)
        sprintf(fpga_string + len, "C%u", fpga_version >> 24);
    return format_one_result(result, "%s SW: %s FPGA: %s %08x %08x rootfs: %s",
        server_name, server_version, fpga_string, fpga_build, user_version,
        rootfs_version);
}


/* *METADATA.key?
 * *METADATA.*?
 * *METADATA.key=name
 * *METADATA.key<
 *
 * Arbitrary string associated with configuration. */
static error__t get_metadata(
    const char *command, struct connection_result *result)
{
    char key[MAX_NAME_LENGTH];
    return
        parse_char(&command, '.')  ?:
        IF_ELSE(read_char(&command, '*'),
            parse_eos(&command)  ?:
            get_metadata_keys(result),
        //else
            parse_alphanum_name(&command, key, sizeof(key))  ?:
            parse_eos(&command)  ?:
            get_metadata_value(key, result));
}


static error__t put_metadata(
    struct connection_context *connection,
    const char *command, const char *value)
{
    char key[MAX_NAME_LENGTH];
    const char *string;
    return
        parse_char(&command, '.')  ?:
        parse_alphanum_name(&command, key, sizeof(key))  ?:
        parse_eos(&command)  ?:
        parse_utf8_string(&value, &string)  ?:
        put_metadata_value(key, string);
}


static error__t put_table_metadata(
    const char *command, struct put_table_writer *writer)
{
    char key[MAX_NAME_LENGTH];
    return
        parse_char(&command, '.')  ?:
        parse_alphanum_name(&command, key, sizeof(key))  ?:
        parse_eos(&command)  ?:
        put_metadata_table(key, writer);
}


/* *BLOCKS?
 *
 * Returns formatted list of all the blocks in the system. */
static error__t get_blocks(
    const char *command, struct connection_result *result)
{
    result->response = RESPONSE_MANY;
    return block_list_get(result);
}


/* *ECHO echo string?
 *
 * Echos echo string back to caller. */
static error__t get_echo(const char *command, struct connection_result *result)
{
    return
        parse_char(&command, ' ') ?:
        format_one_result(result, "%s", command);
}


/* *WHO?
 *
 * Returns list of connections. */
static error__t get_who(const char *command, struct connection_result *result)
{
    result->response = RESPONSE_MANY;
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
 * *CHANGES.METADATA?
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
    else if (strcmp(action, "METADATA" ) == 0) *change_set = CHANGES_METADATA;
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
    result->response = RESPONSE_MANY;
    return
        parse_change_set(&command, &change_set)  ?:
        DO(generate_change_sets(result, change_set, false));
}


/* *CHANGES=[S|E]
 * *CHANGES.CONFIG=[S|E]
 * *CHANGES.BITS=[S|E]
 * *CHANGES.POSN=[S|E]
 * *CHANGES.READ=[S|E]
 * *CHANGES.ATTR=[S|E]
 * *CHANGES.TABLE=[S|E]
 * *CHANGES.METADATA=[S|E]
 *
 * Resets change reporting for selected change set. */

static error__t parse_change_set_reset(
    const char **string, enum reset_change_set_action *action)
{
    *action = RESET_END;    // Default is reset to end
    if (read_char(string, 'E'))
        return ERROR_OK;
    else if (read_char(string, 'S'))
    {
        *action = RESET_START;
        return ERROR_OK;
    }
    else
        return TEST_OK_(**string == '\0', "Invalid reset option");
}

static error__t put_changes(
    struct connection_context *connection,
    const char *command, const char *value)
{
    enum change_set change_set;
    enum reset_change_set_action action;
    return
        parse_change_set(&command, &change_set)  ?:
        parse_change_set_reset(&value, &action)  ?:
        parse_eos(&value)  ?:
        DO(reset_change_set(
            connection->change_set_context, change_set, action));
}


/* *DESC.block?
 * *DESC.block.field?
 * *DESC.block.field[].subfield?
 *
 * Returns description field for block or field. */
static error__t get_desc(const char *command, struct connection_result *result)
{
    const char *string = NULL;
    struct entity_context parse;
    struct table_subfield *subfield = NULL;
    return
        parse_char(&command, '.')  ?:
        parse_block_entity(&command, &parse, NULL, NULL)  ?:
        parse_table_subfield(&command, &parse, &subfield)  ?:
        parse_eos(&command)  ?:

        IF_ELSE(parse.attr,
            TEST_OK_(string = get_attr_description(parse.attr),
                "No description for attribute"),
        //else
        IF_ELSE(subfield,
            /* Table subfield: *DESC.block.field[].subfield? */
            TEST_OK_(string = get_table_subfield_description(subfield),
                "No description for table sub-field"),
        //else
        IF_ELSE(parse.field,
            /* Field follows: *DESC.block.field? */
            TEST_OK_(string = get_field_description(parse.field),
                "No description set for field"),
        //else
            /* Just a block: *DESC.block? */
            TEST_OK_(string = get_block_description(parse.block),
                "No description set for block"))))  ?:
        format_one_result(result, "%s", string);
}


/* *CAPTURE?
 * *CAPTURE.*?
 * *CAPTURE.OPTIONS?
 * *CAPTURE.ENUMS?
 *
 * Returns list of captured fields in capture order or list of fields that can
 * be selected for capture. */
static error__t get_capture(
    const char *command, struct connection_result *result)
{
    char name[MAX_NAME_LENGTH];
    result->response = RESPONSE_MANY;
    return
        IF_ELSE(read_char(&command, '.'),
            /* Either .* or .name */
            IF_ELSE(read_char(&command, '*'),
                parse_eos(&command)  ?:
                DO(report_capture_labels(result)),
            // else
                parse_name(&command, name, sizeof(name))  ?:
                parse_eos(&command)  ?:
                IF_ELSE(strcmp(name, "OPTIONS") == 0,
                    get_capture_options(result),
                IF_ELSE(strcmp(name, "ENUMS") == 0,
                    get_capture_enums(result),
                //else
                    FAIL_("Invalid *CAPTURE option")))),
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
 * Returns list of names for each position bus entry. */
static error__t get_positions(
    const char *command, struct connection_result *result)
{
    result->response = RESPONSE_MANY;
    report_capture_positions(result);
    return ERROR_OK;
}


/* *BITS?
 *
 * Returns list of bit field names. */
static error__t get_bits(
    const char *command, struct connection_result *result)
{
    result->response = RESPONSE_MANY;
    report_capture_bits(result);
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
 * *ENUMS.block.field[].subfield?
 *
 * Returns list of enumeration labels, if appropriate. */
static error__t get_enums(const char *command, struct connection_result *result)
{
    struct entity_context parse;
    const struct enumeration *enumeration;
    struct table_subfield *subfield = NULL;
    result->response = RESPONSE_MANY;
    return
        parse_char(&command, '.')  ?:
        parse_block_entity(&command, &parse, NULL, NULL)  ?:
        parse_table_subfield(&command, &parse, &subfield)  ?:
        parse_eos(&command)  ?:

        TEST_OK_(parse.field, "Missing field name")  ?:
        IF_ELSE(parse.attr,
            TEST_OK_(enumeration = get_attr_enumeration(parse.attr),
                "Attribute is not an enumeration"),
        IF_ELSE(subfield,
            TEST_OK_(enumeration = get_table_subfield_enumeration(subfield),
                "Table sub-field is not an enumeration"),
        //else
            TEST_OK_(enumeration = get_field_enumeration(parse.field),
                "Field is not an enumeration")))  ?:
        DO(write_enum_labels(enumeration, result));
}


/* *PCAP control methods.
 *
 * *PCAP.ARM=
 * *PCAP.DISARM=
 * *PCAP.STATUS?
 * *PCAP.CAPTURED?
 * *PCAP.COMPLETION?
 *
 * Manages and interrogates capture interface. */

static error__t lookup_pcap_put_action(const char *name)
{
    return
        IF_ELSE(strcmp(name, "ARM") == 0,
            arm_capture(),
        IF_ELSE(strcmp(name, "DISARM") == 0,
            disarm_capture(),
        //else
            FAIL_("Invalid *PCAP field")));
}

static error__t put_pcap(
    struct connection_context *connection,
    const char *command, const char *value)
{
    char action_name[MAX_NAME_LENGTH];
    return
        parse_char(&command, '.')  ?:
        parse_name(&command, action_name, sizeof(action_name))  ?:
        parse_eos(&value)  ?:

        lookup_pcap_put_action(action_name);
}


static error__t lookup_pcap_get_action(
    const char *name, struct connection_result *result)
{
    return
        IF_ELSE(strcmp(name, "STATUS") == 0,
            get_capture_status(result),
        IF_ELSE(strcmp(name, "CAPTURED") == 0,
            get_capture_count(result),
        IF_ELSE(strcmp(name, "COMPLETION") == 0,
            get_capture_completion(result),
        //else
            FAIL_("Invalid *PCAP field"))));
}

static error__t get_pcap(const char *command, struct connection_result *result)
{
    char action_name[MAX_NAME_LENGTH];
    return
        parse_char(&command, '.')  ?:
        parse_name(&command, action_name, sizeof(action_name))  ?:

        lookup_pcap_get_action(action_name, result);
}


/* *SAVESTATE=
 *
 * Processing this command forces current persistence state to be written
 * immediately before returning. */
static error__t put_savestate(
    struct connection_context *connection,
    const char *command, const char *value)
{
    return
        parse_eos(&value)  ?:
        save_persistent_state();
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
    error__t (*put_table)(
        const char *command, struct put_table_writer *writer);
};

static const struct command_table_entry command_table_list[] = {
    { "IDN",        false, .get = get_idn, },
    { "METADATA",   true,  .get = get_metadata, .put = put_metadata,
                           .put_table = put_table_metadata, },
    { "BLOCKS",     false, .get = get_blocks, },
    { "ECHO",       true,  .get = get_echo, },
    { "WHO",        false, .get = get_who, },
    { "CHANGES",    true,  .get = get_changes,  .put = put_changes },
    { "DESC",       true,  .get = get_desc, },
    { "CAPTURE",    true,  .get = get_capture,  .put = put_capture, },
    { "POSITIONS",  false, .get = get_positions, },
    { "BITS",       false, .get = get_bits, },
    { "VERBOSE",    false, .put = put_verbose, },
    { "ENUMS",      true,  .get = get_enums, },
    { "PCAP",       true,  .get = get_pcap,     .put = put_pcap, },
    { "SAVESTATE",  false, .put = put_savestate, },
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
    const char *command, bool append, bool binary,
    struct put_table_writer *writer)
{
    const struct command_table_entry *command_set;
    return
        parse_system_command(&command, &command_set)  ?:
        TEST_OK_(command_set->put_table, "Not a table")  ?:
        TEST_OK_(!append, "Append not supported")  ?:
        TEST_OK_(!binary, "Binary writes not supported")  ?:
        command_set->put_table(command, writer);
}


/* Command interface from server. */
const struct config_command_set system_commands = {
    .get = process_system_get,
    .put = process_system_put,
    .put_table = process_system_put_table,
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

error__t initialise_system_command(const char *_rootfs_version)
{
    rootfs_version = _rootfs_version;

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
