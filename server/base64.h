/* Base 64 encode and decode functions. */

/* Converts binary data to base 64.  The output buffer must be at least
 * ceiling(4/3*length)+1 bytes long. */
size_t base64_encode(const void *data, size_t length, char out[]);

/* Converts a binary data length to the corresponding encoded length: does not
 * include the trailing null character. */
#define BASE64_ENCODE_LENGTH(length)    ((4*(length)+2)/3)

/* Return codes from decoding. */
enum base64_status {
    BASE64_STATUS_OK,
    BASE64_STATUS_MALFORMED,
    BASE64_STATUS_OVERRUN,
};

/* Converts a base64 string into binary, or returns an error code if the string
 * is malformed or the output buffer is overrun. */
enum base64_status base64_decode(
    const char *string, void *data, size_t length, size_t *converted);

/* Must be called once before calling base64_decode(). */
void initialise_base64(void);

/* Converts decode error code into string. */
const char *base64_error_string(enum base64_status status);
