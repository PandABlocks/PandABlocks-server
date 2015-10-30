/* Field class interface. */


struct put_table_writer;
struct connection_result;


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
/* Field get, put, put_table support.  These implement the field read and write
 * methods. */

/* Each field has a name and a number of access methods. */
struct field;

/* Returns the field with the given name in the given block. */
const struct field *lookup_field(
    const struct block *block, const char *name);


/* We need one of these for every field class method to provide our working
 * context. */
struct field_context {
    const struct field *field;
    unsigned int number;
    struct config_connection *connection;
};

/* The field_access is an abstract interface implementing access functions. */
struct field_access {
    /*  block[n].field?
     * Implements reading from a field. */
    error__t (*get)(
        const struct field_context *context,
        const struct connection_result *result);

    /*  block[n].field=value
     * Implements writing to the field. */
    error__t (*put)(const struct field_context *context, const char *value);

    /*  block[n].field<
     * Implements writing to a table, for the one class which support this. */
    error__t (*put_table)(
        const struct field_context *context,
        bool append, const struct put_table_writer *writer);
};

/* Returns the field class associated with the given field. */
const struct field_access *get_field_access(const struct field *field);

/* Walks list of fields in a block.  Usage as for walk_blocks_list. */
bool walk_fields_list(
    const struct block *block, int *ix, const struct field **field);

/* Retrieves field name and class name for given field. */
void get_field_info(
    const struct field *field,
    const char **field_name, const char **class_name);


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
