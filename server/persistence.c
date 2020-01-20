/* Support for persistence. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>

#include "error.h"
#include "parse.h"
#include "config_server.h"
#include "config_command.h"
#include "system_command.h"
#include "attributes.h"
#include "fields.h"
#include "metadata.h"
#include "base64.h"
#include "locking.h"

#include "persistence.h"



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Persistence polling, loading and saving. */

static const char *file_name;
static char *backup_file_name;

/* This is used to track state changes. */
static struct change_set_context change_set_context;

/* This is the set of changes recorded in the state file. */
#define PERSIST_CHANGES \
    (CHANGES_CONFIG | CHANGES_ATTR | CHANGES_TABLE | CHANGES_METADATA)


/* Structure for line reading: this will be passed through to
 * process_put_table_command() when loading table data from the persistence
 * file. */
struct read_line_context {
    int line_no;
    FILE *file;
};

static bool do_read_line(void *context, char line[], size_t max_length)
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
static error__t load_one_value(struct read_line_context *read_line, char line[])
{
    size_t name_length = strcspn(line, "=<");
    char action = line[name_length];
    line[name_length] = '\0';
    char *value = &line[name_length + 1];

    /* * prefix switches between system and entity command sets. */
    const struct config_command_set *command_set = &entity_commands;
    if (*line == '*')
    {
        command_set = &system_commands;
        line += 1;
    }

    switch (action)
    {
        case '=':
        {
            struct connection_context context = {
                .change_set_context = &change_set_context,
            };
            return command_set->put(&context, line, value);
        }
        case '<':
        {
            struct table_read_line table_read_line = {
                .context = read_line,
                .read_line = do_read_line,
            };
            return process_put_table_command(
                command_set, &table_read_line, line, value);
        }
        default:
            return FAIL_("Malformed line");
    }
}


static void load_persistent_state(void)
{
    log_message("Loading persistence file");

    FILE *in_file;
    error__t error =
        TEST_OK_IO_(in_file = fopen(file_name, "r"),
            "Unable to open persistent state");
    if (error)
        error_report(error);
    else
    {
        unsigned int error_count = 0;
        struct read_line_context read_line = { .file = in_file, };
        char line[MAX_LINE_LENGTH];
        while (do_read_line(&read_line, line, sizeof(line)))
        {
            error = load_one_value(&read_line, line);
            if (error)
            {
                if (error_count == 0)
                    ERROR_REPORT(error,
                        "Unable to load line %d (%s) of persistent state",
                        read_line.line_no, line);
                else
                    error_discard(error);
                error_count += 1;
            }
        }
        fclose(in_file);
        if (error_count)
            log_error("Unable to load %u lines from persistent state",
                error_count);

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
    return check_change_set(&change_set_context, PERSIST_CHANGES);
}


/* Helper function for connection_result: writes a single value to the
 * persistence state file. */
static void write_one_line(void *context, const char *string)
{
    FILE *out_file = context;
    fprintf(out_file, "%s\n", string);
}


static void write_changed_state(void)
{
    FILE *out_file;
    error__t error =
        TEST_OK_IO(out_file = fopen(backup_file_name, "w"));
    if (!error)
    {
        /* Perform write against a zero timestamp change set so that everything
         * is reported. */
        struct change_set_context zero_change_set = { };

        char string[MAX_RESULT_LENGTH];
        struct connection_result result = {
            .change_set_context = &zero_change_set,
            .string = string,
            .length = sizeof(string),
            .write_context = out_file,
            .write_many = write_one_line,
            .response = RESPONSE_ERROR,
        };

        /* Start by resetting the change context so that we're up to date before
         * we start writing. */
        uint64_t report_index[CHANGE_SET_SIZE];       // This will be discarded
        update_change_index(&change_set_context, PERSIST_CHANGES, report_index);

        /* First generate the single value settings.  Generate attribute values
         * first as they can affect the interpretation of the config values. */
        generate_change_sets(&result, CHANGES_ATTR, true);
        generate_change_sets(&result, CHANGES_CONFIG, true);

        /* Now generate the table settings. */
        generate_change_sets(&result, CHANGES_TABLE, true);
        generate_change_sets(&result, CHANGES_METADATA, true);

        error =
            TEST_IO(fclose(out_file))  ?:
            TEST_IO(rename(backup_file_name, file_name))  ?:
            DO(sync());
    }

    if (error)
        ERROR_REPORT(error, "Writing persistent state");
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Persistence thread state control. */

static unsigned int poll_interval;
static unsigned int holdoff_interval;
static unsigned int backoff_interval;


/* Thread state control. */
static pthread_t persistence_thread_id;
static bool thread_running;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t psignal;


/* Interruptible timeout wait: returns false if thread interrupt requested. */
static bool interruptible_timeout(unsigned int delay)
{
    LOCK(mutex);
    if (thread_running)
        pwait_timeout(
            &mutex, &psignal, &(struct timespec) { .tv_sec = (time_t) delay, });
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
    while (interruptible_timeout(poll_interval))
    {
        /* Check for change. */
        if (check_state_changed())
        {
            /* First wait a bit to let all the changes settle before writing the
             * current state: typically changes will occur in a burst as things
             * are being configured. */
            interruptible_timeout(holdoff_interval);

            LOCK(mutex);
            write_changed_state();
            UNLOCK(mutex);

            /* Now back off a bit so that we don't generate too many updates if
             * there are continual changes going on. */
            interruptible_timeout(backoff_interval);
        }
    }

    /* On exit perform a final state check. */
    if (check_state_changed())
    {
        LOCK(mutex);
        write_changed_state();
        UNLOCK(mutex);
    }

    return NULL;
}


error__t initialise_persistence(
    const char *_file_name,
    unsigned int _poll_interval, unsigned int _holdoff_interval,
    unsigned int _backoff_interval)
{
    file_name = _file_name;
    poll_interval = _poll_interval;
    holdoff_interval = _holdoff_interval;
    backoff_interval = _backoff_interval;
    log_message("Persistence: \"%s\" %u %u %u",
        file_name, poll_interval, holdoff_interval, backoff_interval);

    ASSERT_IO(asprintf(&backup_file_name, "%s.backup", file_name));

    pwait_initialise(&psignal);
    load_persistent_state();
    return ERROR_OK;
}


error__t start_persistence(void)
{
    thread_running = true;
    return
        /* Start the monitor thread. */
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


error__t save_persistent_state(void)
{
    if (thread_running)
    {
        /* Because we want to block until the write has completed, we trigger a
         * write directly under the feed of the persistence_thread.  The
         * alternative of waking the thread and waiting for a completion event
         * is unnecessarily complicated. */
        LOCK(mutex);
        write_changed_state();
        UNLOCK(mutex);
        return ERROR_OK;
    }
    else
        /* If persistence was never initialised we can't do anything here. */
        return FAIL_("No persistence state configured");
}
