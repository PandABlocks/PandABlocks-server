/* Implementation of enums. */

struct type_context;
struct type_attr_context;

/* Starts the loading of an enumeration. */
error__t enum_init(const char **string, unsigned int count, void **type_data);

/* Called during shutdown to release allocated resources. */
void enum_destroy(void *type_data, unsigned int count);

/* Adds a single enumeration label to the enumeration set. */
error__t enum_add_label(void *type_data, const char *string);

/* Parses valid enumeration into corresponding value, otherwise error. */
error__t enum_parse(
    const struct type_context *context,
    const char *string, unsigned int *value);

/* Formats valid value into enumeration string, otherwise error. */
error__t enum_format(
    const struct type_context *context,
    unsigned int value, char string[], size_t length);

/* Returns list of enumeration values and strings. */
error__t enum_labels_get(
    const struct type_attr_context *context,
    const struct connection_result *result);
