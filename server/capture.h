/* Definitions of methods for bit_out and pos_out classes. */

struct class;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Class initialisation. */

/* Common class init function. */
void bit_pos_out_init(unsigned int count, void **class_data);

/* Register file parsing for bit_out. */
error__t bit_out_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line);

/* Register file parsing for pos_out. */
error__t pos_out_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line);

/* Common validation function after register processing. */
error__t bit_pos_out_validate(struct class *class);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Class value access (classes are read only). */

/* Refresh methods used to bring cached _out values up to date with values read
 * from hardware. */
void bit_out_refresh(struct class *class, unsigned int number);
void pos_out_refresh(struct class *class, unsigned int number);

/* Reading of cached value. */
uint32_t bit_out_read(struct class *class, unsigned int number);
uint32_t pos_out_read(struct class *class, unsigned int number);

/* Change set reporting. */
void bit_out_change_set(
    struct class *class, const uint64_t report_index[], bool changes[]);
void pos_out_change_set(
    struct class *class, const uint64_t report_index[], bool changes[]);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Attributes. */

/* .CAPTURE attribute, used to configure capture for this field. */
error__t bit_out_capture_format(
    struct class *class, void *data, unsigned int number,
    char result[], size_t length);
error__t pos_out_capture_format(
    struct class *class, void *data, unsigned int number,
    char result[], size_t length);

error__t bit_out_capture_put(
    struct class *class, void *data, unsigned int number, const char *value);
error__t pos_out_capture_put(
    struct class *class, void *data, unsigned int number, const char *value);

/* .CAPTURE_INDEX attribute, returns encoding of capture index. */
error__t bit_out_index_format(
    struct class *class, void *data, unsigned int number,
    char result[], size_t length);
error__t pos_out_index_format(
    struct class *class, void *data, unsigned int number,
    char result[], size_t length);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Capture enumeration. */

/* *CAPTURE? implementation: returns list of all captured fields. */
void report_capture_list(const struct connection_result *result);

/* *BITSn? implementation, reports bit names in specific capture block. */
void report_capture_bits(
    const struct connection_result *result, unsigned int bit);
