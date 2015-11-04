/* Interface to socket server. */

struct config_connection;
struct connection_result;

/* Initialises the socket server but doesn't run the server yet. */
error__t initialise_socket_server(
    unsigned int config_port, unsigned int data_port);

/* Ensures all connections are terminated and releases any resources. */
void terminate_socket_server(void);

/* To be called after successful initialisation and daemonisation.  Runs
 * processing server socket connections until interrupted or a serious error
 * occurs. */
error__t run_socket_server(void);

/* Interrupts the socket server, run_socket_server() will return after this has
 * been called.  Note that this function is signal safe. */
void kill_socket_server(void);

/* Sets socket timeout. */
error__t set_timeout(int sock, int timeout, int seconds);

/* Generates list of currently active connections. */
void generate_connection_list(
    struct config_connection *connection,
    const struct connection_result *result);
