#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "error.h"
#include "hardware.h"
#include "hashtable.h"
#include "config_server.h"
#include "fields.h"
#include "types.h"

#include "mux_lookup.h"



/* These manage the conversion between bit and position multiplexer register
 * settings and sensible user readable names.
 *
 * For multiplexer selections we convert the register value to and from a
 * multiplexer name and read and write the corresponding register.  The main
 * complication is that we need to map multiplexers to indexes. */

struct mux_lookup {
    size_t length;                  // Number of mux entries
    struct hash_table *numbers;     // Lookup converting name to index
    char **names;                   // Array of mux entry names
};



struct mux_lookup bit_mux_lookup;
struct mux_lookup pos_mux_lookup;


/*****************************************************************************/
/* Multiplexer name lookup. */


/* Creates Mux lookup to support the given number of valid indexes. */
static void mux_lookup_initialise(struct mux_lookup *lookup, size_t length)
{
    *lookup = (struct mux_lookup) {
        .length = length,
        .numbers = hash_table_create(false),
        .names = calloc(length, sizeof(char *)),
    };
}


static void mux_lookup_destroy(struct mux_lookup *lookup)
{
    if (lookup->numbers)
        hash_table_destroy(lookup->numbers);
    if (lookup->names)
    {
        for (unsigned int i = 0; i < lookup->length; i ++)
            free(lookup->names[i]);
        free(lookup->names);
    }
}


/* Add name<->index mapping, called during configuration file parsing. */
static error__t mux_lookup_insert(
    struct mux_lookup *lookup, unsigned int ix, const char *name)
{
    return
        TEST_OK_(ix < lookup->length, "Index %u out of range", ix)  ?:
        TEST_OK_(!lookup->names[ix], "Index %u already assigned", ix)  ?:
        DO(lookup->names[ix] = strdup(name))  ?:
        TEST_OK_(!hash_table_insert(
            lookup->numbers, lookup->names[ix], (void *) (uintptr_t) ix),
            "Duplicate mux name %s", name);
}


/* During register definition parsing add index<->name conversions. */
error__t add_mux_indices(
    struct mux_lookup *lookup, struct field *field,
    unsigned int count, const unsigned int indices[])
{
    /* Add mux entries for our instances. */
    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < count; i ++)
    {
        char name[MAX_NAME_LENGTH];
        format_field_name(name, sizeof(name), field, NULL, i, '\0');
        error = mux_lookup_insert(lookup, indices[i], name);
    }
    return error;
}


/* Converts field name to corresponding index. */
static error__t mux_lookup_name(
    struct mux_lookup *lookup, const char *name, unsigned int *ix)
{
    void *value;
    return
        TEST_OK_(hash_table_lookup_bool(lookup->numbers, name, &value),
            "Mux selector not known")  ?:
        DO(*ix = (unsigned int) (uintptr_t) value);
}


/* Converts register index to multiplexer name, or returns error if an invalid
 * value is read. */
static error__t mux_lookup_index(
    struct mux_lookup *lookup, unsigned int ix, char result[], size_t length)
{
    return
        TEST_OK_(ix < lookup->length, "Index out of range")  ?:
        TEST_OK_(lookup->names[ix], "Mux name unassigned")  ?:
        TEST_OK(strlen(lookup->names[ix]) < length)  ?:
        DO(strcpy(result, lookup->names[ix]));
}


const char *mux_lookup_get_name(struct mux_lookup *lookup, unsigned int ix)
{
    return lookup->names[ix];
}


size_t mux_lookup_get_length(struct mux_lookup *lookup)
{
    return lookup->length;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* bit_mux and pos_mux type methods. */

static error__t bit_mux_format(
    void *type_data, unsigned int number,
    unsigned int value, char result[], size_t length)
{
    return mux_lookup_index(&bit_mux_lookup, value, result, length);
}

static error__t pos_mux_format(
    void *type_data, unsigned int number,
    unsigned int value, char result[], size_t length)
{
    return mux_lookup_index(&pos_mux_lookup, value, result, length);
}


static error__t bit_mux_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
{
    return mux_lookup_name(&bit_mux_lookup, string, value);
}

static error__t pos_mux_parse(
    void *type_data, unsigned int number,
    const char *string, unsigned int *value)
{
    return mux_lookup_name(&pos_mux_lookup, string, value);
}


const struct type_methods bit_mux_type_methods =
    { "bit_mux", .parse = bit_mux_parse, .format = bit_mux_format };
const struct type_methods pos_mux_type_methods =
    { "pos_mux", .parse = pos_mux_parse, .format = pos_mux_format };


/*****************************************************************************/
/* Startup and shutdown. */

void initialise_mux_lookup(void)
{
    mux_lookup_initialise(&bit_mux_lookup, BIT_BUS_COUNT);
    mux_lookup_initialise(&pos_mux_lookup, POS_BUS_COUNT);
}


void terminate_mux_lookup(void)
{
    mux_lookup_destroy(&bit_mux_lookup);
    mux_lookup_destroy(&pos_mux_lookup);
}
