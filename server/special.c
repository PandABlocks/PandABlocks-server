/* Support for special keys via *SPECIAL command. */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>

#include "error.h"
#include "hashtable.h"
#include "config_server.h"
#include "locking.h"

#include "special.h"


struct special_value {
    char *value;
    uint64_t update_index;
};


static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static struct hash_table *special_map;


error__t initialise_special(void)
{
    special_map = hash_table_create(true);
    return ERROR_OK;
}


void terminate_special(void)
{
    if (special_map)
    {
        size_t ix = 0;
        struct special_value *value;
        while (hash_table_walk(special_map, &ix, NULL, (void **) &value))
        {
            free(value->value);
            free(value);
        }
        hash_table_destroy(special_map);
    }
}


error__t add_special_key(const char *key)
{
    struct special_value *value = malloc(sizeof(struct special_value));
    *value = (struct special_value) {
        .update_index = 1,
    };
    return TEST_OK_(hash_table_insert(special_map, key, value) == 0,
        "Special key %s repeated", key);
}


error__t get_special_keys(struct connection_result *result)
{
    size_t ix = 0;
    const char *key;
    while (hash_table_walk(special_map, &ix, (const void **) &key, NULL))
        format_many_result(result, "%s", key);
    result->response = RESPONSE_MANY;
    return ERROR_OK;
}


error__t get_special_value(const char *key, struct connection_result *result)
{
    struct special_value *value = hash_table_lookup(special_map, key);
    return
        TEST_OK_(value, "Special key %s not found", key)  ?:
        WITH_LOCK(mutex, format_one_result(result, "%s", value->value ?: ""));
}


error__t put_special_value(const char *key, const char *string)
{
    struct special_value *value = hash_table_lookup(special_map, key);
    error__t error = TEST_OK_(value, "Special key %s not found", key);
    if (!error)
    {
        LOCK(mutex);
        free(value->value);
        *value = (struct special_value) {
            .value = strdup(string),
            .update_index = get_change_index(),
        };
        UNLOCK(mutex);
    }
    return error;
}


bool check_special_change_set(uint64_t report_index)
{
    bool changed = false;
    LOCK(mutex);
    size_t ix = 0;
    struct special_value *value;
    while (hash_table_walk(special_map, &ix, NULL, (void *) &value))
        if (value->update_index > report_index)
            changed = true;
    UNLOCK(mutex);
    return changed;
}


void generate_special_change_set(
    struct connection_result *result, uint64_t report_index)
{
    LOCK(mutex);
    size_t ix = 0;
    const char *key;
    struct special_value *value;
    while (hash_table_walk(
            special_map, &ix, (const void **) &key, (void *) &value))
        if (value->update_index > report_index)
        {
            char string[MAX_RESULT_LENGTH];
            snprintf(string, sizeof(string),
                "*SPECIAL.%s=%s", key, value->value ?: "");
            result->write_many(result->write_context, string);
        }
    UNLOCK(mutex);
}
