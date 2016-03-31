/* Implementation of special keys. */

error__t initialise_special(void);
void terminate_special(void);

/* Adds key to list of special keys. */
error__t add_special_key(const char *key);

/* Returns list of permitted special keys. */
error__t get_special_keys(struct connection_result *result);

/* Returns string currently associated with key. */
error__t get_special_value(const char *key, struct connection_result *result);

/* Updates string associated with key. */
error__t put_special_value(const char *key, const char *value);

/* Checks if any special values have changed since the given report index. */
bool check_special_change_set(uint64_t report_index);

/* Returns change set for given index. */
void generate_special_change_set(
    struct connection_result *result, uint64_t report_index);
