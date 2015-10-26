/* Socket server core.  Maintains listening socket. */

#include <stdbool.h>
#include <stdlib.h>
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
#include "config_server.h"
#include "data_server.h"

#include "socket_server.h"


/* We have socket timeout on sending to avoid blocking for too long. */
#define TRANSMIT_TIMEOUT    2       // May be too short


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Connection handling. */

/* This structure defines the configuration of a single listening socket.  We
 * have two instances of this structure, one for a configuration socket and one
 * for a data socket -- connections to these sockets have quite different
 * semantics. */
struct listen_socket {
    int sock;                   // Listening socket
    const char *name;           // Config or Data, for logging
    error__t (*process)(int sock); // Function for processing socket connection
};

/* Listening sockets for configuration and data connections. */
static struct listen_socket config_socket = {
    .sock = -1, .name = "config", .process = process_config_socket };
static struct listen_socket data_socket = {
    .sock = -1, .name = "data",   .process = process_data_socket };


/* This struct is used to pass connection information to a newly created
 * connection thread.  This structure is allocated by the listening thread and
 * released by the connection thread. */
struct socket_connection {
    int sock;                   // Socket for connection
    const struct listen_socket *parent;   // Parent structure
    char name[64];              // Name of connected client
};


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
    /* Force the two listening sockets to close.  This will bump
     * run_socket_server out of its listen loop. */
    shutdown(config_socket.sock, SHUT_RDWR);
    shutdown(data_socket.sock, SHUT_RDWR);
}


/* Converts connected socket to a printable identification string. */
static error__t get_client_name(int sock, char *client_name)
{
    struct sockaddr_in name;
    socklen_t namelen = sizeof(name);
    return
        TEST_IO(getpeername(sock, (struct sockaddr *) &name, &namelen))  ?:
        DO(
            /* Consider using inet_ntop() here.  However doesn't include the
             * port number, so probably not so interesting until IPv6 in use. */
            uint8_t *ip = (uint8_t *) &name.sin_addr.s_addr;
            sprintf(client_name, "%u.%u.%u.%u:%u",
                ip[0], ip[1], ip[2], ip[3], ntohs(name.sin_port))
        );
}


/* Sets the specified timeout in seconds on sock.  timeout must be one of
 * SO_RCVTIMEO or SO_SNDTIMEO. */
static error__t set_timeout(int sock, int timeout, int seconds)
{
    struct timeval timeval = { .tv_sec = seconds, .tv_usec = 0 };
    return TEST_IO(setsockopt(
        sock, SOL_SOCKET, timeout, &timeval, sizeof(timeval)));
}


static void *connection_thread(void *context)
{
    struct socket_connection *connection = context;

    log_message("Client %s %s connected",
        connection->parent->name, connection->name);
    error__t error = connection->parent->process(connection->sock);
    if (error)
        ERROR_REPORT(error, "Client %s %s raised error",
            connection->parent->name, connection->name);
    log_message("Client %s %s closed",
        connection->parent->name, connection->name);

    free(connection);
    return NULL;
}


static error__t process_connection(const struct listen_socket *listen_socket)
{
    struct socket_connection *connection =
        malloc(sizeof(struct socket_connection));
    connection->parent = listen_socket;

    pthread_attr_t attr;
    pthread_t thread;
    return
        /* Note that we need to create the spawned threads with DETACHED
         * attribute, otherwise we accumlate internal joinable state information
         * and eventually run out of resources. */
        TEST_PTHREAD(pthread_attr_init(&attr))  ?:
        TEST_PTHREAD(
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED))  ?:
        TRY_CATCH(
            TEST_IO(connection->sock =
                accept(listen_socket->sock, NULL, NULL))  ?:
            TRY_CATCH(
                /* Set the transmit timeout so that the server won't be stuck if
                 * the client stops accepting data. */
                set_timeout(connection->sock, SO_SNDTIMEO, TRANSMIT_TIMEOUT)  ?:
                get_client_name(connection->sock, connection->name)  ?:
                TEST_PTHREAD(pthread_create(
                    &thread, &attr, connection_thread, connection)),

            // if thread creation fails
                close(connection->sock)
            ),

        // accept fails or thread creation fails
            free(connection)
        );
    /* Note that the careful use of TRY_CATCH above is somewhat futile, as if
     * any of the above steps fail we're going to terminate the server anyway.
     * Ah well, maybe this will change... */
}


/* Main action of server: listens for connections and creates a thread for each
 * new connection. */
error__t run_socket_server(void)
{
    error__t error = ERROR_OK;
    while (!error  &&  running)
    {
        /* Listen for connection on both configuration and data socket. */
        fd_set readset;
        FD_ZERO(&readset);
        FD_SET(config_socket.sock, &readset);
        FD_SET(data_socket.sock, &readset);
        int maxfd = config_socket.sock > data_socket.sock ?
            config_socket.sock : data_socket.sock;
        errno = 0;
        int count = select(maxfd + 1, &readset, NULL, NULL, NULL);

        /* Ignore EINTR returns from select.  We get this on socket shutdown,
         * and it may occur at other times as well. */
        if (count > 0  ||  errno != EINTR)
            error =
                TEST_IO(count)  ?:
                /* Process connections for the readable sockets. */
                IF(FD_ISSET(config_socket.sock, &readset),
                    process_connection(&config_socket))  ?:
                IF(FD_ISSET(data_socket.sock, &readset),
                    process_connection(&data_socket));
    }

    return error;
}


/* Creates listening socket on the given port. */
static error__t create_and_listen(
    struct listen_socket *listen_socket, unsigned int port)
{
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };
    return
        TEST_IO(listen_socket->sock = socket(AF_INET, SOCK_STREAM, 0))  ?:
        TEST_IO_(
            bind(listen_socket->sock, (struct sockaddr *) &sin, sizeof(sin)),
            "Unable to bind to server socket")  ?:
        TEST_IO(listen(listen_socket->sock, 5))  ?:
        DO(log_message("Listening on port %d for %s",
            port, listen_socket->name));
}


error__t initialise_socket_server(
    unsigned int config_port, unsigned int data_port)
{
    return
        create_and_listen(&config_socket, config_port)  ?:
        create_and_listen(&data_socket, data_port);
}
