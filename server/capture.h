/* Data capture control. */

/* Data capture options. */
struct data_options {
};

/* This structure encapsulates everything needed to process captured data. */
struct data_capture;


/* Called just before arming hardware to prepare system for data capture.  The
 * data capture state will remain valid while lock_capture_disable() returns
 * success. */
struct data_capture *prepare_data_capture(void);


/* Returns size of single raw data capture length. */
size_t get_raw_row_length(struct data_capture *capture);

/* Parses option line from connection request. */
error__t parse_data_options(const char *line, struct data_options *options);

/* Sends header describing current set of data options.  Returns false if
 * writing to the connection fails. */
bool send_data_header(
    struct data_capture *capture, struct data_options *options,
    struct buffered_file *file);

/* Computes output block according to selected data options and current data
 * capture configuration, returns number of bytes written to output buffer and
 * number of bytes consumed from input.  Note that input must be at least one
 * raw row length. */
size_t compute_output_data(
    struct data_capture *capture, struct data_options *options,
    const void *input, size_t input_length, size_t *input_consumed,
    void *output, size_t output_length);
