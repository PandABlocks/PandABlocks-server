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
#include "data_server.h"
#include "database.h"
#include "system_command.h"
#include "attributes.h"
#include "fields.h"
#include "output.h"
#include "time.h"
#include "base64.h"
#include "persistence.h"
#include "version.h"
#include "metadata.h"
#include "extension.h"
#include "mac_address.h"


static unsigned int config_port = 8888;
static unsigned int data_port = 8889;
static unsigned int extension_port = 0;
static bool reuse_addr = false;

/* Paths to configuration databases. */
static const char *config_dir;

/* Persistence state. */
static const char *persistence_file;
static unsigned int persistence_poll = 20;
static unsigned int persistence_holdoff = 30;
static unsigned int persistence_backoff = 60;

/* Option for loading MAC addresses at startup. */
static const char *mac_address_filename = NULL;

/* Daemon state. */
static bool daemon_mode = false;
static const char *pid_filename = NULL;

/* This flag is set to trigger shutdown immediately after completing startup
 * checks.  This is used for testing and configuration file validation. */
static bool test_config_only = false;

/* String used to report rootfs_version on system startup via *IDN? command. */
static const char *rootfs_version = "(unknown)";

/* Extension server legacy mode flag. */
static bool legacy_mode = false;


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

/* Parses unsigned integer. */
static error__t parse_port(const char *arg, unsigned int *port)
{
    return
        parse_uint(&arg, port)  ?:
        parse_eos(&arg)  ?:
        TEST_OK_(0 < *port && *port < 65536, "Invalid port number");
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
"   -R  Reuse address immediately, don't wait for stray packets to expire\n"
"   -c: Specify configuration directory\n"
"   -f: Specify persistence file\n"
"   -t: Specify persistence timeouts.  Format is poll:holdoff:backoff\n"
"   -D  Run server as a daemon\n"
"   -P: Write process id to given file name\n"
"   -T  Run in test mode and terminate immediately after initialisation\n"
"   -M: Load MAC addresses from specified file\n"
"   -X: Use extension server on specified port\n"
"   -r: Specify rootfs version to report via *IDN? command\n"
"   -L  Run extension server in legacy mode\n"
        , argv0, config_port, data_port);
}


static error__t process_options(int argc, char *const argv[])
{
    const char *argv0 = argv[0];
    error__t error = ERROR_OK;
    while (!error)
    {
        switch (getopt(argc, argv, "+hp:d:Rc:f:t:DP:TM:X:r:L"))
        {
            case 'h':   usage(argv0);                                   exit(0);
            case 'p':   error = parse_port(optarg, &config_port);       break;
            case 'd':   error = parse_port(optarg, &data_port);         break;
            case 'R':   reuse_addr = true;                              break;
            case 'c':   config_dir = optarg;                            break;
            case 'f':   persistence_file = optarg;                      break;
            case 't':   error = parse_persistence_times(optarg);        break;
            case 'D':   daemon_mode = true;                             break;
            case 'P':   pid_filename = optarg;                          break;
            case 'T':   test_config_only = true;                        break;
            case 'M':   mac_address_filename = optarg;                  break;
            case 'X':   error = parse_port(optarg, &extension_port);    break;
            case 'r':   rootfs_version = optarg;                        break;
            case 'L':   legacy_mode = true;                             break;
            default:
                return FAIL_("Try `%s -h` for usage", argv0);
            case -1:
                return TEST_OK_(argc == optind, "Unexpected arguments");
        }
    }
    return error;
}


static error__t maybe_daemonise(void)
{
    FILE *pid_file = NULL;
    error__t error =
        /* The logic here is a little odd: we want to check that we can write
         * the PID file before daemonising, to ensure that the caller gets the
         * error message if daemonising fails, but we need to write the PID file
         * afterwards to get the right PID. */
        IF(pid_filename,
            TEST_OK_(pid_file = fopen(pid_filename, "wx"),
                "PID file already exists: is server already running?"))  ?:
        IF(daemon_mode,
            /* Don't chdir to / so that we can unlink(pid_filename) at end. */
            TEST_IO(daemon(true, false))  ?:
            DO(start_logging("PandA server")))  ?:
        IF(pid_file,
            TEST_IO(fprintf(pid_file, "%d", getpid())));

    if (pid_file)
        fclose(pid_file);
    return error;
}


/* Signal handler for orderly shutdown. */
static void at_exit(int signum)
{
    kill_socket_server();
}


static error__t initialise_signals(void)
{
    sigset_t signal_mask;
    struct sigaction do_shutdown = {
        .sa_handler = at_exit, .sa_flags = SA_RESTART };
    struct sigaction do_ignore = {
        .sa_handler = SIG_IGN, .sa_flags = SA_RESTART };
    struct sigaction do_default = { .sa_handler = SIG_DFL, };
    return
        /* Make sure that we can actually see the signals we're going handle,
         * and block everything else. */
        TEST_IO(sigfillset(&signal_mask))  ?:
        TEST_IO(sigdelset(&signal_mask, SIGHUP))  ?:
        TEST_IO(sigdelset(&signal_mask, SIGINT))  ?:
        TEST_IO(sigdelset(&signal_mask, SIGTERM))  ?:
        TEST_IO(sigdelset(&signal_mask, SIGPIPE))  ?:
        TEST_IO(sigdelset(&signal_mask, SIGQUIT))  ?:
        TEST_IO(sigprocmask(SIG_SETMASK, &signal_mask, NULL))  ?:

        /* Catch the usual interruption signals and use them to trigger an
         * orderly shutdown.  As a reminder, these are the sources of these
         * three signals:
         *  1  HUP      Terminal hangup, also often used for config reload
         *  2  INT      Keyboard interrupt (CTRL-C)
         *  15 TERM     Normal termination request, default kill signal */
        TEST_IO(sigfillset(&do_shutdown.sa_mask))  ?:
        TEST_IO(sigaction(SIGHUP,  &do_shutdown, NULL))  ?:
        TEST_IO(sigaction(SIGINT,  &do_shutdown, NULL))  ?:
        TEST_IO(sigaction(SIGTERM, &do_shutdown, NULL))  ?:

        /* When acting as a server we need to ignore SIGPIPE, of course. */
        TEST_IO(sigaction(SIGPIPE, &do_ignore,   NULL))  ?:

        /* Allow SIGQUIT to kill us unconditionally.  This is useful if the
         * server has become stuck. */
        TEST_IO(sigaction(SIGQUIT, &do_default,  NULL));
}


int main(int argc, char *const argv[])
{
    log_message(
        "Starting %s server version %s built %s",
        server_name, server_version, server_build_date);
    initialise_base64();

    error__t error =
        process_options(argc, argv)  ?:

        initialise_metadata()  ?:
        initialise_fields()  ?:
        initialise_output()  ?:
        initialise_time()  ?:
        initialise_system_command(rootfs_version)  ?:

        initialise_signals()  ?:
        initialise_hardware()  ?:
        IF(extension_port,
            initialise_extension_server(extension_port, legacy_mode))  ?:
        load_config_databases(config_dir)  ?:

        IF(persistence_file,
            initialise_persistence(
                persistence_file,
                persistence_poll, persistence_holdoff, persistence_backoff))  ?:
        IF(mac_address_filename,
            load_mac_address_file(mac_address_filename))  ?:
        initialise_data_server()  ?:
        initialise_socket_server(config_port, data_port, reuse_addr)  ?:

        maybe_daemonise();

    if (!ERROR_REPORT(error, "Server startup failed")  &&  !test_config_only)
    {
        /* Now run the server.  Control will not return until we're ready to
         * terminate. */
        log_message("Server started");
        error =
            IF(persistence_file, start_persistence())  ?:
            start_data_server()  ?:
            run_socket_server();
        ERROR_REPORT(error, "Server shutting down");
    }

    log_message("Server shutting down");

    /* Purely for the sake of valgrind heap checking, perform an orderly
     * shutdown.  Everything is done in reverse order, and each component needs
     * to cope with being called even if it was never initialised. */
    terminate_data_server_early();
    terminate_socket_server();
    terminate_extension_server();
    terminate_persistence();

    terminate_data_server();
    terminate_hardware();
    terminate_system_command();
    terminate_time();
    terminate_output();
    terminate_fields();
    terminate_metadata();

    if (pid_filename)
        unlink(pid_filename);

    return error ? 1 : 0;
}
