/* For each field there is a field "class" which implements the interface to
 * the hardware. */

struct put_table_writer;
struct connection_result;
struct hash_table;

struct class;
struct type;


struct class {
    const struct class_methods *methods;    // Class implementation
    unsigned int count;             // Number of instances of this block
    unsigned int block_base;        // Register base for block
    unsigned int field_register;    // Register for field (if required)
    void *class_data;               // Class specific data
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Class field access. */

/* Reads value from associated register, or error if reading not supported. */
error__t class_read(
    struct class *class, unsigned int number, uint32_t *value, bool refresh);

/* Writes value to register, or returns error if not supported. */
error__t class_write(struct class *class, unsigned int number, uint32_t value);


/* These methods will only be called if there is no type support for the class,
 * in this case the class operates directly on the strings. */
error__t class_get(
    struct class *class, unsigned int number,
    const struct connection_result *result);
error__t class_put(
    struct class *class, unsigned int number, const char *value);


/* Direct implementation of table support. */
error__t class_put_table(
    struct class *class, unsigned int number,
    bool append, struct put_table_writer *writer);


/* Allocates and returns a fresh change index. */
uint64_t get_change_index(void);

/* Global refresh of changes for those classes which need a global refresh. */
void refresh_class_changes(enum change_set change_set);

/* Retrieves change set for the given class. */
void get_class_change_set(
    struct class *class, const uint64_t report_index[], bool changes[]);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Class inititialisation. */

/* Performs class initialisation and creates any associated type. */
error__t create_class(
    const char *class_name, const char **line, unsigned int count,
    struct class **class, struct type **type);

/* Adds class attributes to given attr_map. */
void create_class_attributes(
    struct class *class, struct hash_table *attr_map);

/* Parse register definition line. */
error__t class_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line);

/* To be called after database loading is complete to ensure that all classes
 * have their required register assignments.  Also at this point we assign the
 * block base address. */
error__t validate_class(struct class *class, unsigned int block_base);

/* Returns name of class. */
const char *get_class_name(struct class *class);

/* This should be called during shutdown for each created class. */
void destroy_class(struct class *class);
