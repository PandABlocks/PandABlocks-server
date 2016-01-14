/* Socket server for PandA. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>

#include "error.h"
#include "hardware.h"
#include "parse.h"
#include "config_server.h"
#include "socket_server.h"
#include "database.h"
#include "system_command.h"
#include "fields.h"
#include "output.h"
#include "base64.h"
#include "persistence.h"


static unsigned int config_port = 8888;
static unsigned int data_port = 8889;
static bool reuse_addr = false;

/* Paths to configuration databases. */
static const char *config_db;
static const char *register_db;
static const char *description_db;

/* Persistence state. */
static const char *persistence_file;
static unsigned int persistence_poll = 2;
static unsigned int persistence_holdoff = 10;
static unsigned int persistence_backoff = 60;


/* Parses a persistence time specification in the form
 *
 *  [poll] [":" [holdoff] [":" backoff]]
 *
 * Note that all parts are optional, in which case the default values are used.
 * No validation of values is done. */
static error__t parse_persistence_times(const char *arg)
{
    return
        IF(isdigit(*arg), parse_uint(&arg, &persistence_poll))  ?:
        IF(read_char(&arg, ':'),
            IF(isdigit(*arg), parse_uint(&arg, &persistence_holdoff))  ?:
            IF(read_char(&arg, ':'),
                IF(isdigit(*arg), parse_uint(&arg, &persistence_backoff))))  ?:
        parse_eos(&arg);
}


static void usage(const char *argv0)
{
    printf(
"Usage: %s [options]\n"
"Runs PandA hardware interface server\n"
"\n"
"options:\n"
"   -h  Show this usage\n"
"   -p: Specify configuration port (default %d)\n"
"   -d: Specify data port (default %d)\n"
"   -R: Reuse address immediately, don't wait for stray packets to expire\n"
"   -c: Specify configuration database\n"
"   -r: Specify register database\n"
"   -D: Specify description database\n"
"   -f: Specify persistence file\n"
"   -t: Specify persistence timeouts.  Format is poll:holdoff:backoff\n"
        , argv0, config_port, data_port);
}


static error__t process_options(int argc, char *const argv[])
{
    const char *argv0 = argv[0];
    error__t error = ERROR_OK;
    while (!error)
    {
        switch (getopt(argc, argv, "+hp:d:Rc:r:D:f:t:"))
        {
            case 'h':   usage(argv0);                                   exit(0);
            case 'p':   config_port = (unsigned int) atoi(optarg);      break;
            case 'd':   data_port   = (unsigned int) atoi(optarg);      break;
            case 'R':   reuse_addr = true;                              break;
            case 'c':   config_db = optarg;                             break;
            case 'r':   register_db = optarg;                           break;
            case 'D':   description_db = optarg;                        break;
            case 'f':   persistence_file = optarg;                      break;
            case 't':   error = parse_persistence_times(optarg);        break;
            default:
                return FAIL_("Try `%s -h` for usage", argv0);
            case -1:
                argc -= optind;
                argv += optind;
                return TEST_OK_(argc == 0, "Unexpected arguments");
        }
    }
    return error;
}


static error__t maybe_daemonise(void)
{
    return ERROR_OK;
}


/* Signal handler for orderly shutdown. */
static void at_exit(int signum)
{
    kill_socket_server();
}


static error__t initialise_signals(void)
{
    struct sigaction do_shutdown = {
        .sa_handler = at_exit, .sa_flags = SA_RESTART };
    struct sigaction do_ignore = {
        .sa_handler = SIG_IGN, .sa_flags = SA_RESTART };
    return
        TEST_IO(sigfillset(&do_shutdown.sa_mask))  ?:
        /* Catch the usual interruption signals and use them to trigger an
         * orderly shutdown.  As a reminder, these are the sources of these
         * three signals:
         *  1  HUP      Terminal hangup, also often used for config reload
         *  2  INT      Keyboard interrupt (CTRL-C)
         *  15 TERM     Normal termination request, default kill signal
         */
        TEST_IO(sigaction(SIGHUP,  &do_shutdown, NULL))  ?:
        TEST_IO(sigaction(SIGINT,  &do_shutdown, NULL))  ?:
        TEST_IO(sigaction(SIGTERM, &do_shutdown, NULL))  ?:

        /* When acting as a server we need to ignore SIGPIPE, of course. */
        TEST_IO(sigaction(SIGPIPE, &do_ignore,   NULL));
}


int main(int argc, char *const argv[])
{
    initialise_base64();

    error__t error =
        process_options(argc, argv)  ?:

        initialise_fields()  ?:
        initialise_output()  ?:
        load_config_databases(config_db, register_db, description_db)  ?:

        initialise_hardware()  ?:
        initialise_system_command()  ?:
        IF(persistence_file,
            initialise_persistence(
                persistence_file,
                persistence_poll, persistence_holdoff, persistence_backoff))  ?:
        initialise_socket_server(config_port, data_port, reuse_addr)  ?:

        maybe_daemonise()  ?:
        initialise_signals();
    if (error)
        ERROR_REPORT(error, "Server startup failed");

    if (!error)
    {
        /* Now run the server.  Control will not return until we're ready to
         * terminate. */
        error = run_socket_server();

        if (error)
            ERROR_REPORT(error, "Server shutting down");
    }

    log_message("Server shutting down");

    /* Purely for the sake of valgrind heap checking, perform an orderly
     * shutdown.  Everything is done in reverse order, and each component needs
     * to cope with being called even if it was never initialised. */
    terminate_socket_server();
    terminate_persistence();

    terminate_system_command();
    terminate_hardware();
    terminate_databases();
    terminate_output();
    terminate_fields();

    return error ? 1 : 0;
}
