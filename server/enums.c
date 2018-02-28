/* Implementation of enums. */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "error.h"
#include "hashtable.h"
#include "parse.h"
#include "config_server.h"
#include "types.h"
#include "attributes.h"

#include "enums.h"


/* Somewhere around this number it's quicker to use a hash table than to do a
 * linear search. */
#define HASH_TABLE_THRESHOLD    3


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Generic enumeration interface. */


struct enumeration {
    bool dynamic;               // Set if enum_set can be populated dynamically
    bool direct_index;          // Set if number matches position in enum_set
    struct enum_set enum_set;   // Array of strings and associated number
    struct hash_table *map;     // String to index, optional
};


static const char *binary_search(
    const struct enum_set *enum_set, unsigned int value)
{
    size_t left = 0;
    size_t right = enum_set->count - 1;
    while (right > left)
    {
        size_t centre = (left + right) / 2;
        if (value <= enum_set->enums[centre].value)
            right = centre;
        else
            left = centre + 1;
    }
    if (enum_set->enums[left].value == value)
        return enum_set->enums[left].name;
    else
        return NULL;
}


const char *enum_index_to_name(
    const struct enumeration *enumeration, unsigned int value)
{
    if (enumeration->direct_index)
    {
        if (value < enumeration->enum_set.count)
            return enumeration->enum_set.enums[value].name;
        else
            return NULL;
    }
    else
        return binary_search(&enumeration->enum_set, value);
}


/* For sufficiently small lists a linear search is actually cheaper than a hash
 * table lookup. */
static const struct enum_entry *linear_search(
    const struct enum_set *enum_set, const char *name)
{
    for (size_t i = 0; i < enum_set->count; i ++)
    {
        const struct enum_entry *entry = &enum_set->enums[i];
        if (entry->name  &&  strcmp(entry->name, name) == 0)
            return entry;
    }
    return NULL;
}


bool enum_name_to_index(
    const struct enumeration *enumeration,
    const char *name, unsigned int *value)
{
    /* If we have a hash table use that, otherwise it'll have to be a linear
     * search through the keys. */
    const struct enum_entry *entry =
        enumeration->map ?
            hash_table_lookup(enumeration->map, name)
        :
            linear_search(&enumeration->enum_set, name);
    if (entry)
        *value = entry->value;
    return entry;
}


bool walk_enumerations(
    const struct enumeration *enumeration,
    size_t *ix, struct enum_entry *entry_out)
{
    for (; *ix < enumeration->enum_set.count; (*ix) ++)
    {
        const struct enum_entry *entry = &enumeration->enum_set.enums[*ix];
        if (entry->name)
        {
            *entry_out = *entry;
            *ix += 1;
            return true;
        }
    }
    return false;
}


/* This checks whether a static enumeration set is suitable for direct indexing:
 * this is possible if every populated entry has the same value as its position
 * in the array. */
static bool check_direct_index(const struct enum_set *enum_set)
{
    for (size_t i = 0; i < enum_set->count; i ++)
    {
        const struct enum_entry *entry = &enum_set->enums[i];
        if (entry->name  &&  entry->value != i)
            return false;
    }
    return true;
}


/* This checks whether a static enumeration is suitable for binary search.  In
 * this case all entries must be populated and in strictly ascending order. */
static bool check_binary_search(const struct enum_set *enum_set)
{
    if (enum_set->count == 0)
        return false;
    else
    {
        if (!enum_set->enums[0].name)
            return false;
        unsigned int value = enum_set->enums[0].value;
        for (size_t i = 1; i < enum_set->count; i ++)
        {
            const struct enum_entry *entry = &enum_set->enums[i];
            if (!entry->name  ||  entry->value <= value)
                return false;
            value = entry->value;
        }
        return true;
    }
}


const struct enumeration *create_static_enumeration(
    const struct enum_set *enum_set)
{
    struct enumeration *enumeration = malloc(sizeof(struct enumeration));
    *enumeration = (struct enumeration) {
        .dynamic = false,
        .direct_index = check_direct_index(enum_set),
        .enum_set = *enum_set,
    };

    /* At present it's a fatal static error if the enum set can't be indexed
     * directly or by binary search. */
    ASSERT_OK(enumeration->direct_index  ||  check_binary_search(enum_set));

    if (enum_set->count > HASH_TABLE_THRESHOLD)
    {
        enumeration->map = hash_table_create(false);
        for (size_t i = 0; i < enum_set->count; i ++)
        {
            const struct enum_entry *entry = &enum_set->enums[i];
            ASSERT_OK(!hash_table_insert_const(
                enumeration->map, entry->name, entry));
        }
    }
    return enumeration;
}


struct enumeration *create_dynamic_enumeration(size_t count)
{
    struct enumeration *enumeration = malloc(sizeof(struct enumeration));
    *enumeration = (struct enumeration) {
        .dynamic = true,
        .direct_index = true,
        .enum_set = {
            .enums = calloc(count, sizeof(struct enum_entry)),
            .count = count,
        },
    };
    if (count > HASH_TABLE_THRESHOLD)
        enumeration->map = hash_table_create(false);
    return enumeration;
}


error__t add_enumeration(
    struct enumeration *enumeration, const char *string, unsigned int ix)
{
    ASSERT_OK(enumeration->dynamic);

    char *string_copy = strdup(string);
    const struct enum_entry *entry = &enumeration->enum_set.enums[ix];
    return
        TEST_OK_(ix < enumeration->enum_set.count,
            "Enumeration index out of range")  ?:
        TEST_OK_(entry->name == NULL,
            "Repeated enumeration index")  ?:
        /* Add the string to our hash table, if we're using one, otherwise at
         * least make sure the key isn't already present. */
        TEST_OK_(enumeration->map
            ?
                !hash_table_insert_const(enumeration->map, string_copy, entry)
            :
                !linear_search(&enumeration->enum_set, string_copy),
            "Enumeration value already in use")  ?:
        DO(
            *CAST_FROM_TO(
                const struct enum_entry *, struct enum_entry *, entry) =
                (struct enum_entry) {
                    .name = string_copy,
                    .value = ix,
                });
}


void destroy_enumeration(const struct enumeration *enumeration)
{
    if (enumeration->dynamic)
    {
        for (size_t i = 0; i < enumeration->enum_set.count; i ++)
            free(CAST_FROM_TO(
                const char *, char *, enumeration->enum_set.enums[i].name));
        free(CAST_FROM_TO(
            const struct enum_entry *, struct enum_entry *,
            enumeration->enum_set.enums));
    }
    if (enumeration->map)
        hash_table_destroy(enumeration->map);
    free(CAST_FROM_TO(const void *, void *, enumeration));
}


void write_enum_labels(
    const struct enumeration *enumeration, struct connection_result *result)
{
    struct enum_entry entry;
    size_t ix = 0;
    while (walk_enumerations(enumeration, &ix, &entry))
        result->write_many(result->write_context, entry.name);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* enums type */


/* Adds a single enumeration label to the enumeration set. */
static error__t enum_add_label(
    void *type_data, const char **string, struct indent_parser *parser)
{
    unsigned int ix;
    return
        parse_uint(string, &ix)  ?:
        parse_whitespace(string)  ?:
        add_enumeration(type_data, *string, ix)  ?:
        /* Skip to end of string to complete the parse. */
        DO(*string = strchr(*string, '\0'));
}


/* Starts the loading of an enumeration. */
static error__t enum_init(
    const char **string, unsigned int count, void **type_data,
    struct indent_parser *parser)
{
    unsigned int enum_count;
    return
        parse_whitespace(string)  ?:
        parse_uint(string, &enum_count)  ?:
        DO(
            *type_data = create_dynamic_enumeration(enum_count);
            /* We expect the field definition to be followed by enumeration
             * definitions. */
            *parser = (struct indent_parser) {
                .context = *type_data,
                .parse_line = enum_add_label,
            });
}


/* Called during shutdown to release allocated resources. */
static void enum_destroy(void *type_data, unsigned int count)
{
    destroy_enumeration(type_data);
}


/* Parses valid enumeration into corresponding value, otherwise error. */
error__t parse_enumeration(
    const struct enumeration *enumeration,
    const char *string, unsigned int *value)
{
    return TEST_OK_(
        enum_name_to_index(enumeration, string, value), "Label not found");
}

static error__t enum_parse(
    void *type_data, unsigned int number,
    const char **string, unsigned int *value)
{
    return
        parse_enumeration(type_data, *string, value)  ?:
        DO(*string += strlen(*string));
}


/* Formats valid value into enumeration string, otherwise error. */
error__t format_enumeration(
    const struct enumeration *enumeration,
    unsigned int value, char string[], size_t length)
{
    const char *label = enum_index_to_name(enumeration, value);
    return
        TEST_OK_(label, "No label for value")  ?:
        format_string(string, length, "%s", label);
}


static error__t enum_format(
    void *type_data, unsigned int number,
    unsigned int value, char string[], size_t length)
{
    const struct enumeration *enumeration = type_data;
    return format_enumeration(enumeration, value, string, length);
}


const struct enumeration *enum_get_enumeration(void *type_data)
{
    return type_data;
}


const struct type_methods enum_type_methods = {
    "enum",
    .init = enum_init, .destroy = enum_destroy,
    .parse = enum_parse, .format = enum_format,
    .get_enumeration = enum_get_enumeration,
};
