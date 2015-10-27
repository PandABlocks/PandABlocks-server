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
#include "parse.h"
#include "config_command.h"
#include "system_command.h"

#include "config_server.h"


/* This should be long enough for any reasonable command. */
#define MAX_LINE_LENGTH     1024

#define TABLE_BUFFER_SIZE   4096



/* This structure holds the local state for a config socket connection. */
struct config_connection {
    FILE *stream;               // Stream connection to client
};


/* A note on error handling.  It seems to be harmless to read and write from a
 * stream in error state, and it turns out that mostly errors aren't reported
 * until we try and read anyway (because of buffering), so we just ignore stream
 * errors until we come to read.
 *   Actually, it turns out we are losing one piece of information: by the time
 * we get around to testing ferror() any assignment to errno will be utterly
 * unreliable.  In practice there really isn't enough information to make fixing
 * this worthwhile. */


static void report_error(
    struct config_connection *connection, command_error_t command_error)
{
    fprintf(connection->stream, "ERR %s\n", error_format(command_error));
    error_discard(command_error);
}

static void report_status(
    struct config_connection *connection, command_error_t command_error)
{
    if (command_error)
        report_error(connection, command_error);
    else
        fprintf(connection->stream, "OK\n");
}


static void write_one_result(
    struct config_connection *connection, const char *result)
{
    fprintf(connection->stream, "OK =%s\n", result);
}

static void write_many_result(
    struct config_connection *connection, const char *result, bool last)
{
    if (last)
        fprintf(connection->stream, ".\n");
    else
        fprintf(connection->stream, "!%s\n", result);
}

static struct connection_result connection_result = {
    .write_one  = write_one_result,
    .write_many = write_many_result,
};



/* Processes command of the form [*]name? */
static void do_read_command(
    struct config_connection *connection,
    const char *command, const char *value,
    const struct config_command_set *command_set)
{
    if (*value == '\0')
    {
        command_error_t command_error = command_set->get(
            connection, command, &connection_result);
        /* We only need to report an error, any success will have been reported
         * by .get(). */
        if (command_error)
            report_error(connection, command_error);
    }
    else
        report_error(connection, FAIL_("Unexpected text after command"));
}


/* Processes command of the form [*]name=value */
static void do_write_command(
    struct config_connection *connection,
    const char *command, const char *value,
    const struct config_command_set *command_set)
{
    report_status(connection, command_set->put(connection, command, value));
}


typedef command_error_t fill_buffer_t(
    struct config_connection *connection,
    unsigned int data_buffer[], unsigned int to_read,
    unsigned int *seen, bool *eos);


/* Fills buffer from a binary stream by reading exactly the requested number of
 * values as bytes. */
static command_error_t fill_binary_buffer(
    struct config_connection *connection,
    unsigned int data_buffer[], unsigned int to_read,
    unsigned int *seen, bool *eos)
{
    *seen = (unsigned int) fread(
        data_buffer, sizeof(int32_t), to_read, connection->stream);
    /* If we see eof on input try to report it to the client.  Doesn't
     * particularly matter if this fails! */
    return TEST_OK_(*seen == to_read, "Error on table input");
}


/* Fills buffer from a text stream by reading formatted numbers until an error
 * or a blank line is encountered. */
static command_error_t fill_ascii_buffer(
    struct config_connection *connection,
    unsigned int data_buffer[], unsigned int to_read,
    unsigned int *seen, bool *eos)
{
    char line[MAX_LINE_LENGTH];
    /* Reduce the target count by a headroom factor so that we can process each
     * full line read. */
    unsigned int headroom = sizeof(line) / 2;   // At worst 1 number for 2 chars
    ASSERT_OK(to_read > headroom);
    to_read -= headroom;

    /* Loop until end of input stream (blank line), a parsing error, fgets
     * fails, or we fill the target buffer (subject to headroom). */
    *seen = 0;
    *eos = false;
    command_error_t error = ERROR_OK;
    while (!error  &&  !*eos  &&  *seen < to_read)
    {
        error = TEST_OK_(
           fgets(line, sizeof(line), connection->stream), "Unexpected EOF");

        const char *data_in = skip_whitespace(line);
        if (*data_in == '\n')
            *eos = true;
        else
            while (!error  &&  *data_in != '\n')
                error =
                    parse_uint(&data_in, &data_buffer[(*seen)++])  ?:
                    DO(data_in = skip_whitespace(data_in));
    }
    return error;
}


/* Reads blocks of data from input stream and send to put_table. */
static command_error_t do_put_table(
    struct config_connection *connection, const char *command,
    const struct config_command_set *command_set,
    fill_buffer_t *fill_buffer, bool binary, bool append, unsigned int count)
{
    command_error_t command_error = ERROR_OK;
    bool eos = false;
    while (!command_error  &&  !eos)
    {
        unsigned int to_read =
            binary ?
                count > TABLE_BUFFER_SIZE ? TABLE_BUFFER_SIZE : count :
            TABLE_BUFFER_SIZE;

        unsigned int data_buffer[TABLE_BUFFER_SIZE];
        unsigned int seen;
        command_error =
            fill_buffer(connection, data_buffer, to_read, &seen, &eos)  ?:
            command_set->put_table(
                connection, command, data_buffer, seen, append);

        append = true;
        if (binary)
        {
            eos = count <= seen;    // Better not be less than!
            count -= seen;
        }
    }
    return command_error;
}


/* Processes command of the form [*]name<format
 * This has the special condition that the input stream will be read for further
 * data, and so the put_table function can return one of two different error
 * codes: if a communication error is reported then we need to drop the
 * connection. */
static void do_table_command(
    struct config_connection *connection,
    const char *command, const char *format,
    const struct config_command_set *command_set)
{
    /* Process the format: this is of the form "<" ["<"] ["B" count] .*/
    bool append = read_char(&format, '<');
    bool binary = read_char(&format, 'B');

    unsigned int count = 0;
    command_error_t command_error =
        /* Binary flag must be followed by a non-zero count. */
        IF(binary,
            parse_uint(&format, &count)  ?:
            TEST_OK_(count > 0, "Zero count invalid"))  ?:
        parse_eos(&format)  ?:
        do_put_table(
            connection, command, command_set,
            binary ? fill_binary_buffer : fill_ascii_buffer,
            binary, append, count);

    report_status(connection, command_error);
}


/* Processes a general configuration command.  Error reporting to the user is
 * very simple: a fixed error message is returned if the command fails,
 * otherwise the command is responsible for reporting success and any other
 * output. */
static void process_config_command(
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
            do_read_command(connection, command, value, command_set);   break;
        case '=':
            do_write_command(connection, command, value, command_set);  break;
        case '<':
            do_table_command(connection, command, value, command_set);  break;
        default:
            report_error(connection, FAIL_("Unknown command"));         break;
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
            error = TEST_OK_(line[len - 1] == '\n', "Unterminated line");
            if (!error)
            {
                line[len - 1] = '\0';
                process_config_command(&connection, line);
            }
        }
        if (!error)
            error = TEST_OK_(!ferror(stream), "Error on stream");
        fclose(stream);
    }
    return error;
}
