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
    METADATA_STRING,        // Single string
    METADATA_MULTILINE,     // Multiline value
    METADATA_CONSTANT,      // Read-only constant string
};

struct metadata_value {
    enum metadata_type type;
    char *value;
    uint64_t update_index;
    pthread_mutex_t mutex;
};


static struct hash_table *metadata_map;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation and shutdown. */


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
        IF_ELSE(strcmp(type_name, "constant") == 0,
            DO(*type = METADATA_CONSTANT),
        //else
            FAIL_("Invalid metadata type"))));
}


static error__t parse_metadata_constant(
    struct metadata_value *value, const char **line)
{
    const char *string;
    return
        parse_whitespace(line)  ?:
        parse_char(line, '=')  ?:
        parse_utf8_string(line, &string)  ?:
        /* For constants force the update index to 0 to suppress change
         * reporting. */
        DO( value->value = strdup(string);
            value->update_index = 0);
}


error__t add_metadata_key(const char *key, const char **line)
{
    struct metadata_value *value = malloc(sizeof(struct metadata_value));
    *value = (struct metadata_value) {
        .update_index = 1,
        .mutex = (pthread_mutex_t) PTHREAD_MUTEX_INITIALIZER,
    };
    char type_name[MAX_NAME_LENGTH];
    return
        parse_name(line, type_name, sizeof(type_name))  ?:
        lookup_type(type_name, &value->type)  ?:
        IF(value->type == METADATA_CONSTANT,
            parse_metadata_constant(value, line))  ?:
        TEST_OK_(hash_table_insert(metadata_map, key, value) == 0,
            "Metadata key %s repeated", key);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Metadata field access. */


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
        case METADATA_MULTILINE:
            /* Multi-line data is stored as a sequence of null terminated
             * strings with a final empty string to terminate. */
            for (const char *string = value->value; string  &&  *string;
                    string += strlen(string) + 1)
                result->write_many(result->write_context, string);
            result->response = RESPONSE_MANY;
            return ERROR_OK;

        case METADATA_STRING:
        case METADATA_CONSTANT:
            return format_one_result(result, "%s", value->value ?: "");

        default:
            ASSERT_FAIL();
    }
}


error__t get_metadata_value(const char *key, struct connection_result *result)
{
    struct metadata_value *value = hash_table_lookup(metadata_map, key);
    return
        TEST_OK_(value, "Metadata key %s not found", key)  ?:
        ERROR_WITH_MUTEX(value->mutex, format_value(value, result));
}


error__t put_metadata_value(const char *key, const char *string)
{
    struct metadata_value *value = hash_table_lookup(metadata_map, key);
    error__t error =
        TEST_OK_(value, "Metadata key %s not found", key)  ?:
        TEST_OK_(value->type == METADATA_STRING, "Cannot write to this field");
    if (!error)
    {
        WITH_MUTEX(value->mutex)
        {
            free(value->value);
            value->value = strdup(string);
            value->update_index = get_change_index();
        }
    }
    return error;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Table access. */

#define MIN_LENGTH      1024U

struct multiline_writer {
    struct metadata_value *value;
    char *text;
    size_t length;
    size_t max_length;
};


static void append_string(struct multiline_writer *writer, const char *string)
{
    size_t line_length = strlen(string) + 1;
    if (writer->length + line_length > writer->max_length)
    {
        size_t new_length = MAX(MIN_LENGTH, 2 * writer->max_length);
        writer->text = realloc(writer->text, new_length);
        writer->max_length = new_length;
    }
    memcpy(writer->text + writer->length, string, line_length);
    writer->length += line_length;
}


static error__t write_multiline_put(void *context, const char *line)
{
    const char *string;
    return
        parse_utf8_string(&line, &string)  ?:
        DO(append_string(context, string));
}


static error__t close_multiline_put(void *context, bool write_ok)
{
    struct multiline_writer *writer = context;
    if (write_ok)
    {
        append_string(writer, "");
        struct metadata_value *value = writer->value;
        WITH_MUTEX(value->mutex)
        {
            free(value->value);
            value->value = realloc(writer->text, writer->length);
            free(writer);
            value->update_index = get_change_index();
        }
    }
    else
    {
        free(writer->text);
        free(writer);
    }
    return ERROR_OK;
}


error__t put_metadata_table(const char *key, struct put_table_writer *writer)
{
    struct metadata_value *value = hash_table_lookup(metadata_map, key);
    error__t error =
        TEST_OK_(value, "Metadata key %s not found", key)  ?:
        TEST_OK_(value->type == METADATA_MULTILINE, "Not a multi-line field");

    if (!error)
    {
        struct multiline_writer *context =
            malloc(sizeof(struct multiline_writer));
        *context = (struct multiline_writer) {
            .value = value,
        };
        *writer = (struct put_table_writer) {
            .context = context,
            .write = write_multiline_put,
            .close = close_multiline_put,
        };
    }
    return error;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Change set support. */


bool check_metadata_change_set(uint64_t report_index)
{
    bool changed = false;
    size_t ix = 0;
    struct metadata_value *value;
    while (!changed  &&
        hash_table_walk(metadata_map, &ix, NULL, (void *) &value))
    {
        WITH_MUTEX(value->mutex)
            if (value->update_index > report_index)
                changed = true;
    }
    return changed;
}


void generate_metadata_change_set(
    struct connection_result *result, uint64_t report_index,
    bool print_table)
{
    size_t ix = 0;
    const char *key;
    struct metadata_value *value;
    while (hash_table_walk(
            metadata_map, &ix, (const void **) &key, (void *) &value))
    {
        bool changed;
        WITH_MUTEX(value->mutex)
            changed = value->update_index > report_index;

        if (changed)
        {
            switch (value->type)
            {
                case METADATA_MULTILINE:
                    format_many_result(result, "*METADATA.%s<", key);
                    if (print_table)
                    {
                        error_report(get_metadata_value(key, result));
                        result->write_many(result->write_context, "");
                    }
                    break;

                case METADATA_STRING:
                    WITH_MUTEX(value->mutex)
                        snprintf(result->string, result->length,
                            "*METADATA.%s=%s", key, value->value ?: "");
                    /* We avoid writing while holding the lock. */
                    result->write_many(result->write_context, result->string);
                    break;

                case METADATA_CONSTANT:
                    /* This should never occur. */
                    break;
            }
        }
    }
}
