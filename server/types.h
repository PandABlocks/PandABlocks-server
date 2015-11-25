/* Support for types. */
struct type;
struct register_api;


/* Reads value from associated register and formats for presentation into the
 * given result.  Implements  block.field?  method. */
error__t type_get(
    struct type *type, unsigned int number, struct connection_result *result);

/* Parses given string and writes result into associated register.  Implements
 * block.field=value  method. */
error__t type_put(struct type *type, unsigned int number, const char *string);


/* Parses type description in name and returns type.  The bound register will be
 * used read and write the underlying value for formatting. */
error__t create_type(
    const char **line, unsigned int count,
    struct register_api *reg, struct type **type);

/* Releases internal resources associated with type. */
void destroy_type(struct type *type);

/* Returns name of type. */
const char *get_type_name(const struct type *type);


/* Adds attribute line to specified type.  Used to add enumeration options. */
error__t type_parse_attribute(struct type *type, const char **line);

/* Adds type attributes to given attr_map. */
void create_type_attributes(struct type *type, struct hash_table *attr_map);


/* Formats double without leading spaces. */
error__t format_double(char result[], size_t length, double value);
