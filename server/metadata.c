/* Support for metadata keys via *METADATA command. */

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

#include "metadata.h"


struct metadata_value {
    char *value;
    uint64_t update_index;
};


static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static struct hash_table *metadata_map;


error__t initialise_metadata(void)
{
    metadata_map = hash_table_create(true);
    return ERROR_OK;
}


void terminate_metadata(void)
{
    if (metadata_map)
    {
        size_t ix = 0;
        struct metadata_value *value;
        while (hash_table_walk(metadata_map, &ix, NULL, (void **) &value))
        {
            free(value->value);
            free(value);
        }
        hash_table_destroy(metadata_map);
    }
}


error__t add_metadata_key(const char *key, const char **line)
{
    struct metadata_value *value = malloc(sizeof(struct metadata_value));
    *value = (struct metadata_value) {
        .update_index = 1,
    };
    return TEST_OK_(hash_table_insert(metadata_map, key, value) == 0,
        "Special key %s repeated", key);
}


error__t get_metadata_keys(struct connection_result *result)
{
    size_t ix = 0;
    const char *key;
    while (hash_table_walk(metadata_map, &ix, (const void **) &key, NULL))
        format_many_result(result, "%s", key);
    result->response = RESPONSE_MANY;
    return ERROR_OK;
}


error__t get_metadata_value(const char *key, struct connection_result *result)
{
    struct metadata_value *value = hash_table_lookup(metadata_map, key);
    return
        TEST_OK_(value, "Special key %s not found", key)  ?:
        WITH_LOCK(mutex, format_one_result(result, "%s", value->value ?: ""));
}


error__t put_metadata_value(const char *key, const char *string)
{
    struct metadata_value *value = hash_table_lookup(metadata_map, key);
    error__t error = TEST_OK_(value, "Special key %s not found", key);
    if (!error)
    {
        LOCK(mutex);
        free(value->value);
        *value = (struct metadata_value) {
            .value = strdup(string),
            .update_index = get_change_index(),
        };
        UNLOCK(mutex);
    }
    return error;
}


bool check_metadata_change_set(uint64_t report_index)
{
    bool changed = false;
    LOCK(mutex);
    size_t ix = 0;
    struct metadata_value *value;
    while (hash_table_walk(metadata_map, &ix, NULL, (void *) &value))
        if (value->update_index > report_index)
            changed = true;
    UNLOCK(mutex);
    return changed;
}


void generate_metadata_change_set(
    struct connection_result *result, uint64_t report_index)
{
    LOCK(mutex);
    size_t ix = 0;
    const char *key;
    struct metadata_value *value;
    while (hash_table_walk(
            metadata_map, &ix, (const void **) &key, (void *) &value))
        if (value->update_index > report_index)
        {
            char string[MAX_RESULT_LENGTH];
            snprintf(string, sizeof(string),
                "*SPECIAL.%s=%s", key, value->value ?: "");
            result->write_many(result->write_context, string);
        }
    UNLOCK(mutex);
}
