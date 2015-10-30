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
const struct block *lookup_block(const char *name);

/* Function for walking the database of blocks.  Use by setting *ix to 0 and
 * repeately calling this function until false is returned. */
bool walk_blocks_list(int *ix, const struct block **block);

/* Retrieves name and instance count for the given block. */
void get_block_info(
    const struct block *block, const char **name, unsigned int *count);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Field lookup. */

/* Each field has a name and a number of access methods. */
struct field;

/* Returns the field with the given name in the given block. */
const struct field *lookup_field(
    const struct block *block, const char *name);

/* Walks list of fields in a block.  Usage as for walk_blocks_list. */
bool walk_fields_list(
    const struct block *block, int *ix, const struct field **field);

/* Retrieves field name and class name for given field. */
void get_field_info(
    const struct field *field,
    const char **field_name, const char **class_name);


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Field access. */

/* Field access methods.  These all require the following context. */
struct field_context {
    const struct field *field;              // Field database entry
    unsigned int number;                    // Block number, within valid range
    struct config_connection *connection;   // Connection from request
};

/* Retrieves current value of field. */
error__t field_get(
    const struct field_context *context,
    const struct connection_result *result);

/* Writes value to field. */
error__t field_put(const struct field_context *context, const char *value);

/* Writes table of values ot a field. */
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
    struct block **block, const char *name,
    unsigned int count, unsigned int base);

/* Call this to create each field. */
error__t create_field(
    struct field **field, const struct block *parent, const char *name,
    const char *class_name, const char *type_name);


/* This must be called early during initialisation. */
error__t initialise_fields(void);
