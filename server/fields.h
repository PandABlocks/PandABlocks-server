/* Field class interface. */

/* Connection dependencies. */
struct connection_result;   // Used to return results to active client
struct put_table_writer;    // Used for writing table data
struct hash_table;
struct field;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


/* Abstract interface to class. */
struct class_methods {
    const char *name;

    /* Called to parse the class definition line for a field.  The corresponding
     * class has already been identified. */
    error__t (*init)(
        const char **line, unsigned int count,
        struct hash_table *attr_map, void **class_data,
        struct indent_parser *parser);

    /* Parses the register definition line for this field. */
    error__t (*parse_register)(
        void *class_data, struct field *field, unsigned int block_base,
        const char **line);
    /* Called at end of startup to finalise and validate setup. */
    error__t (*finalise)(void *class_data);
    /* Called during shutdown to release all class resources. */
    void (*destroy)(void *class_data);

    /* Option for parsing indented extra description lines, designed for array
     * subfields. */
    void (*set_description_parse)(
        void *class_data, struct indent_parser *parser);

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
        bool streaming, bool last_table, bool binary,
        struct put_table_writer *writer);

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

    /* Returns subfield for array field. */
    struct table_subfield *(*get_subfield)(void *class_data, const char *name);

    /* Class specific attributes. */
    const struct attr_array attrs;
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Block, field, and attribute lookup. */

/* A block represents a top level block.  Each block has a number of fields and
 * a number of instances.  Each field has a name and a number of access methods.
 * Each field may have a number of attributes. */
struct block;               // Top level hardware entity
struct field;               // Controllable fields for each block
struct attr;                // Type specific attributes for individual fields
struct table_subfield;      // Subfields for arrays
struct extension_block;     // Block extension for extension server


/* Returns block with the given name. */
error__t lookup_block(
    const char *name, struct block **block, unsigned int *count);

/* Returns the field with the given name in the given block. */
error__t lookup_field(
    const struct block *block, const char *name, struct field **field);

/* Returns attribute with the given name for this field. */
error__t lookup_attr(
    const struct field *field, const char *name, struct attr **attr);

/* Returns array subfield if defined. */
error__t lookup_table_subfield(
    const struct field *field, const char *name,
    struct table_subfield **subfield);


/* Description field access. */
const char *get_block_description(struct block *block);
const char *get_field_description(struct field *field);

/* Formats field name into given string.  The target string *must* be long
 * enough, and the resulting length is returned. */
size_t format_field_name(
    char string[], size_t length,
    const struct field *field, const struct attr *attr,
    unsigned int number, char suffix);

/* Generates list of all changed fields and their values. */
void generate_change_sets(
    struct connection_result *result, enum change_set changes,
    bool print_tables);

/* Returns true if a change is detected in the given change set. */
bool check_change_set(
    struct change_set_context *change_set_context, enum change_set change_set);

/* Ensures specified change set will not report any changes up to this point, or
 * resets change set back to the start. */
enum reset_change_set_action { RESET_START, RESET_END };
void reset_change_set(
    struct change_set_context *context, enum change_set change_set,
    enum reset_change_set_action action);


/* Returns block associated with given field. */
struct block *get_field_block(const struct field *field);

/* Returns extension_block for this block. */
struct extension_block *get_block_extension(const struct block *block);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Field access methods. */


/* Implements block meta-data listing command:  *BLOCKS?  */
error__t block_list_get(struct connection_result *result);

/* Implements field meta-data listing command:  block.*?  */
error__t field_list_get(
    const struct block *block, struct connection_result *result);


/* Retrieves current value of field:  block<n>.field?  */
error__t field_get(
    struct field *field, unsigned int number, struct connection_result *result);

/* Writes value to field:  block<n>.field=value  */
error__t field_put(struct field *field, unsigned int number, const char *value);

/* Writes table of values ot a field:  block<n>.field<  */
error__t field_put_table(
    struct field *field, unsigned int number,
    bool append, bool binary, bool more_expected,
    struct put_table_writer *writer);


/* List of attributes for field:  block.field.*?  */
error__t attr_list_get(struct field *field, struct connection_result *result);

/* Associated enumeration or NULL. */
const struct enumeration *get_field_enumeration(const struct field *field);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Field and block database initialisation.  All the following methods are
 * called during database parsing. */

/* Call this to create each top level block.  The block name, block count and
 * register base must be given.  Once this has been created the subsiduary
 * fields can be created. */
error__t create_block(
    struct block **block, const char *name, unsigned int count);

/* Sets base address for block. */
error__t parse_block_set_register(const char **line, struct block *block);

/* Sets description string for block. */
error__t block_set_description(struct block *block, const char *description);


/* Call this to create each field.  If further lines are needed to define this
 * field then *parser should be updated accordingly. */
error__t create_field(
    const char **line, struct block *block, struct indent_parser *parser);

/* Parse register setting for field. */
error__t field_parse_registers(struct field *field, const char **line);

/* Sets description string for field. */
error__t field_set_description(
    struct field *field, const char *description, struct indent_parser *parser);


/* Parses validates and assigns register, ensuring that the register is unique
 * to the associated block.  Should be called for each register parsed. */
error__t check_parse_register(
    struct field *field, const char **line, unsigned int *reg);


/* Must be called after loading configuration and register database to check
 * that everything is configured correctly and consistently. */
error__t validate_fields(void);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* This must be called early during initialisation and before any other methods
 * listed here. */
error__t initialise_fields(void);

/* This will deallocate all resources used for field management. */
void terminate_fields(void);
