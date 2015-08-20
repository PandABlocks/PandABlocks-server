/* Interface to socket server. */

/* Initialises the socket server but doesn't run the server yet. */
bool initialise_socket_server(int config_port, int data_port);

/* To be called after successful initialisation and daemonisation.  Runs
 * processing server socket connections until interrupted. */
bool run_socket_server(void);

/* Interrupts the socket server, run_socket_server() will return after this has
 * been called.  Note that this function is signal safe. */
void kill_socket_server(void);
