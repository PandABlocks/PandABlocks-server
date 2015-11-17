/* Support for types. */


struct field;
struct type;
struct attr;


/* Context require to read or write a type attribute. */
struct type_attr_context {
    unsigned int number;
    struct config_connection *connection;
    const struct field *field;
    struct type *type;
    void *type_data;
    const struct attr *attr;
};


/* This converts a string to a writeable integer. */
error__t type_parse(
    struct type *type, unsigned int number,
    const char *string, unsigned int *value);

/* This formats the value into a string according to the type rules. */
error__t type_format(
    struct type *type, unsigned int number,
    unsigned int value, char string[], size_t length);

/* Outputs list of attributes for the specified type to result. */
void type_attr_list_get(
    const struct type *type,
    struct config_connection *connection,
    const struct connection_result *result);

/* Returns attribute with the given name or NULL if not found. */
const struct attr *type_lookup_attr(const struct type *type, const char *name);

/* Reads identified attribute. */
error__t type_attr_get(
    const struct type_attr_context *context,
    const struct connection_result *result);

/* Writes identified attribute. */
error__t type_attr_put(
    const struct type_attr_context *context, const char *value);


/* Parses type description in name and returns type. */
error__t create_type(
    const char **string, bool forced, unsigned int count, struct type **type);

void destroy_type(struct type *type);

/* Adds attribute line to specified type. */
error__t type_parse_attribute(struct type *type, const char **line);


/* Returns name of type. */
const char *get_type_name(const struct type *type);


error__t initialise_types(void);

void terminate_types(void);
