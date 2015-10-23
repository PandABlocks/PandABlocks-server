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

#include "config_server.h"


/* This should be long enough for any reasonable command. */
#define MAX_LINE_LENGTH     1024


#define TEST_FPRINTF(connection, format...) \
    TEST_OK(fprintf(connection->stream, format) > 0)


static error__t report_success(struct config_connection *connection)
{
    return TEST_FPRINTF(connection, "OK\n");
}


static error__t report_value(
    struct config_connection *connection, const char *value)
{
    return TEST_FPRINTF(connection, "OK =%s\n", value);
}


static error__t report_command_error(
    struct config_connection *connection, command_error_t command_error)
{
    error__t error =
        TEST_FPRINTF(connection, "ERR %s\n", error_format(command_error));
    error_discard(command_error);
    return error;
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
static error__t process_read_command(
    struct config_connection *connection, char *command,
    const struct config_command_set *command_set)
{
    char *action = strchr(command, '?');    // Already tested, WILL succeed
    *action++ = '\0';
    if (*action == '\0')
    {
        char result[MAX_VALUE_LENGTH];
        command_error_t command_error = ERROR_OK;
        void *multiline = NULL;
        command_set->get(
            connection, command, result, &command_error, &multiline);
        if (command_error)
            return report_command_error(connection, command_error);
        else if (multiline)
            return process_multiline(
                connection, multiline, command_set, result);
        else
            return report_value(connection, result);
    }
    else
        return report_command_error(
            connection, FAIL_("Unexpected text after command"));
}


/* Processes command of the form [*]name=value */
static error__t process_write_command(
    struct config_connection *connection, char *command,
    const struct config_command_set *command_set)
{
    char *action = strchr(command, '=');    // Already tested, WILL succeed
    *action++ = '\0';
    command_error_t command_error = ERROR_OK;
    return
        command_set->put(connection, command, action, &command_error)  ?:
        IF_ELSE(command_error,
            report_command_error(connection, command_error),
        // else
            report_success(connection));
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

    /* Now choose between read and write command. */
    if (strchr(command, '?'))
        return process_read_command(connection, command, command_set);
    else if (strchr(command, '='))
        return process_write_command(connection, command, command_set);
    else
        return report_command_error(connection, FAIL_("Unknown command"));
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
