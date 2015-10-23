/* Interface to database of record definitions. */

/* We'll reject identifiers longer than this. */
#define MAX_NAME_LENGTH     20



/* Abstract interfaces to the database. */
struct config_block;
struct field_entry;
struct meta_field;


/* This function loads the three configuration databases into memory. */
error__t load_config_databases(
    const char *config_db, const char *types_db, const char *register_db);


/* Function for walking the database of blocks.  Use by setting *ix to 0 and
 * repeately calling this function until false is returned. */
bool walk_blocks_list(
    int *ix, const struct config_block **block,
    const char **name, unsigned int *count);

/* Looks up block by name, also returns block count. */
const struct config_block *lookup_block(const char *name, unsigned int *count);

/* Walks list of fields in a block.  Usage as for walk_blocks_list. */
bool walk_fields_list(
    const struct config_block *block,
    int *ix, const struct field_entry **field, const char **name);

/* Looks up field by name. */
const struct field_entry *lookup_field(
    const struct config_block *block, const char *name);

/* Looks up field meta-data. */
const struct meta_field *lookup_meta(
    const struct field_entry *field, const char *name);
