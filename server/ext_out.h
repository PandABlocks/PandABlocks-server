/* ext_out field class */

struct ext_out;
struct capture_info;

error__t initialise_ext_out(void);
void terminate_ext_out(void);

/* Used to implement *CAPTURE= method. */
void reset_ext_out_capture(struct ext_out *ext_out);

/* Interrogates capture status and returns identification string. */
bool get_ext_out_capture(struct ext_out *ext_out, const char **string);

/* Returns full capture info for field, if captured. */
unsigned int get_ext_out_capture_info(
    struct ext_out *ext_out, struct capture_info *capture_info);

/* Returns capture info for the samples field and whether it is currently
 * configured for capture. */
bool get_samples_capture_info(struct capture_info *capture_info);

extern const struct class_methods ext_out_class_methods;
