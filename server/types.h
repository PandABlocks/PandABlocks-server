/* Support for types. */


struct field_type;


struct type_context {
    void *type_data;
    unsigned int number;
};


/* A field type is used to help in the interpretation of field data and can
 * optionally provide extra attributes. */
struct type_access {
    /* This creates and initialises any type specific data needed. */
    void *(*init_type)(unsigned int count);

    /* This converts a string to a writeable integer. */
    error__t (*parse)(
        const struct type_context *context,
        const char *string, unsigned int *value);

    /* This formats the value into a string according to the type rules. */
    void (*format)(
        const struct type_context *context,
        unsigned int value, char string[], size_t length);

    /* Also have interfaces for attributes. */
    // ...
};


error__t get_type_access(
    const struct field_type *type, const struct type_access **access);

error__t lookup_type(const char *name, const struct field_type **type);

error__t initialise_types(void);

void terminate_types(void);
