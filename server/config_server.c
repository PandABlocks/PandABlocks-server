/* Socket server for configuration interface. */

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "hardware.h"
#include "config_command.h"
#include "system_command.h"

#include "config_server.h"


/* This should be long enough for any reasonable command. */
#define MAX_LINE_LENGTH     1024


#define TEST_FPRINTF(connection, format...) \
    TEST_OK(fprintf(connection->stream, format) > 0)


static error__t report_value(
    struct config_connection *connection, const char *value)
{
    return TEST_FPRINTF(connection, "OK =%s\n", value);
}


static error__t report_status(
    struct config_connection *connection, command_error_t command_error)
{
    if (command_error)
    {
        error__t error =
            TEST_FPRINTF(connection, "ERR %s\n", error_format(command_error));
        error_discard(command_error);
        return error;
    }
    else
        return TEST_FPRINTF(connection, "OK\n");
}


/* Returns multiline response. */
static error__t process_multiline(
    struct config_connection *connection, void *multiline,
    const struct config_command_set *command_set, char result[])
{
    error__t error = ERROR_OK;
    do
        error = TEST_FPRINTF(connection, "!%s\n", result);
    while (!error  &&  command_set->get_more(multiline, result));
    error = error ?:
        TEST_FPRINTF(connection, ".\n");
    return error;
}


/* Processes command of the form [*]name? */
static error__t do_read_command(
    struct config_connection *connection, char *command, char *value,
    const struct config_command_set *command_set)
{
    if (*value == '\0')
    {
        char result[MAX_VALUE_LENGTH];
        void *multiline = NULL;
        command_error_t command_error = command_set->get(
            connection, command, result, &multiline);
        if (command_error)
            return report_status(connection, command_error);
        else if (multiline)
            return process_multiline(
                connection, multiline, command_set, result);
        else
            return report_value(connection, result);
    }
    else
        return report_status(
            connection, FAIL_("Unexpected text after command"));
}


/* Processes command of the form [*]name=value */
static error__t do_write_command(
    struct config_connection *connection, char *command, char *value,
    const struct config_command_set *command_set)
{
    return report_status(
        connection, command_set->put(connection, command, value));
}


/* Processes command of the form [*]name<format
 * This has the special condition that the input stream will be read for further
 * data, and so the put_table function can return one of two different error
 * codes: if a communication error is reported then we need to drop the
 * connection. */
static error__t do_table_command(
    struct config_connection *connection, char *command, char *format,
    const struct config_command_set *command_set)
{
    error__t comms_error = ERROR_OK;
    command_error_t command_error =
        command_set->put_table(connection, command, format, &comms_error);
    return
        comms_error  ?:
        report_status(connection, command_error);
}


/* Processes a general configuration command.  Error reporting to the user is
 * very simple: a fixed error message is returned if the command fails,
 * otherwise the command is responsible for reporting success and any other
 * output. */
static error__t process_config_command(
    struct config_connection *connection, char *command)
{
    /* * prefix switches between system and entity command sets. */
    const struct config_command_set *command_set;
    if (*command == '*')
    {
        command_set = &system_commands;
        command += 1;
    }
    else
        command_set = &entity_commands;

    /* The command is one of  name?, name=value, or name<format.  Split the
     * command into two parts at the separator. */
    size_t ix = strcspn(command, "?=<");
    char ch = command[ix];
    command[ix] = '\0';
    char *value = &command[ix + 1];
    switch (ch)
    {
        case '?':
            return do_read_command(connection, command, value, command_set);
        case '=':
            return do_write_command(connection, command, value, command_set);
        case '<':
            return do_table_command(connection, command, value, command_set);
        default:
            return report_status(connection, FAIL_("Unknown command"));
    }
}


/* This is run as the thread to process a configuration client connection. */
error__t process_config_socket(int sock)
{
    FILE *stream;
    error__t error = TEST_IO(stream = fdopen(sock, "r+"));
    if (error)
        close(sock);
    else
    {
        /* Create connection management structure here.  This will be passed
         * through to act as a connection context throughout the lifetime of
         * this connection. */
        struct config_connection connection = {
            .stream = stream,
        };

        char line[MAX_LINE_LENGTH];
        while (!error  &&  fgets(line, sizeof(line), stream))
        {
            size_t len = strlen(line);
            error =
                TEST_OK_(line[len - 1] == '\n', "Unterminated line")  ?:
                DO(line[len - 1] = '\0')  ?:
                process_config_command(&connection, line);
        }
        fclose(stream);
    }
    return error;
}
