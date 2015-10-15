/* Generic error handling framework. */

#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <execinfo.h>
#include <syslog.h>
#include <pthread.h>

#include "error.h"



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static bool daemon_mode = false;
static bool log_verbose = true;

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK(x)     ASSERT_PTHREAD(pthread_mutex_lock(&log_mutex))
#define UNLOCK(x)   ASSERT_PTHREAD(pthread_mutex_unlock(&log_mutex))


void vlog_message(int priority, const char *format, va_list args)
{
    LOCK(log_lock);
    if (daemon_mode)
        vsyslog(priority, format, args);
    else
    {
        vfprintf(stderr, format, args);
        fprintf(stderr, "\n");
    }
    UNLOCK(log_lock);
}


void log_message(const char *message, ...)
{
    if (log_verbose)
    {
        va_list args;
        va_start(args, message);
        vlog_message(LOG_INFO, message, args);
        va_end(args);
    }
}


void log_error(const char *message, ...)
{
    va_list args;
    va_start(args, message);
    vlog_message(LOG_ERR, message, args);
    va_end(args);
}




/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Two mechanisms for reporting extra error information. */
char *_extra_io(void)
{
    /* This is very annoying: strerror() is not not necessarily thread safe ...
     * but not for any compelling reason, see:
     *  http://sources.redhat.com/ml/glibc-bugs/2005-11/msg00101.html
     * and the rather unhelpful reply:
     *  http://sources.redhat.com/ml/glibc-bugs/2005-11/msg00108.html
     *
     * On the other hand, the recommended routine strerror_r() is inconsistently
     * defined -- depending on the precise library and its configuration, it
     * returns either an int or a char*.  Oh dear.
     *
     * Ah well.  We go with the GNU definition, so here is a buffer to maybe use
     * for the message. */
    char str_error[256];
    int error = errno;
    const char *error_string =
        strerror_r(error, str_error, sizeof(str_error));
    char *result;
    asprintf(&result, "(%d) %s", error, error_string);
    return result;
}


void _report_error(char *extra, const char *format, ...)
{
    /* Large enough not to really worry about overflow.  If we do generate a
     * silly message that's too big, then that's just too bad. */
    const size_t MESSAGE_LENGTH = 512;
    char error_message[MESSAGE_LENGTH];

    va_list args;
    va_start(args, format);
    int count = vsnprintf(error_message, MESSAGE_LENGTH, format, args);
    va_end(args);

    if (extra)
        snprintf(error_message + count, MESSAGE_LENGTH - (size_t) count,
            ": %s", extra);

    log_error("%s", error_message);

    if (extra)
        free(extra);
}


void _panic_error(char *extra, const char *filename, int line)
{
    _report_error(extra, "Unrecoverable error at %s, line %d", filename, line);
    fflush(stderr);
    fflush(stdout);

    /* Now try and create useable backtrace. */
    void *backtrace_buffer[128];
    int count = backtrace(backtrace_buffer, ARRAY_SIZE(backtrace_buffer));
    backtrace_symbols_fd(backtrace_buffer, count, STDERR_FILENO);
    char last_line[128];
    int char_count = snprintf(last_line, sizeof(last_line),
        "End of backtrace: %d lines written\n", count);
    write(STDERR_FILENO, last_line, (size_t) char_count);

    _exit(255);
}
