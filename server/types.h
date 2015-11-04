/* Support for types. */


struct type;
struct attr;


/* Context required to parse or format an ordinary field. */
struct type_context {
    unsigned int number;
    const struct type *type;
    void *type_data;
};


/* Context require to read or write a field attribute. */
struct type_attr_context {
    unsigned int number;
    struct config_connection *connection;
    const struct field *field;
    void *type_data;
    const struct attr *attr;
};


/* This converts a string to a writeable integer. */
error__t type_parse(
    const struct type_context *context,
    const char *string, unsigned int *value);

/* This formats the value into a string according to the type rules. */
error__t type_format(
    const struct type_context *context,
    unsigned int value, char string[], size_t length);

/* Returns list of attributes for the specified type. */
error__t type_attr_list_get(
    const struct type *type,
    struct config_connection *connection,
    const struct connection_result *result);

/* Returns attribute with the given name. */
error__t type_lookup_attr(
    const struct type *type, const char *name,
    const struct attr **attr);

/* Reads identified attribute. */
error__t type_attr_get(
    const struct type_attr_context *context,
    const struct connection_result *result);

/* Writes identified attribute. */
error__t type_attr_put(
    const struct type_attr_context *context, const char *value);


/* Parses type description in name and returns type and type_data. */
error__t create_type(
    const char *name, bool forced, unsigned int count,
    const struct type **type, void **type_data);

void destroy_type(const struct type *type, void *type_data, unsigned int count);

/* Adds attribute line to specified type. */
error__t type_add_attribute_line(
    const struct type *type, void *type_data, const char *line);


/* Returns name of type. */
const char *type_get_type_name(const struct type *type);


error__t initialise_types(void);

void terminate_types(void);
