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
#define HASH_TABLE_THRESHOLD    4


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
            left = centre;
    }
    if (enum_set->enums[left].value == value)
        return enum_set->enums[left].name;
    else
        return NULL;
}


const char *enum_index_to_name(
    struct enumeration *enumeration, unsigned int value)
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
    struct enum_set *enum_set, const char *name)
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
    struct enumeration *enumeration, const char *name, unsigned int *value)
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
    struct enumeration *enumeration, size_t *ix,
    const struct enum_entry **entry_out)
{
    for (; *ix < enumeration->enum_set.count; (*ix) ++)
    {
        const struct enum_entry *entry = &enumeration->enum_set.enums[*ix];
        if (entry->name)
        {
            *entry_out = entry;
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
        for (size_t i = 0; i < enum_set->count; i ++)
        {
            const struct enum_entry *entry = &enum_set->enums[i];
            if (!entry->name  ||  entry->value <= value)
                return false;
            value = entry->value;
        }
        return true;
    }
}


struct enumeration *create_static_enumeration(const struct enum_set *enum_set)
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
    error__t error =
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
            "Enumeration value already in use");

    if (error)
        free(string_copy);
    else
        *CAST_FROM_TO(const struct enum_entry *, struct enum_entry *, entry) =
            (struct enum_entry) {
                .name = string_copy,
                .value = ix,
            };
    return error;
}


void destroy_enumeration(struct enumeration *enumeration)
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
    free(enumeration);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */


struct enum_state {
    struct hash_table *map;     // String to index
    unsigned int count;         // Length of strings array
    char *strings[];
};


static struct enum_state *create_enum_state(unsigned int enum_count)
{
    struct enum_state *state =
        calloc(1, sizeof(struct enum_state) + enum_count * sizeof(char *));
    state->map = hash_table_create(false);
    state->count = enum_count;
    return state;
}


/* Starts the loading of an enumeration. */
static error__t enum_init(
    const char **string, unsigned int count, void **type_data)
{
    unsigned int enum_count;
    return
        parse_whitespace(string)  ?:
        parse_uint(string, &enum_count)  ?:
        DO(*type_data = create_enum_state(enum_count));
}


/* Called during shutdown to release allocated resources. */
static void enum_destroy(void *type_data, unsigned int count)
{
    struct enum_state *state = type_data;
    hash_table_destroy(state->map);
    for (unsigned int i = 0; i < state->count; i ++)
        free(state->strings[i]);
    free(state);
}


/* Adds a single enumeration label to the enumeration set. */
static error__t enum_add_label(void *type_data, const char **string)
{
    struct enum_state *state = type_data;
    unsigned int ix;
    return
        parse_uint(string, &ix)  ?:
        parse_whitespace(string)  ?:
        TEST_OK_(ix < state->count, "Index out of range")  ?:
        TEST_OK_(**string != '\0', "No label specified")  ?:
        TEST_OK_(state->strings[ix] == NULL, "Reusing index")  ?:
        DO(state->strings[ix] = strdup(*string))  ?:
        TEST_OK_(!hash_table_insert(
            state->map, state->strings[ix], (void *) (intptr_t) ix),
            "Label already in use")  ?:
        /* Skip to end of string to complete the parse. */
        DO(*string = strchr(*string, '\0'));
}


/* Parses valid enumeration into corresponding value, otherwise error. */
static error__t enum_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
{
    struct enum_state *state = type_data;
    void *ix;
    return
        TEST_OK_(hash_table_lookup_bool(state->map, string, &ix),
            "Label not found")  ?:
        DO(*value = (unsigned int) (intptr_t) ix);
}


/* Formats valid value into enumeration string, otherwise error. */
static error__t enum_format(
    void *type_data, unsigned int number,
    unsigned int value, char string[], size_t length)
{
    struct enum_state *state = type_data;
    return
        TEST_OK_(value < state->count, "Index out of range")  ?:
        TEST_OK_(state->strings[value], "No label for value")  ?:
        format_string(string, length, "%s", state->strings[value]);
}


/* Returns list of enumeration values and strings. */
static error__t enum_labels_get(
    void *owner, void *type_data, unsigned int number,
    struct connection_result *result)
{
    struct enum_state *state = type_data;
    for (unsigned int i = 0; i < state->count; i ++)
        if (state->strings[i])
        {
            char string[MAX_RESULT_LENGTH];
            snprintf(string, sizeof(string), "%s", state->strings[i]);
            result->write_many(result->write_context, string);
        }
    result->response = RESPONSE_MANY;
    return ERROR_OK;
}


const struct type_methods enum_type_methods = {
    "enum",
    .init = enum_init, .destroy = enum_destroy,
    .add_attribute_line = enum_add_label,
    .parse = enum_parse, .format = enum_format,
    .attrs = (struct attr_methods[]) {
        { "LABELS", .get_many = enum_labels_get, },
    },
    .attr_count = 1,
};
