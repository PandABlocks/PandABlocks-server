/* Configuration interface. */

/* This should be called in a separate thread for each configuration interface
 * socket connection.  This function will run until the given socket closes. */
void process_data_socket(int scon);
