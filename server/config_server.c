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


static bool report_success(struct config_connection *connection)
{
    return TEST_FPRINTF(connection, "OK\n");
}


static bool report_value(
    struct config_connection *connection, const char *value)
{
    return TEST_FPRINTF(connection, "OK =%s\n", value);
}


static bool report_error(
    struct config_connection *connection, command_error_t error)
{
    return TEST_FPRINTF(connection, "ERR %s\n", error);
}


/* Returns multiline response. */
static bool process_multiline(
    struct config_connection *connection, void *multiline,
    const struct config_command_set *command_set, char result[])
{
    bool ok = true;
    do
        ok = TEST_FPRINTF(connection, "!%s\n", result);
    while (ok  &&  command_set->get_more(connection, multiline, result));
    ok = ok  &&
        TEST_FPRINTF(connection, ".\n");
    return ok;
}


/* Processes command of the form [*]name? */
static bool process_read_command(
    struct config_connection *connection, char *command,
    const struct config_command_set *command_set)
{
    char *action = strchr(command, '?');
    *action++ = '\0';
    if (*action == '\0')
    {
        char result[MAX_VALUE_LENGTH];
        void *multiline = NULL;
        command_error_t error;
        command_set->get(
            connection, command, result, &error, &multiline);
        if (error)
            return report_error(connection, error);
        else if (multiline)
            return process_multiline(
                connection, multiline, command_set, result);
        else
            return report_value(connection, result);
    }
    else
        return report_error(connection, "Unexpected text after command");
}


/* Processes command of the form [*]name=value */
static bool process_write_command(
    struct config_connection *connection, char *command,
    const struct config_command_set *command_set)
{
    char *action = strchr(command, '=');
    if (action)
    {
        *action++ = '\0';
        command_error_t error;
        return
            command_set->put(connection, command, action, &error)  &&
            IF_ELSE(error,
                report_error(connection, error),
            // else
                report_success(connection));
    }
    else
        return report_error(connection, "Malformed command");
}


/* Processes a general configuration command.  Error reporting to the user is
 * very simple: a fixed error message is returned if the command fails,
 * otherwise the command is responsible for reporting success and any other
 * output. */
static bool process_config_command(
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
        return report_error(connection, "Unknown command");
}


/* This is run as the thread to process a configuration client connection. */
void process_config_socket(int sock)
{
    FILE *stream;
    bool ok = TEST_IO(stream = fdopen(sock, "r+"));
    if (ok)
    {
        /* Create connection management structure here.  This will be passed
         * through to act as a connection context throughout the lifetime of
         * this connection. */
        struct config_connection connection = {
            .stream = stream,
        };

        char line[MAX_LINE_LENGTH];
        while (ok  &&  fgets(line, sizeof(line), stream))
        {
            size_t len = strlen(line);
            ok =
                TEST_OK_(line[len - 1] == '\n', "Unterminated line")  &&
                DO(line[len - 1] = '\0')  &&
                process_config_command(&connection, line);
        }
        fclose(stream);
    }
    else
        close(sock);

    log_message("Connection terminated");
}
