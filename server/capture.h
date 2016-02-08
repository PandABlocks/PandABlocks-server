/* Data capture control. */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data capture interface. */

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
void register_output(
    const struct output *output, const char *name,
    enum output_class output_class, unsigned int capture_ix[2]);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data capture readout. */

enum data_format {
    DATA_FORMAT_RAW,        // Raw and unframed data
    DATA_FORMAT_FRAMED,     // Framed binary data
    DATA_FORMAT_BASE64,     // Base 64 formatted data
    DATA_FORMAT_ASCII,      // ASCII numerical data
};

enum data_process {
    DATA_PROCESS_RAW,       // Unprocessed raw captured data
    DATA_PROCESS_UNSCALED,  // Integer numbers
    DATA_PROCESS_SCALED,    // Floating point scaled numbers
};

/* Data capture options. */
struct data_options {
    enum data_format data_format;
    enum data_process data_process;
    bool omit_header;
    bool omit_status;
    bool one_shot;
};

/* This structure encapsulates everything needed to process captured data. */
struct data_capture;


/* Called just before arming hardware to prepare system for data capture.  The
 * data capture state will remain valid while lock_capture_disable() returns
 * success. */
error__t prepare_data_capture(const struct data_capture **capture);


/* Returns size of single raw data capture length in bytes. */
size_t get_raw_sample_length(const struct data_capture *capture);

/* Returns size of converted binary data with current output parameters. */
size_t get_binary_sample_length(
    const struct data_capture *capture, struct data_options *options);


/* Parses option line from connection request. */
error__t parse_data_options(const char *line, struct data_options *options);

/* Sends header describing current set of data options.  Returns false if
 * writing to the connection fails. */
bool send_data_header(
    const struct data_capture *capture, struct data_options *options,
    struct buffered_file *file, uint64_t lost_samples);

/* Converts the given number of samples from raw to binary format.  The sizes of
 * the input and output buffers is determined by the corresponding raw and
 * binary sample lengths. */
void convert_raw_data_to_binary(
    const struct data_capture *capture, struct data_options *options,
    unsigned int sample_count, const void *input, void *output);

/* Binary and raw format data need no further processing, but ASCII data needs
 * to be converted according to the appropriate settings.  This function
 * transmits using the appropriate file buffer, returning false if there's a
 * communication problem. */
bool send_binary_as_ascii(
    const struct data_capture *capture, struct data_options *options,
    struct buffered_file *file, unsigned int sample_count, const void *data);
