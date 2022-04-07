/* ext_out field class */

/* Maximum possible number of pos_out capture_info entries that might be written
 * when calling get_ext_out_capture_info(). */
#define MAX_EXT_OUT_CAPTURE     2

struct ext_out;
struct capture_info;

error__t initialise_ext_out(void);
void terminate_ext_out(void);

/* Used to implement *CAPTURE= method. */
void reset_ext_out_capture(struct ext_out *ext_out);

/* Interrogates capture status and returns identification string. */
void report_ext_out_capture(
    struct ext_out *ext_out, const char *field_name,
    struct connection_result *result);

/* Returns full capture info for field, if captured. */
unsigned int get_ext_out_capture_info(
    struct ext_out *ext_out, struct capture_info *capture_info);

/* Unconditionally returns capture info for the samples field.  If capture has
 * been requested this will be reported separately through
 * get_ext_out_capture_info in the normal way. */
void get_samples_capture_info(struct capture_info *capture_info);

/* Returns true if the appropriate PCAP fields have been defined, otherwise we
 * are not ready to perform any capture operations. */
bool check_pcap_valid(void);

extern const struct class_methods ext_out_class_methods;
