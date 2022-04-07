/* Data capture control. */


/* The maximum possible number of captures.  This is massively pessimistic, but
 * allows for all possible pos_out capture options. */
#define MAX_CAPTURE_COUNT \
    (MAX_POS_OUT_CAPTURE * POS_BUS_COUNT + MAX_EXT_OUT_CAPTURE * EXT_BUS_COUNT)


struct data_capture;
struct captured_fields;
struct data_options;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Data capture readout. */


/* Called just before arming hardware to prepare system for data capture.  The
 * data capture state will remain valid while lock_capture_disable() returns
 * success. */
error__t prepare_data_capture(
    const struct captured_fields *fields,
    const struct data_capture **capture);

/* Returns size of single raw data capture length in bytes. */
size_t get_raw_sample_length(const struct data_capture *capture);

/* Returns size of converted binary data with current output parameters. */
size_t get_binary_sample_length(
    const struct data_capture *capture, const struct data_options *options);

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

/* If averaged fields are present, but sample count is not requested, it
 * will be captured, but not added to any group. This returns true if so */
bool sample_count_is_anonymous(const struct data_capture *capture);
