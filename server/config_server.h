/* Configuration interface. */

/* This should be called in a separate thread for each configuration interface
 * socket connection.  This function will run until the given socket closes. */
void process_config_socket(int scon);


/* This structure holds the local state for a config socket connection. */
struct config_connection {
    FILE *stream;               // Stream connection to client
};
