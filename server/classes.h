/* For each field there is a field "class" which implements the interface to
 * the hardware. */

struct put_table_writer;
struct connection_result;
struct hash_table;

struct field;
struct class;

enum change_set;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Abstract interface to class. */
struct class_methods {
    const char *name;

    /* Called to parse the class definition line for a field.  The corresponding
     * class has already been identified. */
    error__t (*init)(
        const char **line, unsigned int count,
        struct hash_table *attr_map, void **class_data);

    /* Parses the attribute definition line for this field. */
    error__t (*parse_attribute)(void *class_data, const char **line);
    /* Parses the register definition line for this field. */
    error__t (*parse_register)(
        void *class_data, struct field *field, unsigned int block_base,
        const char **line);
    /* Called at end of startup to finalise and validate setup. */
    error__t (*finalise)(void *class_data);
    /* Called during shutdown to release all class resources. */
    void (*destroy)(void *class_data);

    /* Implements  block.field? for a single value. */
    error__t (*get)(
        void *class_data, unsigned int number, char result[], size_t length);
    /* Implements  block.field? for multiple values.  Will only be called if
     * .get is not defined. */
    error__t (*get_many)(
        void *class_data, unsigned int number,
        struct connection_result *result);
    /* Implements  block.field=value */
    error__t (*put)(void *class_data, unsigned int number, const char *value);
    /* Implements  block.field<  */
    error__t (*put_table)(
        void *class_data, unsigned int number,
        bool append, struct put_table_writer *writer);

    /* For the _out classes the data provided by .read() needs to be loaded as a
     * separate action, this optional method does this. */
    void (*refresh)(void *class_data, unsigned int number);

    /* Computes change set for this class.  The class looks up its own change
     * index in report_index[] and updates changes[] accordingly. */
    void (*change_set)(
        void *class_data, const uint64_t report_index, bool changes[]);
    /* If .change_set is non NULL then this must be set to the change set index
     * which will be reported. */
    unsigned int change_set_index;

    /* Optionally returns class description. */
    const char *(*describe)(void *class_data);

    /* Returns enumeration if class has an associated enumeration. */
    const struct enumeration *(*get_enumeration)(void *class_data);

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
void refresh_class_changes(enum change_set change_set, uint64_t change_index);

/* Retrieves change set for the given class. */
void get_class_change_set(
    struct class *class, enum change_set change_set,
    const uint64_t report_index[], bool changes[]);

/* Returns description of class including any type. */
error__t describe_class(struct class *class, char string[], size_t length);

/* Associated enumeration or NULL. */
const struct enumeration *get_class_enumeration(const struct class *class);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Class inititialisation. */

/* Performs class initialisation and creates any associated type. */
error__t create_class(
    const char **line, unsigned int count,
    struct hash_table *attr_map, struct class **class);

/* Parses field attribute in configuration file. */
error__t class_parse_attribute(struct class *class, const char **line);

/* Parse register definition line. */
error__t class_parse_register(
    struct class *class, struct field *field, unsigned int block_base,
    const char **line);

/* To be called after database loading is complete to ensure that the
 * initialisation of all classes is complete. */
error__t finalise_class(struct class *class);

/* This should be called during shutdown for each created class. */
void destroy_class(struct class *class);
