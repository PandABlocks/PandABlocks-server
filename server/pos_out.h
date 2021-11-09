/* pos_out field class. */

struct pos_out;
struct capture_info;

error__t initialise_pos_out(void);
void terminate_pos_out(void);

/* Update cached pos_out values up to date with values read from hardware. */
void do_pos_out_refresh(uint64_t change_index);

/* Returns list of available capture options. */
error__t get_capture_options(struct connection_result *result);

/* Used to implement *CAPTURE= method. */
void reset_pos_out_capture(struct pos_out *pos_out, unsigned int number);

/* If capture enabled reports capture status using given field name. */
void report_pos_out_capture(
    struct pos_out *pos_out, unsigned int number,
    const char *field_name, struct connection_result *result);

/* Returns full capture info for field, returning number of fields captured. */
unsigned int get_pos_out_capture_info(
    struct pos_out *pos_out, unsigned int number,
    struct capture_info *capture_info);

extern const struct class_methods pos_out_class_methods;
