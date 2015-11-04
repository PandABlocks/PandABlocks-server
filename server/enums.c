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

#include "enums.h"


struct enum_state {
    struct hash_table *map;     // String to index
    unsigned int count;         // Lenght of strings array
    char *strings[];
};


static struct enum_state *create_enum_state(unsigned int hash_count)
{
    struct enum_state *state =
        calloc(1, sizeof(struct enum_state) + hash_count * sizeof(char *));
    state->map = hash_table_create(false);
    state->count = hash_count;
    return state;
}


/* Starts the loading of an enumeration. */
error__t enum_init(const char **string, unsigned int count, void **type_data)
{
    unsigned int hash_count;
    return
        parse_whitespace(string)  ?:
        parse_uint(string, &hash_count)  ?:
        DO(*type_data = create_enum_state(hash_count));
}


/* Called during shutdown to release allocated resources. */
void enum_destroy(void *type_data, unsigned int count)
{
    struct enum_state *state = type_data;
    hash_table_destroy(state->map);
    for (unsigned int i = 0; i < state->count; i ++)
        free(state->strings[i]);
}


/* Adds a single enumeration label to the enumeration set. */
error__t enum_add_label(void *type_data, const char *string)
{
    struct enum_state *state = type_data;
    unsigned int ix;
    return
        parse_uint(&string, &ix)  ?:
        parse_whitespace(&string)  ?:
        TEST_OK_(ix < state->count, "Index out of range")  ?:
        TEST_OK_(*string != '\0', "No label specified")  ?:
        TEST_OK_(state->strings[ix] == NULL, "Reusing index")  ?:
        DO(state->strings[ix] = strdup(string))  ?:
        TEST_OK_(!hash_table_insert(
            state->map, state->strings[ix], (void *) (intptr_t) ix),
            "Label already in use");
}


/* Parses valid enumeration into corresponding value, otherwise error. */
error__t enum_parse(
    const struct type_context *context,
    const char *string, unsigned int *value)
{
    struct enum_state *state = context->type_data;
    void *ix;
    return
        TEST_OK_(hash_table_lookup_bool(state->map, string, &ix),
            "Label not found")  ?:
        DO(*value = (unsigned int) (intptr_t) ix);
}


/* Formats valid value into enumeration string, otherwise error. */
error__t enum_format(
    const struct type_context *context,
    unsigned int value, char string[], size_t length)
{
    struct enum_state *state = context->type_data;
    return
        TEST_OK_(value < state->count, "Index out of range")  ?:
        TEST_OK_(state->strings[value], "No label for value")  ?:
        DO(snprintf(string, length, "%s", state->strings[value]));
}


/* Returns list of enumeration values and strings. */
error__t enum_labels_get(
    const struct type_attr_context *context,
    const struct connection_result *result)
{
    struct enum_state *state = context->type_data;
    for (unsigned int i = 0; i < state->count; i ++)
        if (state->strings[i])
        {
            char string[MAX_RESULT_LENGTH];
            snprintf(string, sizeof(string), "%d %s", i, state->strings[i]);
            result->write_many(context->connection, string);
        }
    result->write_many_end(context->connection);
    return ERROR_OK;
}
