/* Configuration interface. */

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

/* This is called at the completion of capture arming to initiate reading from
 * the hardware. */
void start_data_capture(void);

/* This can be called at any time.  Data capture to existing and new clients
 * must be aborted, if necessary.  The function data_clients_complete() will be
 * called on successful completion. */
void reset_data_capture(void);

/* Returns numbers of connected clients. */
void get_data_capture_counts(unsigned int *connected, unsigned int *reading);
