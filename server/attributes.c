/* Attribute implementation. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>

#include "error.h"
#include "hashtable.h"
#include "config_server.h"
#include "locking.h"

#include "attributes.h"


struct attr {
    const struct attr_methods *methods;
    void *owner;                // Attribute ownder
    void *data;                 // Any data associated with this attribute
    unsigned int count;         // Number of field instances
    pthread_mutex_t mutex;      // Protects update_index entries
    uint64_t *update_index;     // History management for reported attributes
};



/* Implements block[n].field.attr? */
error__t attr_get(
    struct attr *attr, unsigned int number,
    struct connection_result *result)
{
    /* We have two possible implementations of attr get: .format and .get_many.
     * If the .format field is available then we use that by preference. */
    if (attr->methods->format)
    {
        result->response = RESPONSE_ONE;
        return attr->methods->format(
            attr->owner, attr->data, number, result->string, result->length);
    }
    else if (attr->methods->get_many)
    {
        result->response = RESPONSE_MANY;
        return attr->methods->get_many(attr->owner, attr->data, number, result);
    }
    else
        return FAIL_("Attribute not readable");
}


void attr_changed(struct attr *attr, unsigned int number)
{
    LOCK(attr->mutex);
    attr->update_index[number] = get_change_index();
    UNLOCK(attr->mutex);
}


error__t attr_put(struct attr *attr, unsigned int number, const char *value)
{
    return
        TEST_OK_(attr->methods->put, "Attribute not writeable")  ?:
        attr->methods->put(attr->owner, attr->data, number, value)  ?:
        DO(attr_changed(attr, number));
}


void get_attr_change_set(
    struct attr *attr, uint64_t report_index, bool change_set[])
{
    LOCK(attr->mutex);
    for (unsigned int i = 0; i < attr->count; i ++)
        change_set[i] =
            attr->methods->in_change_set  &&
            attr->update_index[i] > report_index;
    UNLOCK(attr->mutex);
}


const char *get_attr_name(const struct attr *attr)
{
    return attr->methods->name;
}


const struct enumeration *get_attr_enumeration(const struct attr *attr)
{
    if (attr->methods->get_enumeration)
        return attr->methods->get_enumeration(attr->data);
    else
        return NULL;
}


const char *get_attr_description(const struct attr *attr)
{
    return attr->methods->description;
}


struct attr *create_attribute(
    const struct attr_methods *methods,
    void *owner, void *data, unsigned int count)
{
    struct attr *attr = malloc(sizeof(struct attr));
    *attr = (struct attr) {
        .methods = methods,
        .owner = owner,
        .data = data,
        .count = count,
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .update_index = malloc(count * sizeof(uint64_t)),
    };
    /* Initialise change index to ensure initial state is recorded. */
    for (unsigned int i = 0; i < count; i ++)
        attr->update_index[i] = 1;
    return attr;
}


void create_attributes(
    const struct attr_methods methods[], unsigned int attr_count,
    void *owner, void *data, unsigned int count,
    struct hash_table *attr_map)
{
    for (unsigned int i = 0; i < attr_count; i ++)
        hash_table_insert(
            attr_map, methods[i].name,
            create_attribute(&methods[i], owner, data, count));
}


void delete_attributes(struct hash_table *attr_map)
{
    size_t ix = 0;
    void *value;
    while (hash_table_walk(attr_map, &ix, NULL, &value))
    {
        struct attr *attr = value;
        free(attr->update_index);
        free(attr);
    }
}
