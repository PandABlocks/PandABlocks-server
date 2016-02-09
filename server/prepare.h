/* Data capture preparation. */

/* This is a safe upper bound on the number of outputs, and is useful for a
 * number of temporary buffers. */
#define MAX_OUTPUT_COUNT    64



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Interface for registering captured data sources. */

struct output;


/* When registering output sources and preparing data capture we need to treat a
 * number of output sources specially. */
enum output_class {
    OUTPUT_CLASS_NORMAL,       // Normal output source
    OUTPUT_CLASS_TIMESTAMP,    // Timestamp, may need special offset handling
    OUTPUT_CLASS_TS_OFFSET,    // Timestamp offset
    OUTPUT_CLASS_ADC_COUNT,    // ADC sample count for average calculations
};


/* This function is called during system startup to register output sources.  Up
 * to 2 separate capture indices can be registered for each source. */
void register_outputs(
    const struct output *output, unsigned int count,
    enum output_class output_class, const unsigned int capture_index[][2]);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Interface data request and associated header. */

struct captured_fields;
struct data_capture;
struct data_options;


/* Sends header describing current set of data options.  Returns false if
 * writing to the connection fails. */
bool send_data_header(
    const struct captured_fields *fields,
    const struct data_capture *capture,
    const struct data_options *options,
    struct buffered_file *file, uint64_t lost_samples);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data preparation. */

struct output_field;
struct scaling;


struct capture_group {
    unsigned int count;
    struct output_field **outputs;
};

enum ts_capture {
    TS_IGNORE,
    TS_CAPTURE,
    TS_OFFSET,
};

/* This contains all the information required to process the data capture
 * stream, and will be preprocessed by ??? ... */
struct captured_fields {
    /* Timestamp capture status, needs special handling. */
    enum ts_capture ts_capture;
    /* Special fields.  These may be repeated in the unscaled capture group. */
    struct output_field *timestamp;
    struct output_field *offset;
    struct output_field *adc_count;
    /* Other fields grouped by processing. */
    struct capture_group unscaled;
    struct capture_group scaled32;
    struct capture_group scaled64;
    struct capture_group adc_mean;
};


/* Call to extract set of captured fields, then call prepare_data_capture. */
const struct captured_fields *prepare_captured_fields(void);


/* This is called on each captured output to extract the information needed for
 * capture processing. */
enum framing_mode get_output_info(
    const struct output_field *output,
    unsigned int capture_index[2], struct scaling *scaling);
