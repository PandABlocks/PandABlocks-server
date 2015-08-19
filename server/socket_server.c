/* Socket server core.  Maintains listening socket. */

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "error.h"
#include "hardware.h"
#include "socket_server.h"


static int server_socket = -1;


void kill_socket_server(void)
{
    IGNORE(TEST_IO(shutdown(server_socket, SHUT_RDWR)));
}


static void *process_connection(void *context)
{
    int scon = (int) (intptr_t) context;
    log_message("Connection on %d", scon);
    close(scon);
    return NULL;
}


bool run_socket_server(void)
{
    /* Note that we need to create the spawned threads with DETACHED attribute,
     * otherwise we accumlate internal joinable state information and eventually
     * run out of resources. */
    pthread_attr_t attr;
    ASSERT_PTHREAD(pthread_attr_init(&attr));
    ASSERT_PTHREAD(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED));

    int scon;
    pthread_t thread;
    bool ok = true;
    while (ok  &&
        TEST_IO_(scon = accept(server_socket, NULL, NULL),
            "Server interrupted"))
    {
        ok = TEST_PTHREAD(pthread_create(&thread, &attr,
            process_connection, (void *)(intptr_t) scon));
    }

    return ok;
}


bool initialise_socket_server(int port)
{
    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };
    return
        TEST_IO(server_socket = socket(AF_INET, SOCK_STREAM, 0))  &&
        TEST_IO_(
            bind(server_socket, (struct sockaddr *) &sin, sizeof(sin)),
            "Unable to bind to server socket")  &&
        TEST_IO(listen(server_socket, 5))  &&
        DO(log_message("Server listening on port %d", port));
}
