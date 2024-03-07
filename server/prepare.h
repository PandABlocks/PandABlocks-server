/* Data capture preparation. */


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Capture option line parsing. */

enum data_format {
    DATA_FORMAT_UNFRAMED,   // Raw and unframed data
    DATA_FORMAT_FRAMED,     // Framed binary data
    DATA_FORMAT_BASE64,     // Base 64 formatted data
    DATA_FORMAT_ASCII,      // ASCII numerical data
};

enum data_process {
    DATA_PROCESS_RAW,       // Unprocessed raw captured data
    DATA_PROCESS_SCALED,    // Floating point scaled numbers
};

/* Data capture and processing options. */
struct data_options {
    enum data_format data_format;   // How data is transported to the client
    enum data_process data_process; // How data is processed
    bool omit_header;       // With this option the header will be omitted
    bool omit_status;       // This option will omit *all* status reports
    bool one_shot;          // Connection is closed after one experiment
    bool xml_header;        // Header is sent in XML format
};


/* Parses option line from connection request. */
error__t parse_data_options(const char *line, struct data_options *options);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Interface data request and associated header. */

struct captured_fields;
struct data_capture;
struct data_options;
struct buffered_file;

/* Sends header describing current set of data options.  Returns false if
 * writing to the connection fails. */
bool send_data_header(
    const struct captured_fields *fields,
    const struct data_capture *capture,
    const struct data_options *options,
    struct buffered_file *file, uint64_t lost_samples,
    struct timespec *pcap_arm_tsp, struct timespec *pcap_start_tsp,
    bool pcap_hw_ts_offset_ns_valid, int64_t pcap_hw_ts_offset_ns);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data preparation. */

struct capture_index;

struct capture_group {
    unsigned int count;
    struct capture_info **outputs;
};


/* This contains all the information required to process the data capture
 * stream, is prepared by prepare_captured_fields() and finally processed by
 * prepare_data_capture(). */
struct captured_fields {
    /* Special field.  This may be repeated in the unscaled capture group. */
    struct capture_info *sample_count;

    /* Other fields grouped by processing. */
    struct capture_group scaled32;
    struct capture_group averaged;
    struct capture_group std_dev;
    struct capture_group scaled64;
    struct capture_group unscaled;
};


/* Call to extract set of captured fields, then call prepare_data_capture. */
const struct captured_fields *prepare_captured_fields(void);
