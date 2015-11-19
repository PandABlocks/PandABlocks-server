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
        DO(attr->change_index[number] = get_change_index());
}


void get_attr_change_set(
    struct attr *attr, uint64_t report_index, bool change_set[])
{
    for (unsigned int i = 0; i < attr->count; i ++)
        change_set[i] =
            attr->methods->in_change_set  &&
            attr->change_index[i] >= report_index;
}


void create_attribute(
    const struct attr_methods *methods,
    struct class *class, void *data, unsigned int count,
    struct hash_table *attr_map)
{
    struct attr *attr = malloc(sizeof(struct attr));
    *attr = (struct attr) {
        .methods = methods,
        .class = class,
        .data = data,
        .count = count,
        .change_index = calloc(count, sizeof(uint64_t)),
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
        free(attr->change_index);
        free(attr);
    }
}
