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

#define INITIAL_CAPACITY        4


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Generic enumeration interface. */


struct enumeration {
    size_t set_capacity;        // Capacity of set for dynamic allocation
    struct enum_set enum_set;   // Array of strings and associated number
    struct hash_table *map;     // String to index, optional
};


/* Returns index of largest enum set less than or equal to value, or returns
 * length of set if all indices less than value. */
static size_t binary_search(
    const struct enum_set *enum_set, size_t value)
{
    size_t left = 0;
    size_t right = enum_set->count;
    while (left < right)
    {
        size_t centre = (left + right) / 2;
        if (value <= enum_set->enums[centre].value)
            right = centre;
        else
            left = centre + 1;
    }
    return left;
}


const char *enum_index_to_name(
    const struct enumeration *enumeration, unsigned int value)
{
    const struct enum_set *enum_set = &enumeration->enum_set;
    size_t ix = binary_search(enum_set, value);
    if (ix < enum_set->count  &&  enum_set->enums[ix].value == value)
        return enum_set->enums[ix].name;
    else
        return NULL;
}


/* For sufficiently small lists a linear search is actually cheaper than a hash
 * table lookup. */
static bool linear_search(
    const struct enum_set *enum_set, const char *name, unsigned int *value)
{
    for (size_t i = 0; i < enum_set->count; i ++)
    {
        const struct enum_entry *entry = &enum_set->enums[i];
        if (entry->name  &&  strcmp(entry->name, name) == 0)
        {
            *value = entry->value;
            return true;
        }
    }
    return false;
}


bool enum_name_to_index(
    const struct enumeration *enumeration,
    const char *name, unsigned int *value)
{
    /* If we have a hash table use that, otherwise it'll have to be a linear
     * search through the keys. */
    if (enumeration->map)
    {
        void *hashed_value;
        bool found = hash_table_lookup_bool(
            enumeration->map, name, &hashed_value);
        /* An annoying impedance mismatch between the hash table values and what
         * we actually want to return.  Never mind... */
        *value = (unsigned int) (uintptr_t) hashed_value;
        return found;
    }
    else
        return linear_search(&enumeration->enum_set, name, value);
}


/* This checks whether a static enumeration is suitable for binary search.  In
 * this case all entries must be in strictly ascending order. */
static bool check_sorted_enum_set(const struct enum_set *enum_set)
{
    if (enum_set->count == 0)
        return true;
    else
    {
        unsigned int value = enum_set->enums[0].value;
        for (size_t i = 1; i < enum_set->count; i ++)
        {
            const struct enum_entry *entry = &enum_set->enums[i];
            if (entry->value <= value)
                return false;
            value = entry->value;
        }
        return true;
    }
}


/* Called when the set size grows large enough that a hash table is better than
 * a straight linear search. */
static void create_hash_table(struct enumeration *enumeration)
{
    const struct enum_set *enum_set = &enumeration->enum_set;
    enumeration->map = hash_table_create(false);
    for (size_t i = 0; i < enum_set->count; i ++)
    {
        const struct enum_entry *entry = &enum_set->enums[i];
        ASSERT_OK(!hash_table_insert_const(
            enumeration->map, entry->name, (void *) (uintptr_t) entry->value));
    }
}


const struct enumeration *create_static_enumeration(
    const struct enum_set *enum_set)
{
    /* The enumeration set passed in must be sorted. */
    ASSERT_OK(check_sorted_enum_set(enum_set));

    struct enumeration *enumeration = malloc(sizeof(struct enumeration));
    *enumeration = (struct enumeration) {
        .enum_set = *enum_set,
    };

    if (enum_set->count > HASH_TABLE_THRESHOLD)
        create_hash_table(enumeration);

    return enumeration;
}


struct enumeration *create_dynamic_enumeration(void)
{
    struct enumeration *enumeration = malloc(sizeof(struct enumeration));
    *enumeration = (struct enumeration) {
        .set_capacity = INITIAL_CAPACITY,
        .enum_set = {
            .enums = calloc(INITIAL_CAPACITY, sizeof(struct enum_entry)),
            .count = 0,
        },
    };
    return enumeration;
}


/* Helper for add_enumeration to complete adding of enumeration at given index
 * once validation has been completed. */
static void insert_new_enum(
    struct enumeration *enumeration, struct enum_entry *entry, size_t ix)
{
    struct enum_set *enum_set = &enumeration->enum_set;

    /* We're basically doing an insertion sort here! */
    size_t to_move = enum_set->count - ix;
    memmove(
        &enum_set->enums[ix + 1], &enum_set->enums[ix],
        to_move * sizeof(struct enum_entry));
    enum_set->enums[ix] = *entry;
    enum_set->count += 1;

    if (enumeration->map)
        ASSERT_OK(!hash_table_insert_const(
            enumeration->map, entry->name, (void *) (uintptr_t) entry->value));
}


error__t add_enumeration(
    struct enumeration *enumeration, const char *string, unsigned int value)
{
    ASSERT_OK(enumeration->set_capacity > 0);

    /* Prepare the new enumeration entry. */
    struct enum_entry entry = {
        .name = strdup(string),
        .value = value,
    };

    /* Ensure we have enough capacity in the enum set for our new value. */
    struct enum_set *enum_set = &enumeration->enum_set;
    if (enum_set->count >= enumeration->set_capacity)
    {
        enumeration->set_capacity *= 2;
        enum_set->enums = realloc(
            enum_set->enums,
            enumeration->set_capacity * sizeof(struct enum_entry));
    }

    /* Create hash table if appropriate.  If so we need to copy everything from
     * the enum_set into the hash table. */
    if (!enumeration->map  &&  enum_set->count >= HASH_TABLE_THRESHOLD)
        create_hash_table(enumeration);

    /* Now figure out where to insert the new entry and ensure it's new. */
    size_t ix = binary_search(enum_set, entry.value);
    size_t to_move = enum_set->count - ix;
    unsigned int test_ix;
    return
        TEST_OK_(
            to_move == 0  ||  entry.value < enum_set->enums[ix].value,
            "Repeated enumeration index")  ?:
        TEST_OK_(!enum_name_to_index(enumeration, entry.name, &test_ix),
            "Repeated enumeration name")  ?:
        DO(insert_new_enum(enumeration, &entry, ix));
}


void destroy_enumeration(const struct enumeration *enumeration_)
{
    struct enumeration *enumeration = CAST_FROM_TO(
        const struct enumeration *, struct enumeration *, enumeration_);
    if (enumeration->set_capacity > 0)
    {
        struct enum_set *enum_set = &enumeration->enum_set;
        for (size_t i = 0; i < enum_set->count; i ++)
            free(CAST_FROM_TO(
                const char *, char *, enum_set->enums[i].name));
        free(enumeration->enum_set.enums);
    }
    if (enumeration->map)
        hash_table_destroy(enumeration->map);
    free(enumeration);
}


void write_enum_labels(
    const struct enumeration *enumeration, struct connection_result *result)
{
    const struct enum_set *enum_set = &enumeration->enum_set;
    for (size_t i = 0; i < enum_set->count; i ++)
        result->write_many(result->write_context, enum_set->enums[i].name);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* enums type */


/* Adds a single enumeration label to the enumeration set. */
static error__t enum_add_label(
    void *type_data, const char **string, struct indent_parser *parser)
{
    unsigned int ix;
    const char *enum_string;
    return
        parse_uint(string, &ix)  ?:
        parse_whitespace(string)  ?:
        parse_utf8_string(string, &enum_string)  ?:
        add_enumeration(type_data, enum_string, ix);
}


void set_enumeration_parser(
    struct enumeration *enumeration, struct indent_parser *parser)
{
    *parser = (struct indent_parser) {
        .context = enumeration,
        .parse_line = enum_add_label,
    };
}


/* Starts the loading of an enumeration. */
static error__t enum_init(
    const char **string, unsigned int count, void **type_data,
    struct indent_parser *parser)
{
    struct enumeration *enumeration = create_dynamic_enumeration();
    *type_data = enumeration;
    set_enumeration_parser(enumeration, parser);
    return ERROR_OK;
}


/* Called during shutdown to release allocated resources. */
static void enum_destroy(void *type_data, unsigned int count)
{
    destroy_enumeration(type_data);
}


static error__t enum_parse(
    void *type_data, unsigned int number,
    const char **string, unsigned int *value)
{
    struct enumeration *enumeration = type_data;
    return
        TEST_OK_(
            enum_name_to_index(enumeration, *string, value),
            "Invalid enumeration value")  ?:
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


static const struct enumeration *enum_get_enumeration(void *type_data)
{
    return type_data;
}


const struct type_methods enum_type_methods = {
    "enum",
    .init = enum_init, .destroy = enum_destroy,
    .parse = enum_parse, .format = enum_format,
    .get_enumeration = enum_get_enumeration,
};
