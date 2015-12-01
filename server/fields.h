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
    const struct field *field, const char *name, struct attr **attr);


/* Description field access. */
const char *get_block_description(struct block *block);
const char *get_field_description(struct field *field);


/* Generates list of all changed fields and their values. */
void generate_change_sets(
    struct connection_result *result, enum change_set changes);

/* Returns true if a change is detected in the given change set. */
bool check_change_set(
    struct change_set_context *change_set_context, enum change_set change_set);


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
    bool append, struct put_table_writer *writer);


/* List of attributes for field:  block.field.*?  */
error__t attr_list_get(struct field *field, struct connection_result *result);


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

/* Sets description string for block. */
error__t block_set_description(struct block *block, const char *description);


/* Call this to create each field. */
error__t create_field(
    struct field **field, const struct block *parent,
    const char *field_name, const char *class_name, const char **line);

/* Called to add attribute lines while parsing config file. */
error__t field_parse_attribute(struct field *field, const char **line);

/* Parse register setting for field. */
error__t field_parse_register(struct field *field, const char **line);

/* Sets description string for field. */
error__t field_set_description(struct field *field, const char *description);


/* Must be called after loading configuration and register database to check
 * that everything is configured correctly and consistently. */
error__t validate_database(void);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* This must be called early during initialisation and before any other methods
 * listed here. */
error__t initialise_fields(void);

/* This will deallocate all resources used for field management. */
void terminate_fields(void);
