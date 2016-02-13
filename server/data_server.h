/* Configuration interface. */


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation and shutdown. */

/* This should be called in a separate thread for each configuration interface
 * socket connection.  This function will run until the given socket closes. */
error__t process_data_socket(int scon);

/* Data server and processing initialisation. */
error__t initialise_data_server(void);

/* This starts the background data server task.  Must be called after forking to
 * avoid losing the created thread! */
error__t start_data_server(void);

/* Terminating the data server needs to be done in two stages to avoid delays.
 * The _early call ensures that none of the data clients are blocked, and the
 * final call must not be called until the sockets have cleared. */
void terminate_data_server_early(void);
void terminate_data_server(void);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* User interface command support. */

/* User callable capture control methods. */
error__t arm_capture(void);
error__t disarm_capture(void);
error__t reset_capture(void);
error__t capture_status(struct connection_result *result);
