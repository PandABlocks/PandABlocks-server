/* Socket server for PandA. */

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <signal.h>

#include "error.h"
#include "hardware.h"
#include "socket_server.h"


static int server_port = 8888;


static bool maybe_daemonise(void)
{
    return true;
}


static void at_exit(int signum)
{
    log_message("Caught signal %d", signum);
    kill_socket_server();
}


static bool initialise_signals(void)
{
    struct sigaction do_shutdown = {
        .sa_handler = at_exit, .sa_flags = SA_RESTART };
    struct sigaction do_ignore = {
        .sa_handler = SIG_IGN, .sa_flags = SA_RESTART };
    return
        TEST_IO(sigfillset(&do_shutdown.sa_mask))  &&
        /* Catch the usual interruption signals and use them to trigger an
         * orderly shutdown.  As a reminder, these are the sources of these
         * three signals:
         *  1  HUP      Terminal hangup, also often used for config reload
         *  2  INT      Keyboard interrupt (CTRL-C)
         *  15 TERM     Normal termination request, default kill signal
         */
        TEST_IO(sigaction(SIGHUP,  &do_shutdown, NULL))  &&
        TEST_IO(sigaction(SIGINT,  &do_shutdown, NULL))  &&
        TEST_IO(sigaction(SIGTERM, &do_shutdown, NULL))  &&
        /* When acting as a server we need to ignore SIGPIPE, of course. */
        TEST_IO(sigaction(SIGPIPE, &do_ignore,   NULL));
}


int main(int argc, char **argv)
{
    bool ok =
        initialise_hardware()  &&
        initialise_socket_server(server_port)  &&

        maybe_daemonise()  &&
        initialise_signals()  &&

        /* Now run the server.  Control will not return until we're ready to
         * terminate. */
        run_socket_server();

    return ok ? 0 : 1;
}
