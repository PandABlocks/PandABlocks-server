/* Definitions of methods for bit_out and pos_out classes and bit_mux and
 * pos_mux types. */

struct class;
struct field;
struct enumeration;


error__t initialise_output(void);

void terminate_output(void);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data capture API. */

struct output;


/* This is used to record the various capture processing options that may have
 * been selected for this output. */
enum capture_mode {
    CAPTURE_OFF,            // Output not being captured
    CAPTURE_UNSCALED,       // 32-bit value with no processing
    CAPTURE_SCALED32,       // 32-bit value with scaling
    CAPTURE_SCALED64,       // 64-bit value with scaling
    CAPTURE_ADC_MEAN,       // 64-bit value with mean and scaling
    CAPTURE_TS_NORMAL,      // 64-bit timestamp without offset
    CAPTURE_TS_OFFSET,      // 64-bit timestamp with offset correction

    CAPTURE_MODE_COUNT,     // Number of capture mode enum values
};


/* At the same time as the capture processing is set we also need to instruct
 * the hardware about our framing setup. */
enum framing_mode {
    FRAMING_TRIGGER,        // Framing mode not selected, capture on trigger
    FRAMING_FRAME,          // Normal framing mode
    FRAMING_SPECIAL,        // Framing mode with special option flag set
};


/* Scaling parameters, if appropriate. */
struct scaling {
    double scale;
    double offset;
};


struct capture_info {
    bool scaled;
    enum framing_mode framing_mode;
    enum capture_mode capture_mode;
    const char *capture_string;
    struct scaling scaling;
    char units[MAX_NAME_LENGTH];
};

/* Returns all current information about the selected output source: how it's
 * configured for capture, any scaling settings, the units string, and the
 * capture enumeration selection.
 *     If the units buffer is too short, the units string will be silently
 * truncated here.  If the returned capture_mode is CAPTURE_OFF then the
 * remaining fields are not filled in. */
enum capture_mode get_capture_info(
    struct output *output, unsigned int number, struct capture_info *info);


/* Disables capture for the specified output. */
void reset_output_capture(struct output *output, unsigned int number);

/* If this output is configured for capture returns true together with the
 * capture enumeration selection. */
bool get_capture_enabled(
    struct output *output, unsigned int number, const char **capture);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Capture enumeration. */

/* Update cached pos_out values up to date with values read from hardware. */
void do_pos_out_refresh(uint64_t change_index);


/* Registers ext_out and pos_out values as a capture source. */
struct ext_out;
struct pos_out;
error__t register_ext_out(struct ext_out *ext_out, struct field *field);
error__t register_pos_out(struct pos_out *pos_out, struct field *field);
