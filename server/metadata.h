/* Implementation of metadata keys. */

error__t initialise_metadata(void);
void terminate_metadata(void);

/* Adds key to list of metadata keys. */
error__t add_metadata_key(const char *key, const char **line);

/* Returns list of permitted metadata keys. */
error__t get_metadata_keys(struct connection_result *result);

/* Returns string currently associated with key. */
error__t get_metadata_value(const char *key, struct connection_result *result);

/* Updates string associated with key. */
error__t put_metadata_value(const char *key, const char *value);

/* Updates multi-line metadata field. */
error__t put_metadata_table(const char *key, struct put_table_writer *writer);

/* Checks if any metadata values have changed since the given report index. */
bool check_metadata_change_set(uint64_t report_index);

/* Returns change set for given index. */
void generate_metadata_change_set(
    struct connection_result *result, uint64_t report_index,
    bool print_table);
