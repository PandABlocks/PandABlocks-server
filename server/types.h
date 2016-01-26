/* Support for types.  Here a "type" object mediates between an integer value
 * written to a register and a textual representation. */

struct type;


/* A support type is defined by defining at least some of the methods below.  In
 * particular, at least one of .parse or .format should be defined. */
struct type_methods {
    const char *name;

    /* This creates and initialises any type specific data needed. */
    error__t (*init)(const char **string, unsigned int count, void **type_data);
    /* By default type_data will be freed on destruction.  This optional method
     * implements any more complex destruction process needed. */
    void (*destroy)(void *type_data, unsigned int count);

    /* This is called during startup to process an attribute line. */
    error__t (*add_attribute_line)(void *type_data, const char **string);

    /* This converts a string to a writeable integer. */
    error__t (*parse)(
        void *type_data, unsigned int number,
        const char *string, unsigned int *value);

    /* This formats the value into a string according to the type rules. */
    error__t (*format)(
        void *type_data, unsigned int number,
        unsigned int value, char string[], size_t length);

    /* Returns enumeration assocated with type, if appropriate. */
    const struct enumeration *(*get_enumeration)(void *type_data);

    /* Type specific attributes, automatically instantiated when type instance
     * created. */
    const struct attr_methods *attrs;
    unsigned int attr_count;
};


/* API used by type to access the underlying register value.  All methods in
 * this structure are optional. */
struct register_methods {
    /* Reads current register value. */
    error__t (*read)(void *reg_data, unsigned int number, uint32_t *value);
    /* Writes to register. */
    error__t (*write)(void *reg_data, unsigned int number, uint32_t value);
    /* Notifies register change. */
    void (*changed)(void *reg_data, unsigned int number);
};


/* Access to register methods via the bound type. */
error__t read_type_register(
    struct type *type, unsigned int number, uint32_t *value);
error__t write_type_register(
    struct type *type, unsigned int number, uint32_t value);
void changed_type_register(struct type *type, unsigned int number);


/* Formatted raw integer value access. */
error__t raw_format_uint(
    void *owner, void *data, unsigned int number, char result[], size_t length);
error__t raw_put_uint(
    void *owner, void *data, unsigned int number, const char *string);

error__t raw_format_int(
    void *owner, void *data, unsigned int number, char result[], size_t length);
error__t raw_put_int(
    void *owner, void *data, unsigned int number, const char *string);


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
    const char **line, const char *default_type, unsigned int count,
    const struct register_methods *reg, void *reg_data,
    struct hash_table *attr_map, struct type **type);

/* Releases internal resources associated with type. */
void destroy_type(struct type *type);

/* Returns name of type. */
const char *get_type_name(const struct type *type);

/* Associated enumeration or NULL. */
const struct enumeration *get_type_enumeration(const struct type *type);


/* Adds attribute line to specified type.  Used to add enumeration options. */
error__t type_parse_attribute(struct type *type, const char **line);
