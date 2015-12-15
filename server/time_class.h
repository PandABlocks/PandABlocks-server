/* Time class. */

/* Shared with type class implementation. */
enum time_scale { TIME_MINS, TIME_SECS, TIME_MSECS, TIME_USECS, };

/* Parses string using given scaling into 48-bit time. */
error__t time_class_parse(
    const char *string, enum time_scale scale, uint64_t *result);

/* Formats time using given scaling. */
error__t time_class_format(
    uint64_t value, enum time_scale scale, char result[], size_t length);

/* Converts scale into units string. */
error__t time_class_units_format(
    enum time_scale scale, char result[], size_t length);

/* Parses units string into scale. */
error__t time_class_units_parse(const char *string, enum time_scale *scale);

extern const struct class_methods time_class_methods;
