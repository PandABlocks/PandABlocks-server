/* Field class interface. */

struct config_connection;
struct connection_result;
struct put_table_writer;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Block lookup. */

/* A block represents a top level block.  Each block has a number of fields and
 * a number of instances. */
struct block;

/* Returns block with the given name. */
error__t lookup_block(const char *name, struct block **block);

/* Retrieves instance count for the given block. */
unsigned int get_block_count(const struct block *block);



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Field lookup and enumeration. */

/* Each field has a name and a number of access methods. */
struct field;

/* Returns the field with the given name in the given block. */
error__t lookup_field(
    const struct block *block, const char *name, struct field **field);

/* Returns owning block for this field. */
const struct block *field_parent(const struct field *field);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Field access implementation. */

/* The following methods implement all of the entity interface commands
 * published through the socket interface. */

/* The field access methods require the following context. */
struct field_context {
    const struct field *field;              // Field database entry
    unsigned int number;                    // Block number, within valid range
    struct config_connection *connection;   // Connection from request
};


/* Implements block meta-data listing command:  *BLOCKS?  */
error__t block_list_get(
    struct config_connection *connection,
    const struct connection_result *result);

/* Implements field meta-data listing command:  block.*?  */
error__t block_fields_get(
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
/* Interface between fields and supporting class. */

/* Returns value in register. */
error__t read_field_register(
    const struct field *field, unsigned int number, unsigned int *result);

/* Writes value to register. */
error__t write_field_register(
    const struct field *field, unsigned int number,
    unsigned int value, bool mark_changed);


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

/* Methods for register setup. */
error__t block_set_base(struct block *block, unsigned int base);
error__t field_set_reg(struct field *field, unsigned int reg);
error__t mux_set_indices(struct field *field, unsigned int indices[]);

error__t validate_database(void);


/* This must be called early during initialisation. */
error__t initialise_fields(void);

void terminate_fields(void);
