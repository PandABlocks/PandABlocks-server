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


#define TEST_FPRINTF(stream, format...) \
    TEST_OK(fprintf(stream, format) > 0)


static bool report_success(FILE *stream)
{
    return TEST_FPRINTF(stream, "OK\n");
}


static bool report_value(FILE *stream, const char *value)
{
    return TEST_FPRINTF(stream, "OK =%s\n", value);
}


static bool report_error(FILE *stream, command_error_t error)
{
    return TEST_FPRINTF(stream, "ERR %s\n", error);
}


/* Returns multiline response. */
static bool process_multiline(
    FILE *stream, void *multiline,
    const struct config_command_set *command_set,
    char result[], size_t result_length)
{
    bool ok = true;
    do
        ok = TEST_FPRINTF(stream, "!%s\n", result);
    while (ok  &&  command_set->get_more(multiline, result, result_length));
    ok = ok  &&
        TEST_FPRINTF(stream, ".\n");
    return ok;
}


/* Processes command of the form [*]name? */
static bool process_read_command(
    FILE *stream, char *command, const struct config_command_set *command_set)
{
    char *action = strchr(command, '?');
    *action++ = '\0';
    if (*action == '\0')
    {
        char result[MAX_LINE_LENGTH];
        void *multiline = NULL;
        command_error_t error =
            command_set->get(command, result, sizeof(result), &multiline);
        if (error)
            return report_error(stream, error);
        else if (multiline)
            return process_multiline(
                stream, multiline, command_set, result, sizeof(result));
        else
            return report_value(stream, result);
    }
    else
        return report_error(stream, "Unexpected text after command");
}


/* Processes command of the form [*]name=value */
static bool process_write_command(
    FILE *stream, char *command, const struct config_command_set *command_set)
{
    char *action = strchr(command, '=');
    if (action)
    {
        *action++ = '\0';
        command_error_t error;
        return
            command_set->put(command, action, stream, &error)  &&
            IF_ELSE(error,
                report_error(stream, error),
            // else
                report_success(stream));
    }
    else
        return report_error(stream, "Malformed command");
}


/* Processes a general configuration command.  Error reporting to the user is
 * very simple: a fixed error message is returned if the command fails,
 * otherwise the command is responsible for reporting success and any other
 * output. */
static bool process_config_command(FILE *stream, char *command)
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
        return process_read_command(stream, command, command_set);
    else if (strchr(command, '='))
        return process_write_command(stream, command, command_set);
    else
        return report_error(stream, "Unknown command");
}


/* This is run as the thread to process a configuration client connection. */
void process_config_socket(int sock)
{
    FILE *stream;
    bool ok = TEST_IO(stream = fdopen(sock, "r+"));
    if (ok)
    {
        char line[MAX_LINE_LENGTH];
        while (ok  &&  fgets(line, sizeof(line), stream))
        {
            size_t len = strlen(line);
            ok =
                TEST_OK_(line[len - 1] == '\n', "Unterminated line")  &&
                DO(line[len - 1] = '\0')  &&
                process_config_command(stream, line);
        }
        fclose(stream);
    }
    else
        close(sock);

    log_message("Connection terminated");
}
