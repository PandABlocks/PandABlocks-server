/* pos_out field class. */

struct pos_out;
struct capture_info;

error__t initialise_pos_out(void);
void terminate_pos_out(void);

/* Update cached pos_out values up to date with values read from hardware. */
void do_pos_out_refresh(uint64_t change_index);

/* Used to implement *CAPTURE= method. */
void reset_pos_out_capture(struct pos_out *pos_out, unsigned int number);

/* Interrogates capture status and returns identification string. */
bool get_pos_out_capture(
    struct pos_out *pos_out, unsigned int number, const char **string);

/* Returns full capture info for field, returning number of fields captured. */
unsigned int get_pos_out_capture_info(
    struct pos_out *pos_out, unsigned int number,
    struct capture_info *capture_info);

extern const struct class_methods pos_out_class_methods;
