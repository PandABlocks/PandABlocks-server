/* Attribute implementation. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>

#include "error.h"
#include "hashtable.h"
#include "config_server.h"
#include "classes.h"

#include "attributes.h"


/* Implements block[n].field.attr? */
error__t attr_get(
    struct attr *attr, unsigned int number,
    const struct connection_result *result)
{
    /* We have two possible implementations of attr get: .format and .get_many.
     * If the .format field is available then we use that by preference. */
    if (attr->methods->format)
    {
        char string[MAX_RESULT_LENGTH];
        return
            attr->methods->format(attr, number, string, sizeof(string))  ?:
            DO(result->write_one(result->connection, string));
    }
    else if (attr->methods->get_many)
        return attr->methods->get_many(attr, number, result);
    else
        return FAIL_("Attribute not readable");
}


error__t attr_put(struct attr *attr, unsigned int number, const char *value)
{
    return
        TEST_OK_(attr->methods->put, "Attribute not writeable")  ?:
        attr->methods->put(attr, number, value)  ?:
        DO(attr->change_index = get_change_index());
}


void create_attribute(
    const struct attr_methods *methods,
    struct class *class, void *type_data,
    struct hash_table *attr_map)
{
    struct attr *attr = malloc(sizeof(struct attr));
    *attr = (struct attr) {
        .methods = methods,
        .class = class,
        .type_data = type_data,
        .change_index = 0,
    };
    hash_table_insert(attr_map, methods->name, attr);
}


void delete_attributes(struct hash_table *attr_map)
{
    int ix = 0;
    const void *key;
    void *value;
    while (hash_table_walk(attr_map, &ix, &key, &value))
    {
        struct attr *attr = value;
        free(attr);
    }
}
