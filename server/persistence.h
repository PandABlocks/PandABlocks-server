/* Support for persistence. */

/* The persistence loop periodically writes the entire saved state to a file.
 * The loop is controlled by three timeouts:
 *  - poll_rate     This determines how often we check for changes
 *  - holdoff       This is delay from change detection to performing write
 *  - backoff       This delay is used after writing before resuming polling
 * The idea is to detect changes promptly, give them time to be finished, but to
 * try to avoid writing changes too frequently. */
error__t initialise_persistence(
    const char *file_name,
    int poll_interval, int holdoff_interval, int backoff_interval);

/* This will ensure that the persistence state is updated, so should be called
 * after all active threads have been closed. */
void terminate_persistence(void);
