/* Data capture control. */

enum capture_state {
    CAPTURE_IDLE,       // No capture in progress, can update state
    CAPTURE_ACTIVE,     // Capture in progress
    CAPTURE_CLOSING,    // Capture complete, clients still taking data
};

/* Returns current capture state and locks the capture state.  MUST NOT be held
 * for long, and release_capture_state() must be called. */
enum capture_state lock_capture_state(void);
/* This must be called shortly after calling get_capture_state(). */
void release_capture_state(void);


/* User callable capture control methods. */
error__t arm_capture(void);
error__t disarm_capture(void);
error__t reset_capture(void);
error__t capture_status(struct connection_result *result);
error__t capture_waiting(struct connection_result *result);


#define WITH_CAPTURE_STATE(state, result) \
    ( { \
        enum capture_state state = lock_capture_state(); \
        DO_FINALLY(result, release_capture_state()); \
    } )
