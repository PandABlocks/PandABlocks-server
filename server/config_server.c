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
#include "fields.h"
#include "config_command.h"
#include "system_command.h"

#include "config_server.h"


/* This should be long enough for any reasonable command. */
#define MAX_LINE_LENGTH     1024

#define TABLE_BUFFER_SIZE   4096U

#define IN_BUF_SIZE         16384
#define OUT_BUF_SIZE        16384


/*****************************************************************************/
/* Buffered file handling.
 *
 * You'd think the obvious solution was to use fdopen() to wrap the socket in a
 * stream and then lean on the easy to use stream functions.  Alas, it would
 * seem that this doesn't work terribly well with a socket interface.  The most
 * obvious problem is that if multiple commands are sent in one block they're
 * not all received!
 *
 * So we write our own buffered file handling.  Ho hum.  At least we can tweak
 * the API to suit.
 *
 * Actually, there is an alternative: apparently if we dup(2) the socket handle
 * and call fdopen twice we then get two streams which can each be treated as a
 * unidirectional stream. */

/* The input and output buffers are managed somewhat differently: we always
 * flush the entire output buffer, but the input buffer is read and filled
 * piecemeal. */
struct buffered_file {
    int file;                   // File handle to read
    bool eof;                   // Set once end of input encountered
    error__t error;             // Any error blocks all further IO processing
    size_t in_length;           // Length of data currently in in_buf
    size_t read_ptr;            // Start of readout data from in_buf
    size_t out_length;          // Length of data in out_buf
    char in_buf[IN_BUF_SIZE];   // Input buffer
    char out_buf[OUT_BUF_SIZE]; // Output buffer
};


/* Writes out the entire output buffer, retrying as necessary to ensure it's all
 * gone. */
static void flush_out_buf(struct buffered_file *file)
{
    size_t out_start = 0;
    while (!file->error  &&  out_start < file->out_length)
    {
        ssize_t written;
        file->error =
            TEST_IO_(written = write(
                    file->file, file->out_buf + out_start,
                    file->out_length - out_start),
                "Error writing to socket");
            DO(out_start += (size_t) written);
    }
    file->out_length = 0;
}


/* Reads what data is available into the input buffer. */
static void fill_in_buf(struct buffered_file *file)
{
    if (!file->eof  &&  !file->error)
    {
        ssize_t seen;
        file->error = TEST_IO_(
            seen = read(file->file, file->in_buf, IN_BUF_SIZE),
            "Error reading from socket");
        file->eof = seen == 0;
        file->read_ptr = 0;
        file->in_length = (size_t) seen;
    }
}


/* Fills the buffer as necessary to return a line.  False is returned if eof or
 * an error was encountered first.  We also flush the out buffer if the buffer
 * needs filling so that the other side of the conversation has a chance to keep
 * up. */
static bool read_line(
    struct buffered_file *file, char line[], size_t line_size, bool flush)
{
    while (!file->eof  &&  !file->error)
    {
        /* See if we've got a line in the buffer. */
        size_t data_avail = file->in_length - file->read_ptr;
        char *data_start = file->in_buf + file->read_ptr;
        char *newline = memchr(data_start, '\n', data_avail);
        if (newline)
        {
            size_t line_length = (size_t) (newline - data_start);
            file->error = TEST_OK_(line_length + 1 < line_size, "Line overrun");
            if (file->error)
                return false;
            else
            {
                memcpy(line, data_start, line_length);
                file->read_ptr += line_length + 1;
                line[line_length] = '\0';
                return true;
            }
        }

        /* Not enough data.  Empty what we have into the line, refill the
         * buffer, and try again. */
        file->error = TEST_OK_(data_avail + 1 < line_size, "Line overrun");
        if (file->error)
            return false;

        memcpy(line, data_start, data_avail);
        line += data_avail;
        line_size -= data_avail;

        if (flush)
            flush_out_buf(file);
        flush = false;
        fill_in_buf(file);
    }
    return false;
}


/* This reads a fixed size block of data, returns false if the entire block
 * cannot be read for any reason. */
static bool read_block(struct buffered_file *file, char data[], size_t length)
{
    while (!file->eof  &&  !file->error  &&  length > 0)
    {
        /* Copy what we've got to the destination. */
        size_t to_copy = MIN(file->in_length - file->read_ptr, length);
        memcpy(data, file->in_buf + file->read_ptr, to_copy);
        data += to_copy;
        length -= to_copy;
        file->read_ptr += to_copy;

        if (length > 0)
            fill_in_buf(file);
    }
    return !file->eof  &&  !file->error  &&  length == 0;
}


/* Writes a string to the output buffer, flushing it to make room if needed. */
static void write_string(
    struct buffered_file *file, const char *string, size_t length)
{
    while (!file->error  &&  length > 0)
    {
        /* Put as much of the string as possible into out_buf. */
        size_t to_write = MIN(OUT_BUF_SIZE - file->out_length, length);
        memcpy(file->out_buf + file->out_length, string, to_write);
        string += to_write;
        length -= to_write;
        file->out_length += to_write;

        /* If out_buf is full, send it. */
        if (file->out_length >= OUT_BUF_SIZE)  // Only == is possible!
            flush_out_buf(file);
    }
}


/* As we guarantee that there's always room for one character in the output
 * buffer (we always flush when full) this function can be quite simple. */
static void write_char(struct buffered_file *file, char ch)
{
    if (!file->error)
    {
        file->out_buf[file->out_length++] = ch;
        if (file->out_length >= OUT_BUF_SIZE)
            flush_out_buf(file);
    }
}



/*****************************************************************************/



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Interface to external implementation of commands. */

/* This structure holds the local state for a config socket connection. */
struct config_connection {
    struct buffered_file file;
    uint64_t change_index[CHANGE_SET_SIZE];
};


void update_change_index(
    struct config_connection *connection,
    enum change_set change_set, uint64_t change_index,
    uint64_t reported[CHANGE_SET_SIZE])
{
    for (unsigned int i = 0; i < CHANGE_SET_SIZE; i ++)
    {
        reported[i] = connection->change_index[i];
        if (change_set & (1U << i))
            connection->change_index[i] = change_index;
    }
}


/* Writes error code to client, consumes and released error. */
static void report_error(
    struct config_connection *connection, error__t error)
{
    const char *message = error_format(error);
    write_string(&connection->file, "ERR ", 4);
    write_string(&connection->file, message, strlen(message));
    write_char(&connection->file, '\n');
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
        write_string(&connection->file, "OK\n", 3);
}


/* Interface for single data response. */
static void write_one_result(
    struct config_connection *connection, const char *result)
{
    write_string(&connection->file, "OK =", 4);
    write_string(&connection->file, result, strlen(result));
    write_char(&connection->file, '\n');
}


/* Interface for multi-line response, called repeatedly until all done. */
static void write_many_result(
    struct config_connection *connection, const char *result)
{
    write_char(&connection->file, '!');
    write_string(&connection->file, result, strlen(result));
    write_char(&connection->file, '\n');
}

/* Called to complete multi-line response. */
static void write_many_end(struct config_connection *connection)
{
    write_string(&connection->file, ".\n", 2);
}


static const struct connection_result connection_result = {
    .write_one  = write_one_result,
    .write_many = write_many_result,
    .write_many_end = write_many_end,
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Simple read and write commands. */


/* Processes command of the form [*]name? */
static void do_read_command(
    struct config_connection *connection,
    const char *command, const char *value,
    const struct config_command_set *command_set)
{
    if (*value == '\0')
    {
        struct connection_result result = connection_result;
        result.connection = connection;
        error__t error = command_set->get(command, &connection_result);
        /* We only need to report an error, any success will have been reported
         * by .get(). */
        if (error)
            report_error(connection, error);
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
        read_block(&connection->file,
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
        const char *data_in;
        error =
            TEST_OK_(
               read_line(&connection->file, line, sizeof(line), false),
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
    struct put_table_writer writer = dummy_table_writer;
    /* Call .put_table to start the transaction, which must then be
     * completed with calls to writer. */
    error__t error =
        command_set->put_table(connection, command, append, &writer);
    /* If we failed here then at least give the client a chance to discover
     * early.  If we're in ASCII mode the force the message out. */
    bool reported = error != ERROR_OK;
    if (error)
    {
        report_error(connection, error);
        if (!binary)
            flush_out_buf(&connection->file);
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
        .file = { .file = sock },
    };

    char line[MAX_LINE_LENGTH];
    while (read_line(&connection.file, line, sizeof(line), true))
        process_config_command(&connection, line);
    return connection.file.error;
}
