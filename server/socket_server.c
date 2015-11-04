/* Socket server core.  Maintains listening socket. */

#include <stdbool.h>
#include <stddef.h>
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
#include "list.h"
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
    error__t (*process)(int sock); // Function for processing socket session
};

/* Listening sockets for configuration and data connections. */
static struct listen_socket config_socket = {
    .sock = -1, .name = "config", .process = process_config_socket };
static struct listen_socket data_socket = {
    .sock = -1, .name = "data",   .process = process_data_socket };



/* This struct is used to pass connection information to a newly created
 * connection thread.  This structure is allocated by the listening thread and
 * released by the connection thread. */
struct session {
    struct list_head list;
    struct timespec ts;             // Time client connection completed
    const struct listen_socket *parent;   // Parent structure
    int sock;                       // Socket for connection
    pthread_t thread;               // Thread id of connection thread
    char name[64];                  // Name of connected client

    /* Two lifetime control flags. */
    unsigned int ref_count;
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


/* All socket sessions are maintained on a list.  We need to protect all
 * maintenance of these connection objects with a lock. */
static pthread_mutex_t session_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK()      ASSERT_PTHREAD(pthread_mutex_lock(&session_lock))
#define UNLOCK()    ASSERT_PTHREAD(pthread_mutex_unlock(&session_lock))

/* Two lists of sessions: those that are active, and those that have
 * completed and need cleanup. */
static LIST_HEAD(active_sessions);
static LIST_HEAD(closed_sessions);


/* Creates a new session object and records it as active. */
static struct session *create_session(void)
{
    struct session *session = calloc(1, sizeof(struct session));
    clock_gettime(CLOCK_REALTIME, &session->ts);
    session->sock = -1;
    session->ref_count = 1;

    LOCK();
    list_add(&session->list, &active_sessions);
    UNLOCK();

    return session;
}


/* Moves an active session to the closed sessions list, waiting to be joined. */
static void close_session(struct session *session)
{
    LOCK();
    if (session->sock != -1)
        close(session->sock);
    session->sock = -1;

    /* Once the running flag is reset it's no longer safe to move these lists
     * around as termination cleanup may be in progress. */
    if (running)
    {
        list_del(&session->list);
        list_add(&session->list, &closed_sessions);
    }
    UNLOCK();
}


/* Unlinks session object and discards it.  Doesn't matter which list it's on,
 * but it must be on a list. */
static void destroy_session(struct session *session)
{
    LOCK();
    list_del(&session->list);
    session->ref_count -= 1;
    if (session->ref_count == 0)
        free(session);
    UNLOCK();
}


static void join_sessions(struct list_head *list)
{
    /* Move the entire list to our workspace under lock. */
    struct list_head work_list;
    LOCK();
    if (list_is_empty(list))
        init_list_head(&work_list);
    else
    {
        __list_add(&work_list, list->prev, list->next);
        init_list_head(list);
    }
    UNLOCK();

    /* Perform a join on each entry in the list. */
    struct list_head *entry = work_list.next;
    while (entry != &work_list)
    {
        struct session *session = container_of(entry, struct session, list);
        error_report(TEST_PTHREAD(pthread_join(session->thread, NULL)));
        entry = entry->next;
        destroy_session(session);
    }
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Implementation of *WHO? command.
 *
 * This is surprisingly tricky! */

static void format_session_item(
    struct config_connection *connection,
    const struct connection_result *result,
    struct session *session)
{
    struct tm tm;
    char message[128];
    gmtime_r(&session->ts.tv_sec, &tm);
    snprintf(message, sizeof(message),
        "%4d-%02d-%02dT%02d:%02d:%02d.%03ldZ %s %s ",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec, session->ts.tv_nsec / 1000000,
        session->parent->name, session->name);
    result->write_many(connection, message);
}


/* We'll use a list of these fellows for a safe copy of the session list! */
struct session_copy {
    struct list_head list;
    struct session *session;
};

/* Creates list of sessions.  Somewhat tricky to implement as the list has to
 * be walked under a lock, but we don't want to hold the lock while generating
 * the output stream. */
void generate_connection_list(
    struct config_connection *connection,
    const struct connection_result *result)
{
    LIST_HEAD(copy_list);

    /* First grab a safe copy of the session list.  This all has to be done
     * under the lock. */
    LOCK();
    list_for_each_entry(struct session, list, session, &active_sessions)
    {
        struct session_copy *copy = malloc(sizeof(struct session_copy));
        list_add(&copy->list, &copy_list);
        copy->session = session;
        session->ref_count += 1;
    }
    UNLOCK();

    /* Now we can walk the list at our leisure and emit the results. */
    list_for_each_entry(struct session_copy, list, copy, &copy_list)
        format_session_item(connection, result, copy->session);

    /* Finally cleanup the list. */
    struct list_head *copy_head = copy_list.next;
    while (copy_head != &copy_list)
    {
        struct session_copy *copy =
            container_of(copy_head, struct session_copy, list);
        LOCK();
        copy->session->ref_count -= 1;
        if (copy->session->ref_count == 0)
            free(copy->session);
        UNLOCK();
        copy_head = copy_head->next;
        free(copy);
    }

    result->write_many_end(connection);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

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
error__t set_timeout(int sock, int timeout, int seconds)
{
    struct timeval timeval = { .tv_sec = seconds, .tv_usec = 0 };
    return TEST_IO(setsockopt(
        sock, SOL_SOCKET, timeout, &timeval, sizeof(timeval)));
}


static void *session_thread(void *context)
{
    struct session *session = context;

    log_message("Client %s %s connected", session->parent->name, session->name);

    error__t error = session->parent->process(session->sock);

    if (error)
        ERROR_REPORT(error, "Client %s %s raised error",
            session->parent->name, session->name);
    log_message("Client %s %s closed", session->parent->name, session->name);

    close_session(session);
    return NULL;
}


static error__t process_session(const struct listen_socket *listen_socket)
{
    struct session *session = create_session();
    session->parent = listen_socket;

    return
        TRY_CATCH(
            TEST_IO_(session->sock = accept(listen_socket->sock, NULL, NULL),
                "Socket accept failed")  ?:
            TRY_CATCH(
                /* Set the transmit timeout so that the server won't be stuck if
                 * the client stops accepting data. */
                set_timeout(session->sock, SO_SNDTIMEO, TRANSMIT_TIMEOUT)  ?:
                get_client_name(session->sock, session->name)  ?:
                TEST_PTHREAD(pthread_create(
                    &session->thread, NULL, session_thread, session)),

            //catch
                /* If thread session fails we have to close the socket. */
                close(session->sock)
            ),

        //catch
            /* If accept or thread creation fail the session is no good. */
            destroy_session(session)
        );
    /* Note that the careful use of TRY_CATCH above is somewhat futile, as if
     * any of the above steps fail we're going to terminate the server anyway.
     * Ah well, maybe this will change... */
}


/* Main action of server: listens for connections and creates a thread for each
 * new session. */
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
        int maxfd = MAX(config_socket.sock, data_socket.sock);
        errno = 0;
        int count = select(maxfd + 1, &readset, NULL, NULL, NULL);
        bool select_ok = count > 0  ||  errno != EINTR;

        /* Perform any pending joins for cleanup. */
        join_sessions(&closed_sessions);

        /* Ignore EINTR returns from select.  We get this on socket shutdown,
         * and it may occur at other times as well. */
        if (select_ok)
            error =
                TEST_IO(count)  ?:
                /* Process connections for the readable sockets. */
                IF(FD_ISSET(config_socket.sock, &readset),
                    process_session(&config_socket))  ?:
                IF(FD_ISSET(data_socket.sock, &readset),
                    process_session(&data_socket));
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


/* Note that this must not be called until after socket_server() has stopped
 * running. */
void terminate_socket_server()
{
    /* First we need to walk the list of all active sessions and force them
     * to close. */
    list_for_each_entry(
        struct session, list, session, &active_sessions)
    {
        LOCK();
        if (session->sock != -1)
            shutdown(session->sock, SHUT_RDWR);
        UNLOCK();
    }

    /* Now wait for everything to by joining all the pending sessions. */
    join_sessions(&active_sessions);
    join_sessions(&closed_sessions);
}
