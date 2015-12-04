/* Socket server for configuration interface. */

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "hardware.h"
#include "parse.h"
#include "buffered_file.h"
#include "config_command.h"
#include "system_command.h"
#include "classes.h"
#include "base64.h"

#include "config_server.h"


#define IN_BUF_SIZE         16384
#define OUT_BUF_SIZE        16384



/* This is set to enable logging of each incoming command. */
static bool verbose = false;



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
    ASSERT_OK((size_t) snprintf(result, length, "%.10g", value) < length);
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
 * encoded base64, with two different implementations. */
typedef error__t convert_line_t(
    const char *line, void *data, size_t *converted);


/* Fills buffer from a binary stream by reading exactly the requested number of
 * values as bytes. */
static error__t convert_base64_line(
    const char *line, void *data, size_t *converted)
{
    enum base64_status status =
        base64_decode(line, data, MAX_LINE_LENGTH, converted);
    return TEST_OK_(status == BASE64_STATUS_OK,
        "%s", base64_error_string(status));
}

/* In ASCII mode we accept a sequence of numbers on each line. */
static error__t convert_ascii_line(
    const char *line, void *data, size_t *converted)
{
    uint32_t *data_out = data;
    size_t count = 0;
    error__t error = parse_uint(&line, (unsigned int *) &data_out[count++]);
    while (!error  &&  *line != '\0')
        error =
            parse_whitespace(&line)  ?:
            parse_uint(&line, (unsigned int *) &data_out[count++]);
    *converted = sizeof(uint32_t) * count;
    return error;
}


/* Writes a single line to the table after conversion.  The conversion is
 * complicated by the the fact that binary conversion may deliver incomplete
 * words, so we need to keep track of the partial residue: this will be in the
 * range 0..3. */
static error__t put_one_line_to_table(
    const struct put_table_writer *writer,
    convert_line_t *convert_line, const char *line,
    uint32_t data_buffer[], size_t *residue)
{
    char *convert_buffer = (char *) data_buffer + *residue;
    size_t data_length;
    return
        convert_line(line, convert_buffer, &data_length)  ?:
        DO(data_length += *residue)  ?:
        writer->write(
            writer->context, data_buffer, data_length / sizeof(uint32_t))  ?:
        /* Binary conversion can leave us with fragments of words which
         * we need to carry over to the next line read and conversion. */
        DO( *residue = data_length % sizeof(uint32_t);
            data_buffer[0] = data_buffer[data_length / sizeof(uint32_t)]);
}


/* Reads blocks of data from input stream and send to put_table. */
static error__t do_put_table(
    const struct table_read_line *table_read_line,
    const struct put_table_writer *writer, convert_line_t *convert_line)
{
    uint32_t data_buffer[MAX_LINE_LENGTH / sizeof(uint32_t)];
    size_t residue = 0;

    error__t first_error = ERROR_OK;
    bool eos = false;
    while (!eos)
    {
        /* Read one line, convert to binary, write to table. */
        char line[MAX_LINE_LENGTH];
        error__t error =
            TEST_OK_(
                table_read_line->read_line(
                    table_read_line->context, line, sizeof(line)),
                "Unexpected EOF")  ?:
            IF_ELSE(*line == '\0',
                DO(eos = true),
            //else
                put_one_line_to_table(
                    writer, convert_line, line, data_buffer, &residue));

        /* Quietly discard all errors after the first error seen. */
        if (first_error)
            error_discard(error);
        else
            first_error = error;
    }

    return
        first_error  ?:
        TEST_OK_(residue == 0, "Invalid data length");
}


static error__t dummy_table_write(
    void *context, const uint32_t data[], size_t length)
{
    return ERROR_OK;
}

static void dummy_table_close(void *context, bool write_ok)
{
}

/* Dummy table writer used to accept table data stream if .put_table fails. */
static const struct put_table_writer dummy_table_writer = {
    .write = dummy_table_write,
    .close = dummy_table_close,
};


/* Parsing the table command is a little bit odd: despite the fact that the
 * command parsing may have failed, we still need to complete the command.  This
 * is so that the client can carry on sending: we carry on accepting table data
 * until a blank line is seen.
 *    This is handled in part by returning a dummy writer to receive the table
 * data if parsing failed. */
static error__t parse_table_command(
    const struct config_command_set *command_set,
    const char *command, const char *format,
    bool *binary, struct put_table_writer *writer)
{
    /* Process the format: this is of the form "<" ["<"] ["B"] , except the
     * first "<" has already been consumed. */
    bool append = read_char(&format, '<');  // Table append operation
    *binary = read_char(&format, 'B');      // Table data is in binary format

    /* If the request is malformed we'll ignore it, otherwise we'll do our best
     * to accept what was meant. */
    error__t error =
        parse_eos(&format)  ?:
        command_set->put_table(command, append, writer);
    if (error)
        /* If any part of the parse failed use a dummy writer so that we can at
         * least complete the write. */
        *writer = dummy_table_writer;
    return error;
}


/* Processes command of the form [*]name<format
 *
 * We end up having to handle up to two errors, depending on whether the parse
 * fails or writing fails later on. */
error__t process_put_table_command(
    const struct config_command_set *command_set,
    const struct table_read_line *table_read_line,
    const char *name, const char *format)
{
    struct put_table_writer writer;
    bool binary;
    error__t error = parse_table_command(
        command_set, name, format, &binary, &writer);

    /* Handle the rest of the input. */
    error__t put_error = do_put_table(
        table_read_line, &writer,
        binary ? convert_base64_line : convert_ascii_line);
    writer.close(writer.context, !put_error);

    /* Now we may have multiple errors.  Return the first one and log the
     * second, if necessary. */
    if (error  &&  put_error)
        ERROR_REPORT(put_error, "Extra error while handling do_put_table");
    return error  ?:  put_error;
}


static bool wrap_read_line(void *context, char *line, size_t length)
{
    return read_line(context, line, length, false);
}

static void do_table_command(
    struct config_connection *connection,
    const char *command, const char *format,
    const struct config_command_set *command_set)
{
    struct table_read_line table_read_line = {
        .context = connection->file,
        .read_line = wrap_read_line,
    };

    report_status(connection,
        process_put_table_command(
            command_set, &table_read_line, command, format));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Top level command processing. */


void set_config_server_verbosity(bool _verbose)
{
    verbose = _verbose;
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
    /* Create connection management structure here.  This will be passed through
     * to act as a connection context throughout the lifetime of this
     * connection. */
    struct config_connection connection = {
        .file = create_buffered_file(sock, IN_BUF_SIZE, OUT_BUF_SIZE),
    };

    char line[MAX_LINE_LENGTH];
    while (read_line(connection.file, line, sizeof(line), true))
    {
        if (verbose)
            log_message("< %s", line);
        process_config_command(&connection, line);
    }
    return destroy_buffered_file(connection.file);
}
