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
#include "parse.h"
#include "config_server.h"
#include "locking.h"

#include "metadata.h"


enum metadata_type {
    METADATA_STRING,
    METADATA_MULTILINE,
};

struct metadata_value {
    enum metadata_type type;
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


static error__t lookup_type(const char *type_name, enum metadata_type *type)
{
    return
        IF_ELSE(strcmp(type_name, "string") == 0,
            DO(*type = METADATA_STRING),
        //else
        IF_ELSE(strcmp(type_name, "multiline") == 0,
            DO(*type = METADATA_MULTILINE),
        //else
            FAIL_("Invalid metadata type")));
}


error__t add_metadata_key(const char *key, const char **line)
{
    struct metadata_value *value = malloc(sizeof(struct metadata_value));
    *value = (struct metadata_value) {
        .update_index = 1,
    };
    char type_name[MAX_NAME_LENGTH];
    return
        parse_whitespace(line)  ?:
        parse_name(line, type_name, sizeof(type_name))  ?:
        lookup_type(type_name, &value->type)  ?:
        TEST_OK_(hash_table_insert(metadata_map, key, value) == 0,
            "Metadata key %s repeated", key);
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


static error__t format_value(
    struct metadata_value *value, struct connection_result *result)
{
    switch (value->type)
    {
        case METADATA_STRING:
            return format_one_result(result, "%s", value->value ?: "");
        case METADATA_MULTILINE:
            /* Multi-line data is stored as a sequence of null terminated
             * strings with a final empty string to terminate. */
            for (const char *string = value->value; string  &&  *string;
                    string += strlen(string) + 1)
                result->write_many(result->write_context, string);
            result->response = RESPONSE_MANY;
            return ERROR_OK;
        default:    ASSERT_FAIL();
    }
}


error__t get_metadata_value(const char *key, struct connection_result *result)
{
    struct metadata_value *value = hash_table_lookup(metadata_map, key);
    return
        TEST_OK_(value, "Metadata key %s not found", key)  ?:
        WITH_LOCK(mutex, format_value(value, result));
}


error__t put_metadata_value(const char *key, const char *string)
{
    struct metadata_value *value = hash_table_lookup(metadata_map, key);
    error__t error =
        TEST_OK_(value, "Metadata key %s not found", key)  ?:
        TEST_OK_(value->type == METADATA_STRING, "Cannot write this field");
    if (!error)
    {
        LOCK(mutex);
        free(value->value);
        value->value = strdup(string);
        value->update_index = get_change_index();
        UNLOCK(mutex);
    }
    return error;
}


error__t put_metadata_table(const char *key, struct put_table_writer *writer)
{
    return FAIL_("Not implemented");
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
    struct connection_result *result, uint64_t report_index,
    bool print_table)
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
            switch (value->type)
            {
                case METADATA_STRING:
                    snprintf(string, sizeof(string),
                        "*METADATA.%s=%s", key, value->value ?: "");
                    break;
                case METADATA_MULTILINE:
                    snprintf(string, sizeof(string), "*METADATA.%s<", key);
                    break;
                default:    ASSERT_FAIL();
            }
            result->write_many(result->write_context, string);
        }
    UNLOCK(mutex);
}
