/* Support for types. */


struct type;
struct type_data;


struct type_context {
    unsigned int number;
    const struct type *type;
    struct type_data *type_data;
};


/* This converts a string to a writeable integer. */
error__t type_parse(
    const struct type_context *context,
    const char *string, unsigned int *value);

/* This formats the value into a string according to the type rules. */
error__t type_format(
    const struct type_context *context,
    unsigned int value, char string[], size_t length);


/* Parses type description in name and returns type and type_data. */
error__t create_type(
    const char *name, const struct type **type, struct type_data **type_data);

void destroy_type(const struct type *type, struct type_data *type_data);


error__t initialise_types(void);

void terminate_types(void);
