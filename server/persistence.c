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


/* Structure for line reading: this will be passed through to
 * process_put_table_command() when loading table data from the persistence
 * file. */
struct read_line_context {
    int line_no;
    FILE *file;
};

static bool do_read_line(void *context, char *line, size_t max_length)
{
    struct read_line_context *read_context = context;
    if (fgets(line, (int) max_length, read_context->file))
    {
        read_context->line_no += 1;
        size_t length = strlen(line);
        error__t error =
            TEST_OK_(length > 0, "Unexpected empty line")  ?:
            TEST_OK_(line[length - 1] == '\n', "Line too long");
        if (error)
        {
            ERROR_REPORT(error,
                "Error reading line %d of persistent state",
                read_context->line_no);
            return false;
        }
        else
        {
            line[length - 1] = '\0';
            return true;
        }
    }
    else
        return false;
}


/* Handles one value definition read from the file.  Each line should either be
 * an assignment or a table entry, we invoke the appropriate handler.  The table
 * handler needs a bit of help. */
static error__t load_one_value(struct read_line_context *read_line, char *line)
{
    size_t name_length = strcspn(line, "=<");
    char action = line[name_length];
    line[name_length] = '\0';
    char *value = &line[name_length + 1];

    switch (action)
    {
        case '=':
        {
            struct connection_context context = {
                .change_set_context = &change_set_context,
            };
            return entity_commands.put(&context, line, value);
        }
        case '<':
        {
            struct table_read_line table_read_line = {
                .context = read_line,
                .read_line = do_read_line,
            };
            return process_put_table_command(
                &entity_commands, &table_read_line, line, value);
        }
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
        struct read_line_context read_line = { .file = in_file, };
        char line[MAX_LINE_LENGTH];
        while (do_read_line(&read_line, line, sizeof(line)))
        {
            error = load_one_value(&read_line, line);
            if (error)
                ERROR_REPORT(error,
                    "Error on line %d of persistent state", read_line.line_no);
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
    fprintf(out_file, "%sB\n", string);

    struct connection_result result = {
        .write_context = context,
        .write_many = write_one_value,
    };

    /* A bit of a hack here.  We know that string is block.field< and we want to
     * generate block.field.B. */
    char table_b[2*MAX_NAME_LENGTH];
    snprintf(table_b, sizeof(table_b),
        "%.*s.B", (int) (strlen(string) - 1), string);

    error__t error = entity_commands.get(table_b, &result);
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

        /* Start by resetting the change context so that we're up to date before
         * we start writing. */
        reset_change_context(
            &change_set_context, CHANGES_CONFIG | CHANGES_ATTR | CHANGES_TABLE);

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
static pthread_cond_t psignal;


/* Interruptible timeout wait: returns false if thread interrupt requested. */
static bool pwait_timeout(int delay)
{
    struct timespec timeout;
    ASSERT_IO(clock_gettime(CLOCK_MONOTONIC, &timeout));
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

    pthread_condattr_t attr;
    return
        /* Need to initialise the shutdown/wakeup signal we're using to use the
         * CLOCK_MONOTONIC clock. */
        TEST_PTHREAD(pthread_condattr_init(&attr))  ?:
        TEST_PTHREAD(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC))  ?:
        TEST_PTHREAD(pthread_cond_init(&psignal, &attr))  ?:

        /* Load state and start the monitor thread. */
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
