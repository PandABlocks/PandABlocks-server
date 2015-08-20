/* Socket server core.  Maintains listening socket. */

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "error.h"
#include "hardware.h"
#include "socket_server.h"



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Single server thread. */


#define MAX_LINE_LENGTH     80


static void *dummy_process(void *context)
{
    int scon = (int) (intptr_t) context;
    log_message("Process connection on %d", scon);

    FILE *stream;
    if (TEST_IO(stream = fdopen(scon, "r+")))
    {
        char line[MAX_LINE_LENGTH];
        while (fgets(line, sizeof(line), stream))
        {
            size_t len = strlen(line);
            if (!TEST_OK_(line[len - 1] == '\n', "Unterminated line"))
                break;

            line[len - 1] = '\0';
            log_message("Read line (%d): \"%s\"", scon, line);
            fprintf(stream, "ECHO: %s\n", line);
            fflush(stream);
        }
        fclose(stream);
    }
    else
        close(scon);

    log_message("Connection terminated");
    return NULL;
}


static void *(*process_config)(void *) = dummy_process;
static void *(*process_data)(void *) = dummy_process;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Connection handling. */

static int config_socket = -1;
static int data_socket = -1;

/* This flag inhabits contentious territory.  It serves one purpose, to help the
 * run_socket_server() loop to terminate gracefully after being poked by
 * kill_socket_server().  Classically we are told that volatile is not useful
 * when working with multiple threads (memory barriers are also needed) and that
 * we should be using pthread locking around access to this flag to be safe.
 *    However, this flag is set from within kill_socket_server, and this is
 * called from within a signal handler, and of course pthread locking is rather
 * unsafe in this context.
 *    The main hazard is that when running is cleared the thread which checks it
 * won't see the flag change (because of cache race conditions).  However,
 * actually this won't really matter because the system calls in the main thread
 * will just fail instead.  The point of this flag is to try to avoid the error
 * messages from these failing system calls. */
static volatile bool running = true;


/* Take care: this function is called from a signal handler, so must be signal
 * safe.  See signal(7) for a list of safely callable functions. */
void kill_socket_server(void)
{
    running = false;
    shutdown(config_socket, SHUT_RDWR);
    shutdown(data_socket, SHUT_RDWR);
}


static bool process_connection(int sock, void *(*process)(void *))
{
    log_message("Receive connection on %d", sock);
    int scon;
    pthread_attr_t attr;
    pthread_t thread;
    return
        TEST_IO(scon = accept(sock, NULL, NULL))  &&
        /* Note that we need to create the spawned threads with DETACHED
         * attribute, otherwise we accumlate internal joinable state information
         * and eventually run out of resources. */
        TEST_PTHREAD(pthread_attr_init(&attr))  &&
        TEST_PTHREAD(pthread_attr_setdetachstate(
            &attr, PTHREAD_CREATE_DETACHED))  &&
        TEST_PTHREAD(pthread_create(&thread, &attr,
            process, (void *)(intptr_t) scon));
}


bool run_socket_server(void)
{
    bool ok = true;
    while (ok  &&  running)
    {
        /* Listen for connection on both configuration and data socket. */
        fd_set readset;
        FD_ZERO(&readset);
        FD_SET(config_socket, &readset);
        FD_SET(data_socket, &readset);
        int maxfd = config_socket > data_socket ? config_socket : data_socket;
        int count = select(maxfd + 1, &readset, NULL, NULL, NULL);

        /* Ignore EINTR returns from select.  We get this on socket shutdown,
         * and it may occur at other times as well. */
        ok = IF(errno != EINTR,
            TEST_IO(count)  &&
            /* Process connections for the readable sockets. */
            IF(FD_ISSET(config_socket, &readset),
                process_connection(config_socket, process_config))  &&
            IF(FD_ISSET(data_socket, &readset),
                process_connection(data_socket, process_data)));
    }

    return ok;
}


static bool create_and_listen(int *sock, int port, const char *name)
{
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };
    return
        TEST_IO(*sock = socket(AF_INET, SOCK_STREAM, 0))  &&
        TEST_IO_(
            bind(*sock, (struct sockaddr *) &sin, sizeof(sin)),
            "Unable to bind to server socket")  &&
        TEST_IO(listen(*sock, 5))  &&
        DO(log_message("%s server listening on port %d", name, port));
}


bool initialise_socket_server(int config_port, int data_port)
{
    return
        create_and_listen(&config_socket, config_port, "Config")  &&
        create_and_listen(&data_socket, data_port, "Data");
}
