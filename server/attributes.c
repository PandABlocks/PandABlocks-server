/* Attribute implementation. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>

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
    char **last_values;         // Used when the change set is polled
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
    WITH_MUTEX(attr->mutex)
        attr->update_index[number] = get_change_index();
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
    WITH_MUTEX(attr->mutex)
    {
        for (unsigned int i = 0; i < attr->count; i ++)
        {
            if (attr->methods->polled_change_set)
            {
                /* Check if attribute has changed by formatting it and comparing
                 * with the cached value. */
                char string[MAX_RESULT_LENGTH];
                attr->methods->format(
                    attr->owner, attr->data, i, string, sizeof(string));
                if (strncmp(string, attr->last_values[i], MAX_RESULT_LENGTH))
                {
                    /* This case is special, we have detected a change
                     * by polling (which happens inside this function), so we
                     * need to update the index, but if we used
                     * `get_change_index`, the same value change would be
                     * reported in the next `get_attr_change_set` call, we use
                     * `report_index + 1` so the value is considered new in the
                     * current call (i.e. The one that polled the value and
                     * found a change) but not the next call if it doesn't find
                     * a new value. */
                    attr->update_index[i] = report_index + 1;
                    strncpy(
                        attr->last_values[i], string, MAX_RESULT_LENGTH);
                }
            }
            change_set[i] =
                (attr->methods->in_change_set ||
                    attr->methods->polled_change_set) &&
                attr->update_index[i] > report_index;
        }
    }
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


static struct attr *create_attribute(
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
    if (methods->polled_change_set)
    {
        attr->last_values = malloc(count * sizeof(char *));
        for (unsigned int i = 0; i < count; i ++)
        {
            attr->last_values[i] = malloc(MAX_RESULT_LENGTH);
            attr->last_values[i][0] = '\0';
        }
    }
    /* Initialise change index to ensure initial state is recorded. */
    for (unsigned int i = 0; i < count; i ++)
        attr->update_index[i] = 1;
    return attr;
}


struct attr *add_one_attribute(
    const struct attr_methods *methods,
    void *owner, void *data, unsigned int count,
    struct hash_table *attr_map)
{
    struct attr *attr = create_attribute(methods, owner, data, count);
    ASSERT_OK(!hash_table_insert(attr_map, methods->name, attr));
    return attr;
}


void add_attributes(
    const struct attr_array array,
    void *owner, void *data, unsigned int count,
    struct hash_table *attr_map)
{
    /* See DEFINE_ATTRIBUTES: we rely on .name==NULL to detect end of list. */
    if (array.methods)
        for (unsigned int i = 0; array.methods[i].name; i ++)
            add_one_attribute(&array.methods[i], owner, data, count, attr_map);
}


void delete_attributes(struct hash_table *attr_map)
{
    size_t ix = 0;
    void *value;
    while (hash_table_walk(attr_map, &ix, NULL, &value))
    {
        struct attr *attr = value;
        if (attr->methods->polled_change_set)
        {
            for (unsigned int i = 0; i < attr->count; i ++)
                free(attr->last_values[i]);
            free(attr->last_values);
        }
        free(attr->update_index);
        free(attr);
    }
}
