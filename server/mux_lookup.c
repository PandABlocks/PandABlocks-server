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
    const char *names[];            // Array of mux entry names
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


/* Add name<->index mapping, called during configuration file parsing. */
void mux_lookup_insert(
    struct mux_lookup *lookup, unsigned int ix, const char *name)
{
    ASSERT_OK(ix < lookup->length);
    lookup->names[ix] = strdup(name);
    hash_table_insert(
        lookup->numbers, lookup->names[ix], (void *) (uintptr_t) ix);
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
const char *mux_lookup_index(struct mux_lookup *lookup, unsigned int ix)
{
    if (ix < lookup->length)
        return lookup->names[ix] ?: "(unassigned)";
    else
        return "(invalid)";
}


struct mux_lookup *bit_mux_lookup;
struct mux_lookup *pos_mux_lookup;

error__t initialise_mux_lookup(void)
{
    /* Block input multiplexer maps.  These are initialised by each
     * {bit,pos}_out field as it is loaded. */
    bit_mux_lookup = mux_lookup_create(BIT_MUX_COUNT);
    pos_mux_lookup = mux_lookup_create(POSITION_MUX_COUNT);

    return ERROR_OK;
}
