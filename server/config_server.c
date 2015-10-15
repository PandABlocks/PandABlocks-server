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

#include "config_server.h"


/* This should be long enough for any reasonable command. */
#define MAX_LINE_LENGTH     1024


/* Processes a general configuration command.  Error reporting to the user is
 * very simple: a fixed error message is returned if the command fails,
 * otherwise the command is responsible for reporting success and any other
 * output. */
static const char *process_config_command(FILE *stream, const char *command)
{
    return "Unknown command";
}


void process_config_socket(int sock)
{
    log_message("Process config connection on %d", sock);

    FILE *stream;
    if (TEST_IO(stream = fdopen(sock, "r+")))
    {
        char line[MAX_LINE_LENGTH];
        while (fgets(line, sizeof(line), stream))
        {
            size_t len = strlen(line);
            if (!TEST_OK_(line[len - 1] == '\n', "Unterminated line"))
                break;

            line[len - 1] = '\0';
            log_message("Read line (%d): \"%s\"", sock, line);

            const char *error = process_config_command(stream, line);
            if (error)
                fprintf(stream, "ERR %s\n", error);

//             fflush(stream);
        }
        fclose(stream);
    }
    else
        close(sock);

    log_message("Connection terminated");
}
