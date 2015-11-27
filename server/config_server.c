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
#include "buffered_file.h"
#include "config_command.h"
#include "system_command.h"
#include "classes.h"

#include "config_server.h"


/* This should be long enough for any reasonable command. */
#define MAX_LINE_LENGTH     1024

#define TABLE_BUFFER_SIZE   4096U

#define IN_BUF_SIZE         16384
#define OUT_BUF_SIZE        16384





/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Formatting helper methods. */

error__t __attribute__((format(printf, 3, 4))) format_string(
    char result[], size_t length, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int written = vsnprintf(result, length, format, args);
    va_end(args);

    return TEST_OK_(written >= 0  &&  (size_t) written < length,
        "Result too long");
}


/* Alas the double formatting rules are ill mannered, in particular I don't want
 * to allow leading spaces.  I'd also love to prune trailing zeros, but we'll
 * see. */
error__t format_double(char result[], size_t length, double value)
{
    ASSERT_OK((size_t) snprintf(result, length, "%12g", value) < length);
    const char *skip = skip_whitespace(result);
    if (skip > result)
        memmove(result, skip, strlen(skip) + 1);
    return ERROR_OK;
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Change set management. */

/* This number is used to work out which fields have changed since we last
 * looked.  This is incremented on every update. */
static uint64_t global_change_index = 0;


/* Allocates and returns a fresh change index. */
uint64_t get_change_index(void)
{
    return __sync_add_and_fetch(&global_change_index, 1);
}


void update_change_index(
    struct change_set_context *context,
    enum change_set change_set, uint64_t reported[CHANGE_SET_SIZE])
{
    uint64_t change_index = get_change_index();
    for (unsigned int i = 0; i < CHANGE_SET_SIZE; i ++)
        if (change_set & (1U << i))
        {
            reported[i] = context->change_index[i];
            context->change_index[i] = change_index;
        }
        else
            /* If this change isn't to be reported, push the report index out to
             * the indefinite future. */
            reported[i] = (uint64_t) (int64_t) -1;
}


void reset_change_context(
    struct change_set_context *context, enum change_set change_set)
{
    uint64_t change_index = get_change_index();
    for (unsigned int i = 0; i < CHANGE_SET_SIZE; i ++)
        if (change_set & (1U << i))
            context->change_index[i] = change_index;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* This structure holds the local state for a config socket connection. */
struct config_connection {
    struct buffered_file *file;
    struct change_set_context change_set_context;
};


/* Writes error code to client, consumes and released error. */
static void report_error(
    struct config_connection *connection, error__t error)
{
    const char *message = error_format(error);
    write_string(connection->file, "ERR ", 4);
    write_string(connection->file, message, strlen(message));
    write_char(connection->file, '\n');
    error_discard(error);
}


/* Reports command status, either OK or error as appropriate, releases error
 * code if necessary. */
static void report_status(
    struct config_connection *connection, error__t error)
{
    if (error)
        report_error(connection, error);
    else
        write_string(connection->file, "OK\n", 3);
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Simple read and write commands. */


/* Interface for multi-line response, called repeatedly until all done. */
static void write_many_result(void *context, const char *result)
{
    struct buffered_file *file = context;
    write_char(file, '!');
    write_string(file, result, strlen(result));
    write_char(file, '\n');
}


/* Processes command of the form [*]name? */
static void do_read_command(
    struct config_connection *connection,
    const char *command, const char *value,
    const struct config_command_set *command_set)
{
    if (*value == '\0')
    {
        char string[MAX_RESULT_LENGTH];
        struct connection_result result = {
            .change_set_context = &connection->change_set_context,
            .string = string,
            .length = sizeof(string),
            .write_context = connection->file,
            .write_many = write_many_result,
            .response = RESPONSE_ERROR,
        };
        error__t error = command_set->get(command, &result);
        /* Ensure error return and result response are consistent. */
        ASSERT_OK((error != ERROR_OK) == (result.response == RESPONSE_ERROR));
        switch (result.response)
        {
            case RESPONSE_ERROR:
                report_error(connection, error);
                break;
            case RESPONSE_ONE:
                write_string(connection->file, "OK =", 4);
                write_string(connection->file, string, strlen(string));
                write_char(connection->file, '\n');
                break;
            case RESPONSE_MANY:
                write_string(connection->file, ".\n", 2);
                break;
        }
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
    struct connection_context context = {
        .change_set_context = &connection->change_set_context,
    };
    report_status(connection, command_set->put(&context, command, value));
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Table write command. */


/* Two different sources of table data: ASCII encoded (printable numbers) or
 * binary, with two different implementations. */
typedef error__t fill_buffer_t(
    struct config_connection *connection,
    unsigned int data_buffer[], unsigned int to_read,
    unsigned int *seen, bool *eos);


/* Fills buffer from a binary stream by reading exactly the requested number of
 * values as bytes. */
static error__t fill_binary_buffer(
    struct config_connection *connection,
    unsigned int data_buffer[], unsigned int to_read,
    unsigned int *seen, bool *eos)
{
    return TEST_OK_(
        read_block(connection->file,
            (char *) data_buffer, sizeof(unsigned int) * to_read),
        "Error on table input");
}


/* Fills buffer from a text stream by reading formatted numbers until an error
 * or a blank line is encountered. */
static error__t fill_ascii_buffer(
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
    error__t error = ERROR_OK;
    while (!error  &&  !*eos  &&  *seen < to_read)
    {
        const char *data_in = NULL;
        error =
            TEST_OK_(
               read_line(connection->file, line, sizeof(line), false),
               "Unexpected EOF")  ?:
            DO(data_in = skip_whitespace(line));

        if (!error  &&  *data_in == '\0')
            *eos = true;
        else
            while (!error  &&  *data_in != '\0')
                error =
                    parse_uint(&data_in, &data_buffer[(*seen)++])  ?:
                    DO(data_in = skip_whitespace(data_in));
    }
    return error;
}


/* Reads blocks of data from input stream and send to put_table. */
static error__t do_put_table(
    struct config_connection *connection, const char *command,
    const struct put_table_writer *writer,
    fill_buffer_t *fill_buffer, bool binary, unsigned int count)
{
    error__t error = ERROR_OK;
    bool eos = false;
    while (!error  &&  !eos)
    {
        unsigned int to_read =
            binary ? MIN(count, TABLE_BUFFER_SIZE) : TABLE_BUFFER_SIZE;

        unsigned int data_buffer[TABLE_BUFFER_SIZE];
        unsigned int seen = to_read;
        error =
            fill_buffer(connection, data_buffer, to_read, &seen, &eos)  ?:
            writer->write(writer->context, data_buffer, seen);

        if (!error  &&  binary)
        {
            eos = count <= seen;    // Better not be less than!
            count -= seen;
        }
    }
    writer->close(writer->context);
    return error;
}


static error__t dummy_table_write(
    void *context, const unsigned int data[], size_t length)
{
    return ERROR_OK;
}

static void dummy_table_close(void *context)
{
}

/* Dummy table writer used to accept table data stream if .put_table fails. */
static const struct put_table_writer dummy_table_writer = {
    .write = dummy_table_write,
    .close = dummy_table_close,
};


/* Processing a table command is a little bit tricky: once the client has
 * managed to send a valid command, they're comitted to sending the table data.
 * This means that once we've managed to parse a valid top level syntax we need
 * to accept the rest of the data stream even if the target has rejected the
 * command. */
static void complete_table_command(
    struct config_connection *connection,
    const char *command, bool append, bool binary, unsigned int count,
    const struct config_command_set *command_set)
{
    struct put_table_writer writer;
    /* Call .put_table to start the transaction, which must then be
     * completed with calls to writer. */
    error__t error = command_set->put_table(command, append, &writer);
    /* If we failed here then at least give the client a chance to discover
     * early.  If we're in ASCII mode the force the message out. */
    bool reported = error != ERROR_OK;
    if (error)
    {
        /* .put_table failed, so use the dummy writer instead. */
        writer = dummy_table_writer;
        report_error(connection, error);
        if (!binary)
            flush_out_buf(connection->file);
    }

    /* Handle the rest of the input. */
    error = do_put_table(
        connection, command, &writer,
        binary ? fill_binary_buffer : fill_ascii_buffer, binary, count);
    if (reported)
    {
        if (error)
            ERROR_REPORT(error, "Extra error while handling do_put_table");
    }
    else
        report_status(connection, error);
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
    bool append = read_char(&format, '<');  // Table append operation
    bool binary = read_char(&format, 'B');  // Table data is in binary format

    unsigned int count = 0;
    error__t error =
        /* Binary flag must be followed by a non-zero count. */
        IF(binary,
            parse_uint(&format, &count)  ?:
            TEST_OK_(count > 0, "Zero count invalid"))  ?:
        parse_eos(&format);

    /* If the above failed then the request was completely malformed, so ignore
     * it.  Otherwise we'll do our best to accept what was meant. */
    if (error)
        report_error(connection, error);
    else
        complete_table_command(
            connection, command, append, binary, count, command_set);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Top level command processing. */

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
    /* Create connection management structure here.  This will be passed through
     * to act as a connection context throughout the lifetime of this
     * connection. */
    struct config_connection connection = {
        .file = create_buffered_file(sock, IN_BUF_SIZE, OUT_BUF_SIZE),
    };

    char line[MAX_LINE_LENGTH];
    while (read_line(connection.file, line, sizeof(line), true))
        process_config_command(&connection, line);
    return destroy_buffered_file(connection.file);
}
