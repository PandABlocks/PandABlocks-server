/* Multiplexer lookup tables. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "hashtable.h"
#include "parse.h"

#include "mux_lookup.h"


#define BIT_MUX_COUNT       128
#define POSITION_MUX_COUNT  64



/* These manage the conversion between bit and position multiplexer register
 * settings and sensible user readable names.
 *
 * For multiplexer selections we convert the register value to and from a
 * multiplexer name and read and write the corresponding register.  The main
 * complication is that we need to map multiplexers to indexes. */

struct mux_lookup {
    size_t length;                  // Number of mux entries
    struct hash_table *numbers;     // Lookup converting name to index
    char *names[];                  // Array of mux entry names
};


/* Creates Mux lookup to support the given number of valid indexes. */
static struct mux_lookup *mux_lookup_create(size_t length)
{
    size_t array_length = length * sizeof(const char *);
    struct mux_lookup *lookup =
        malloc(sizeof(struct mux_lookup) + array_length);
    lookup->length = length;
    lookup->numbers = hash_table_create(false);
    memset(lookup->names, 0, array_length);
    return lookup;
}


static void mux_lookup_destroy(struct mux_lookup *lookup)
{
    hash_table_destroy(lookup->numbers);
    for (unsigned int i = 0; i < lookup->length; i ++)
        free(lookup->names[i]);
    free(lookup);
}


/* Add name<->index mapping, called during configuration file parsing. */
error__t mux_lookup_insert(
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


error__t mux_lookup_name(
    struct mux_lookup *lookup, const char *name, unsigned int *ix)
{
    void *value;
    return
        TEST_OK_(hash_table_lookup_bool(lookup->numbers, name, &value),
            "Mux selector not known")  ?:
        DO(*ix = (unsigned int) (uintptr_t) value);
}


/* Converts register index to multiplexer name, or returns a placeholder if an
 * invalid value is read. */
error__t mux_lookup_index(
    struct mux_lookup *lookup, unsigned int ix, const char **name)
{
    return
        TEST_OK_(ix < lookup->length, "Index out of range")  ?:
        TEST_OK_(lookup->names[ix], "Mux name unassigned")  ?:
        DO(*name = lookup->names[ix]);
}


struct mux_lookup *bit_mux_lookup = NULL;
struct mux_lookup *pos_mux_lookup = NULL;

error__t initialise_mux_lookup(void)
{
    /* Block input multiplexer maps.  These are initialised by each
     * {bit,pos}_out field as it is loaded. */
    bit_mux_lookup = mux_lookup_create(BIT_MUX_COUNT);
    pos_mux_lookup = mux_lookup_create(POSITION_MUX_COUNT);

    return ERROR_OK;
}


void terminate_mux_lookup(void)
{
    if (bit_mux_lookup)
        mux_lookup_destroy(bit_mux_lookup);
    if (pos_mux_lookup)
        mux_lookup_destroy(pos_mux_lookup);
}
