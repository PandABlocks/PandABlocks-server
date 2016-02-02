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

#if 0
/* This is called at the completion of capture arming to initiate reading from
 * the hardware. */
void start_data_capture(void);

/* This can be called at any time.  Data capture to existing and new clients
 * must be aborted, if necessary.  The function data_clients_complete() will be
 * called on successful completion. */
void reset_data_capture(void);

/* Returns numbers of connected clients. */
void get_data_capture_counts(unsigned int *connected, unsigned int *reading);
#endif

/* Support functions for capture.  The function lock_capture_disabled() will
 * only succeed if capture is not in progress, in which case
 * unlock_capture_disabled() must be called to release the lock. */
error__t lock_capture_disabled(void);
void unlock_capture_disabled(void);

/* A number of parameters can only be set while capture is not in progress.
 * This macro supports locking capture while setting a parameter. */
#define IF_CAPTURE_DISABLED(action) \
    lock_capture_disabled()  ?: \
    DO_FINALLY(action, unlock_capture_disabled())



/* User callable capture control methods. */
error__t arm_capture(void);
error__t disarm_capture(void);
error__t reset_capture(void);
error__t capture_status(struct connection_result *result);
