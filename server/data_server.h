/* Configuration interface. */

/* This should be called in a separate thread for each configuration interface
 * socket connection.  This function will run until the given socket closes. */
error__t process_data_socket(int scon);

/* This starts the background data server task.  Must be called after forking to
 * avoid losing the created thread! */
error__t start_data_server(void);

void terminate_data_server(void);
