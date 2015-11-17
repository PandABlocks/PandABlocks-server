/* Field class interface. */

/* Connection dependencies. */
struct connection_result;   // Used to return results to active client
struct put_table_writer;    // Used for writing table data


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Block, field, and attribute lookup. */

/* A block represents a top level block.  Each block has a number of fields and
 * a number of instances.  Each field has a name and a number of access methods.
 * Each field may have a number of attributes. */
struct block;               // Top level hardware entity
struct field;               // Controllable fields for each block
struct attr;                // Type specific attributes for individual fields


/* Returns block with the given name. */
error__t lookup_block(
    const char *name, struct block **block, unsigned int *count);

/* Returns the field with the given name in the given block. */
error__t lookup_field(
    const struct block *block, const char *name, struct field **field);

/* Returns attribute with the given name for this field. */
error__t lookup_attr(
    const struct field *field, const char *name, const struct attr **attr);


/* We group the reportable changes into four groups, each separately reportable.
 * Normal configuration changes, two sets of live data updates, and a polled
 * readback change set. */
#define CHANGE_IX_CONFIG    0   // *CHANGES.CONFIG?     Configuration changes
#define CHANGE_IX_BITS      1   // *CHANGES.BITS?       Bit output changes
#define CHANGE_IX_POSITION  2   // *CHANGES.POSN?       Position output changes
#define CHANGE_IX_READ      3   // *CHANGES.READ?       Read register changes
enum change_set {
    CHANGES_NONE     = 0,
    CHANGES_CONFIG   = 1 << CHANGE_IX_CONFIG,
    CHANGES_BITS     = 1 << CHANGE_IX_BITS,
    CHANGES_POSITION = 1 << CHANGE_IX_POSITION,
    CHANGES_READ     = 1 << CHANGE_IX_READ,
    CHANGES_ALL =               // *CHANGES?            All changes
        CHANGES_CONFIG | CHANGES_BITS | CHANGES_POSITION | CHANGES_READ,
};

#define CHANGE_SET_SIZE     4
STATIC_COMPILE_ASSERT(CHANGES_ALL < 1 << CHANGE_SET_SIZE)

/* Generates list of all changed fields and their values. */
void generate_change_sets(
    const struct connection_result *result, enum change_set changes);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Field access methods. */


/* Implements block meta-data listing command:  *BLOCKS?  */
error__t block_list_get(const struct connection_result *result);

/* Implements field meta-data listing command:  block.*?  */
error__t field_list_get(
    const struct block *block, const struct connection_result *result);


/* Retrieves current value of field:  block<n>.field?  */
error__t field_get(
    struct field *field, unsigned int number,
    const struct connection_result *result);

/* Writes value to field:  block<n>.field=value  */
error__t field_put(struct field *field, unsigned int number, const char *value);

/* Writes table of values ot a field:  block<n>.field<  */
error__t field_put_table(
    struct field *field, unsigned int number,
    bool append, struct put_table_writer *writer);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Attribute access methods. */

struct attr_context {
    unsigned int number;                    // Block number, within valid range
    struct field *field;                    // Field database entry
    const struct attr *attr;                // Field attribute
};

/* List of attributes for field:  block.field.*?  */
error__t attr_list_get(
    struct field *field,
    const struct connection_result *result);


/* Retrieves current value of field:  block<n>.field?  */
error__t attr_get(
    const struct attr_context *context,
    const struct connection_result *result);

/* Writes value to field:  block<n>.field=value  */
error__t attr_put(const struct attr_context *context, const char *value);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Field and block database initialisation.  All the following methods are
 * called during database parsing. */

/* Call this to create each top level block.  The block name, block count and
 * register base must be given.  Once this has been created the subsiduary
 * fields can be created. */
error__t create_block(
    struct block **block, const char *name, unsigned int count);

/* Sets base address for block. */
error__t block_set_register(struct block *block, unsigned int base);


/* Call this to create each field. */
error__t create_field(
    struct field **field, const struct block *parent,
    const char *field_name, const char *class_name, const char **line);

/* Called to add attribute lines while parsing config file. */
error__t field_parse_attribute(struct field *field, const char **line);

/* Parse register setting for field. */
error__t field_parse_register(struct field *field, const char **line);


/* Must be called after loading configuration and register database to check
 * that everything is configured correctly and consistently. */
error__t validate_database(void);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* This must be called early during initialisation and before any other methods
 * listed here. */
error__t initialise_fields(void);

/* This will deallocate all resources used for field management. */
void terminate_fields(void);
