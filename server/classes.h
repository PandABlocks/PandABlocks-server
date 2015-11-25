/* For each field there is a field "class" which implements the interface to
 * the hardware. */

struct put_table_writer;
struct connection_result;
struct hash_table;

struct class;
struct type;

enum change_set;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Abstract interface to class. */
struct class_methods {
    const char *name;

    /* Called to parse the class definition line for a field.  The corresponding
     * class has already been identified. */
    error__t (*init)(const char **line, unsigned int count, void **class_data);

    /* Parses the attribute definition line for this field. */
    error__t (*parse_attribute)(void *class_data, const char **line);
    /* Parses the register definition line for this field. */
    error__t (*parse_register)(
        void *class_data, const char *block_name, const char *field_name,
        const char **line);
    /* Called at end of startup to finalise and validate setup. */
    error__t (*finalise)(void *class_data, unsigned int block_base);
    /* Called during shutdown to release all class resources. */
    void (*destroy)(void *class_data);

    /* Implements  block.field? */
    error__t (*get)(
        void *class_data, unsigned int ix, struct connection_result *result);
    /* Implements  block.field=value */
    error__t (*put)(void *class_data, unsigned int ix, const char *value);
    /* Implements  block.field<  */
    error__t (*put_table)(
        void *class_data, unsigned int ix,
        bool append, struct put_table_writer *writer);

    /* For the _out classes the data provided by .read() needs to be loaded as a
     * separate action, this optional method does this. */
    void (*refresh)(void *class_data, unsigned int number);

    /* Computes change set for this class.  The class looks up its own change
     * index in report_index[] and updates changes[] accordingly. */
    void (*change_set)(
        void *class_data, const uint64_t report_index[], bool changes[]);

    /* Optionally returns class description. */
    const char *(*describe)(void *class_data);

    /* Class specific attributes. */
    const struct attr_methods *attrs;
    unsigned int attr_count;
};




/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Class field access. */


/* Read formatted value from class.  If refresh is set then the latest value
 * will be read from hardware (if appropriate), otherwise the most recent cached
 * value is returned. */
error__t class_get(
    struct class *class, unsigned int number, bool refresh,
    struct connection_result *result);

/* Writes formatted value to class. */
error__t class_put(struct class *class, unsigned int number, const char *value);

/* Direct implementation of table support. */
error__t class_put_table(
    struct class *class, unsigned int number,
    bool append, struct put_table_writer *writer);


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
    struct class **class);

/* Adds class attributes to given attr_map. */
void create_class_attributes(struct class *class, struct hash_table *attr_map);

/* Parses field attribute in configuration file. */
error__t class_parse_attribute(struct class *class, const char **line);

/* Parse register definition line. */
error__t class_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line);

/* To be called after database loading is complete to ensure that all classes
 * have their required register assignments.  Also at this point we assign the
 * block base address. */
error__t finalise_class(struct class *class, unsigned int block_base);

/* Returns description of class including any type. */
void describe_class(struct class *class, char *string, size_t length);

/* This should be called during shutdown for each created class. */
void destroy_class(struct class *class);
