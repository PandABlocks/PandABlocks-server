/* Field class interface. */

struct config_connection;
struct connection_result;
struct put_table_writer;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Block, field, and attribute lookup. */

/* A block represents a top level block.  Each block has a number of fields and
 * a number of instances. */
struct block;

/* Returns block with the given name. */
error__t lookup_block(
    const char *name, struct block **block, unsigned int *count);

/* Retrieves instance count for the given block. */
unsigned int get_block_count(const struct block *block);


/* Each field has a name and a number of access methods. */
struct field;

/* Returns the field with the given name in the given block. */
error__t lookup_field(
    const struct block *block, const char *name, struct field **field);

/* Returns owning block for this field. */
const struct block *field_parent(const struct field *field);


/* Each field may have a number of attributes. */
struct attr;

/* Returns attribute with the given name for this field. */
error__t lookup_attr(
    const struct field *field, const char *name, const struct attr **attr);


enum change_set {
    CHANGES_NONE     = 0,
    CHANGES_CONFIG   = 1,   // *CHANGES.CONFIG?     Configuration changes
    CHANGES_BITS     = 2,   // *CHANGES.BITS?       Bit output changes
    CHANGES_POSITION = 4,   // *CHANGES.POSN?       Position output changes
    CHANGES_READ     = 8,   // *CHANGES.READ?       Read register changes
    CHANGES_ALL =           // *CHANGES?            All changes
        CHANGES_CONFIG | CHANGES_BITS | CHANGES_POSITION | CHANGES_READ,
};

/* Generates list of all changed fields and their values. */
void generate_changes(
    struct config_connection *connection,
    const struct connection_result *result,
    enum change_set changes);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Field access methods. */

/* The field access methods require the following context. */
struct field_context {
    struct config_connection *connection;   // Connection from request
    unsigned int number;                    // Block number, within valid range
    const struct field *field;              // Field database entry
};


/* Implements block meta-data listing command:  *BLOCKS?  */
error__t block_list_get(
    struct config_connection *connection,
    const struct connection_result *result);

/* Implements field meta-data listing command:  block.*?  */
error__t field_list_get(
    const struct block *block,
    struct config_connection *connection,
    const struct connection_result *result);

/* Retrieves current value of field:  block<n>.field?  */
error__t field_get(
    const struct field_context *context,
    const struct connection_result *result);

/* Writes value to field:  block<n>.field=value  */
error__t field_put(const struct field_context *context, const char *value);

/* Writes table of values ot a field:  block<n>.field<  */
error__t field_put_table(
    const struct field_context *context,
    bool append, const struct put_table_writer *writer);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Attribute access methods. */

struct attr_context {
    struct config_connection *connection;   // Connection from request
    unsigned int number;                    // Block number, within valid range
    const struct field *field;              // Field database entry
    const struct attr *attr;                // Field attribute
};

/* List of attributes for field:  block.field.*?  */
error__t attr_list_get(
    const struct field *field,
    struct config_connection *connection,
    const struct connection_result *result);

/* Retrieves current value of field:  block<n>.field?  */
error__t attr_get(
    const struct attr_context *context,
    const struct connection_result *result);

/* Writes value to field:  block<n>.field=value  */
error__t attr_put(const struct attr_context *context, const char *value);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Interface between fields and supporting class. */

/* Returns value in register. */
unsigned int read_field_register(
    const struct field *field, unsigned int number);

/* Writes value to register. */
void write_field_register(
    const struct field *field, unsigned int number, unsigned int value);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Field and block database initialisation.  All the following methods are
 * called during database parsing. */

/* Call this to create each top level block.  The block name, block count and
 * register base must be given.  Once this has been created the subsiduary
 * fields can be created. */
error__t create_block(
    struct block **block, const char *name, unsigned int count);

/* Call this to create each field. */
error__t create_field(
    struct field **field, const struct block *parent, const char *name,
    const char *class_name, const char *type_name);

/* Called to add attribute lines while parsing config file. */
error__t field_add_attribute_line(struct field *field, const char *line);


/* Methods for register setup. */

/* Sets base address for block. */
error__t block_set_base(struct block *block, unsigned int base);
/* Sets register offset for field. */
error__t field_set_reg(struct field *field, unsigned int reg);
/* Sets multiplexer index array for field. */
error__t mux_set_indices(struct field *field, unsigned int indices[]);

error__t validate_database(void);


/* This must be called early during initialisation. */
error__t initialise_fields(void);

/* This will deallocate all resources used for field management. */
void terminate_fields(void);
