/* Support for persistence. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>

#include "error.h"
#include "parse.h"
#include "config_server.h"
#include "config_command.h"
#include "fields.h"
#include "attributes.h"
#include "base64.h"
#include "locking.h"

#include "persistence.h"



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Persistence polling, loading and saving. */

static const char *file_name;
static char *backup_file_name;

/* This is used to track state changes. */
static struct change_set_context change_set_context;


/* Parses  <block><n>.<field>  and returns field and number. */
static error__t parse_field_name(
    const char *name, struct field **field, unsigned int *number)
{
    char block_name[MAX_NAME_LENGTH];
    struct block *block;
    unsigned int block_count;
    char field_name[MAX_NAME_LENGTH];
    return
        parse_name(&name, block_name, sizeof(block_name))  ?:
        lookup_block(block_name, &block, &block_count)  ?:
        IF_ELSE(isdigit(*name),
            parse_uint(&name, number)  ?:
            DO(*number -= 1),
        //else
            DO(*number = 0))  ?:
        TEST_OK_(*number < block_count, "Invalid block number")  ?:
        parse_char(&name, '.')  ?:
        parse_name(&name, field_name, sizeof(field_name))  ?:
        lookup_field(block, field_name, field)  ?:
        parse_char(&name, '<')  ?:
        parse_eos(&name);
}


/* Alas, this code is horribly similar to do_put_table in config_server.c, some
 * unification is in order. */
static error__t write_table_lines(
    FILE *in_file, int *line_no, struct put_table_writer *writer)
{
    error__t error = ERROR_OK;
    while (!error)
    {
        char line[MAX_LINE_LENGTH];
        size_t length;
        error =
            TEST_OK_(fgets(line, sizeof(line), in_file),
                "Unexpected end of file")  ?:
            DO(*line_no += 1; length = strlen(line))  ?:
            TEST_OK_(length > 0, "Unexpected blank line")  ?:
            TEST_OK_(line[length - 1] == '\n', "Overlong line");
        if (!error)
        {
            line[length - 1] = '\0';
            if (*line == '\0')
                break;
            else
            {
                uint32_t data[MAX_LINE_LENGTH / sizeof(uint32_t)];
                size_t converted;
                enum base64_status status =
                    base64_decode(line, data, MAX_LINE_LENGTH, &converted);
                error =
                    TEST_OK_(status == BASE64_STATUS_OK,
                        "%s", base64_error_string(status))  ?:
                    TEST_OK(converted % sizeof(uint32_t) == 0)  ?:
                    writer->write(
                        writer->context, data, converted / sizeof(uint32_t));
            }
        }
    }
    return error;
}


static error__t load_table_lines(FILE *in_file, int *line_no, char *name)
{
    struct field *field;
    unsigned int number;

    struct put_table_writer writer;
    return
        parse_field_name(name, &field, &number)  ?:
        field_put_table(field, number, false, &writer)  ?:
        DO_FINALLY(
            write_table_lines(in_file, line_no, &writer),
        //finally
            writer.close(writer.context));
}


static error__t load_one_line(FILE *in_file, int *line_no, char *line)
{
    size_t name_length = strcspn(line, "=<");

    struct connection_context context = {
        .change_set_context = &change_set_context,
    };

    switch (line[name_length])
    {
        case '=':
            line[name_length] = '\0';
            return entity_commands.put(&context, line, &line[name_length + 1]);
        case '<':
            return
                TEST_OK_(line[name_length + 1] == '\0',
                    "Unexpected extra characters on line")  ?:
                load_table_lines(in_file, line_no, line);
        default:
            return FAIL_("Malformed line");
    }
}


static void load_persistent_state(void)
{
    printf("load_persistent_state\n");

    FILE *in_file;
    error__t error =
        TEST_OK_IO_(in_file = fopen(file_name, "r"),
            "Unable to open persistent state");
    if (error)
        error_report(error);
    else
    {
        int line_no = 0;
        char line[MAX_LINE_LENGTH];
        while (fgets(line, sizeof(line), in_file))
        {
            line_no += 1;
            size_t line_length = strlen(line);

            error =
                TEST_OK_(line_length > 1, "Unexpected blank line")  ?:
                TEST_OK_(line[line_length - 1] == '\n', "Overlong line")  ?:
                DO(line[line_length - 1] = '\0')  ?:
                load_one_line(in_file, &line_no, line);
            if (error)
                ERROR_REPORT(error,
                    "Error on line %d of persistent state", line_no);
        }
        fclose(in_file);

        /* Reset change set context to the state we've just loaded.  We can do
         * this safely because the state is loaded before the socket server is
         * allowed to start. */
        uint64_t change_index = get_change_index();
        for (unsigned int i = 0; i < CHANGE_SET_SIZE; i ++)
            change_set_context.change_index[i] = change_index;
    }
}


static bool check_state_changed(void)
{
    return check_change_set(
        &change_set_context, CHANGES_CONFIG | CHANGES_ATTR | CHANGES_TABLE);
}


/* Helper function for connection_result: writes a single value to the
 * persistence state file. */
static void write_one_value(void *context, const char *string)
{
    FILE *out_file = context;
    fprintf(out_file, "%s\n", string);
}


/* Helper value for table results.  Here we do a bit of a hack and divert the
 * request to the table's .B attribute. */
static void write_table_value(void *context, const char *string)
{
    FILE *out_file = context;
    fprintf(out_file, "%s\n", string);

    struct connection_result result = {
        .write_context = context,
        .write_many = write_one_value,
    };
//     char table_b[MAX_RESULT_LENGTH];
//     snprintf(table_b, sizeof(table_b),
//         "%.*s.B", (int) (strlen(string) - 1), string);
    struct field *field;
    unsigned int number;
    struct attr *attr;
    error__t error =
        parse_field_name(string, &field, &number)  ?:
        lookup_attr(field, "B", &attr)  ?:
        attr_get(attr, number, &result);

//     error__t error = entity_commands.get(table_b, &result);
    if (error)
        ERROR_REPORT(error, "Unexpected error writing %s", string);
    fprintf(out_file, "\n");
}


static void write_changed_state(void)
{
    printf("write_changed_state\n");

    FILE *out_file;
    error__t error =
        TEST_OK_IO(out_file = fopen(backup_file_name, "w"));
    if (!error)
    {
        /* Perform write against a zero timestamp change set so that everything
         * is reported. */
        struct change_set_context zero_change_set = { };

        struct connection_result result = {
            .change_set_context = &zero_change_set,
            .write_context = out_file,
            .write_many = write_one_value,
        };

        /* First generate the single value settings.  Generate attribute values
         * first as they can affect the interpretation of the config values. */
        generate_change_sets(&result, CHANGES_ATTR);
        generate_change_sets(&result, CHANGES_CONFIG);

        /* Now generate the table settings. */
        result.write_many = write_table_value;
        generate_change_sets(&result, CHANGES_TABLE);

        fclose(out_file);

        error = TEST_IO(rename(backup_file_name, file_name));
    }

    if (error)
        ERROR_REPORT(error, "Writing persistent state");
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Persistence thread state control. */

static int poll_interval;
static int holdoff_interval;
static int backoff_interval;


/* Thread state control. */
static pthread_t persistence_thread_id;
static bool thread_running;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t psignal = PTHREAD_COND_INITIALIZER;


/* Interruptible timeout wait: returns false if thread interrupt requested. */
static bool pwait_timeout(int delay)
{
    struct timespec timeout;
    ASSERT_IO(clock_gettime(CLOCK_REALTIME, &timeout));
    timeout.tv_sec += delay;

    LOCK(mutex);
    if (thread_running)
        pthread_cond_timedwait(&psignal, &mutex, &timeout);
    bool running = thread_running;
    UNLOCK(mutex);

    return running;
}


static void stop_thread(void)
{
    LOCK(mutex);
    thread_running = false;
    pthread_cond_signal(&psignal);
    UNLOCK(mutex);
}


static void *persistence_thread(void *context)
{
    while (pwait_timeout(poll_interval))
    {
        /* Check for change. */
        if (check_state_changed())
        {
            /* First wait a bit to let all the changes settle before writing the
             * current state: typically changes will occur in a burst as things
             * are being configured. */
            pwait_timeout(holdoff_interval);
            write_changed_state();
            /* Now back off a bit so that we don't generate too many updates if
             * there are continual changes going on. */
            pwait_timeout(backoff_interval);
        }
    }

    /* On exit perform a final state check. */
    if (check_state_changed())
        write_changed_state();

    return NULL;
}


error__t initialise_persistence(
    const char *_file_name,
    int _poll_interval, int _holdoff_interval, int _backoff_interval)
{
    file_name = _file_name;
    poll_interval = _poll_interval;
    holdoff_interval = _holdoff_interval;
    backoff_interval = _backoff_interval;
    log_message("Persistence: \"%s\" %d %d %d",
        file_name, poll_interval, holdoff_interval, backoff_interval);

    asprintf(&backup_file_name, "%s.backup", file_name);
    thread_running = true;
    return
        DO(load_persistent_state())  ?:
        TRY_CATCH(
            TEST_PTHREAD(pthread_create(
                &persistence_thread_id, NULL, persistence_thread, NULL)),
        //catch
            /* If pthread creation fails we have to remember this so that
             * termination won't get muddled up. */
            thread_running = false);
}


void terminate_persistence(void)
{
    log_message("Shutting down persistent state");
    if (thread_running)
    {
        stop_thread();
        ASSERT_PTHREAD(pthread_join(persistence_thread_id, NULL));
    }
    free(backup_file_name);
}
