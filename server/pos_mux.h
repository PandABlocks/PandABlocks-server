/* Definitions for pos_mux */

struct field;

/* Given an array of numbers uses format_field_name to add entries to the given
 * enumeration. */
error__t add_mux_indices(
    struct enumeration *lookup, struct field *field,
    const unsigned int array[], size_t length);

/* Adds given array of field names to position mux enumeration. */
error__t add_pos_mux_index(
    struct field *field, const unsigned int array[], size_t length);

/* *POSITIONS? implementation, reports all pos_mux position names. */
void report_capture_positions(struct connection_result *result);

error__t initialise_pos_mux(void);
void terminate_pos_mux(void);

extern const struct class_methods pos_mux_class_methods;
