/* Data capture control. */

extern const struct class_methods software_class_methods;

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
