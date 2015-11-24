/* Time class. */

void time_init(unsigned int count, void **class_data);

void time_change_set(
    struct class *class, const uint64_t report_index[], bool changes[]);

error__t time_get(
    struct class *class, unsigned int number,
    struct connection_result *result);

error__t time_put(
    struct class *class, unsigned int number, const char *string);

/* Attribute methods. */
error__t time_raw_format(
    struct class *class, void *data, unsigned int number,
    char result[], size_t length);
error__t time_raw_put(
    struct class *class, void *data, unsigned int number, const char *string);
error__t time_scale_format(
    struct class *class, void *data, unsigned int number,
    char result[], size_t length);
error__t time_scale_put(
    struct class *class, void *data, unsigned int number, const char *value);
