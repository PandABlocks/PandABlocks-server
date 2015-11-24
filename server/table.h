/* Access to tables. */

error__t table_get(
    struct class *class, unsigned int ix,
    struct connection_result *result);

error__t table_put_table(
    struct class *class, unsigned int ix,
    bool append, struct put_table_writer *writer);

#define TABLE_CLASS_METHODS \
    .get = table_get, \
    .put_table = table_put_table
