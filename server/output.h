/* Definitions of methods for bit_out and pos_out classes and bit_mux and
 * pos_mux types. */

error__t initialise_output(void);
void terminate_output(void);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Interface for registering captured data sources. */

struct ext_out;
struct pos_out;
struct field;


/* Registers position bus capture control field. */
error__t register_pos_out(
    struct pos_out *pos_out, struct field *field, unsigned int count);

/* Registers extension bus capture control field.  The 'ext_out samples' field
 * needs to be identified for special treatment during data capture. */
error__t register_ext_out(struct ext_out *ext_out, struct field *field);


/* *CAPTURE= implementation: resets all capture settings. */
void reset_capture_list(void);

/* *CAPTURE? implementation: returns list of all captured fields. */
void report_capture_list(struct connection_result *result);

/* *CAPTURE.*? implementation: returns list of all fields which can be
 * selected for capture. */
void report_capture_labels(struct connection_result *result);


enum capture_mode {
    CAPTURE_MODE_SCALED32,          // int32 with scaling
    CAPTURE_MODE_SCALED64,          // int64 with scaling
    CAPTURE_MODE_AVERAGE,           // int64 with scaling and averaging
    CAPTURE_MODE_UNSCALED,          // uint32 with no scaling option

    CAPTURE_MODE_COUNT              // Number of capture mode enum values
};


struct capture_index {
    unsigned int index[2];          // Capture index or indices for data
};


/* All the information needed to captured and process a captured field. */
struct capture_info {
    struct capture_index capture_index;
    enum capture_mode capture_mode; // Underlying data type of captured field
    const char *field_name;         // Name of captured field
    const char *capture_string;     // Identifies what is captured
    /* The following fields are invalid if capture mode is unscaled. */
    double scale;                   // Scaling factor
    double offset;                  // Scaling offset
    char units[MAX_NAME_LENGTH];    // Scaling units string
};


/* Interator for generating list of captured values.  Complicated a little by
 * the fact that a single captured field can generate multiple captured values!
 *    Use by setting *ix to 0 before first call and calling repeatedly until
 * false is returned.  *captured is set to the number of capture_info[] entries
 * that have been written, space must be allowed for at least three. */
bool iterate_captured_values(
    unsigned int *ix, unsigned int *captured,
    struct capture_info capture_info[]);
