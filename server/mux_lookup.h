/* Multiplexer lookup.
 *
 * Functions for converting between multiplexer indexes and field names. */

struct mux_lookup;


void mux_lookup_insert(
    struct mux_lookup *lookup, unsigned int ix, const char *name);

error__t mux_lookup_name(
    struct mux_lookup *lookup, const char *name, unsigned int *ix);

const char *mux_lookup_index(struct mux_lookup *lookup, unsigned int ix);


extern struct mux_lookup *bit_mux_lookup;   // bit_{in,out} fields
extern struct mux_lookup *pos_mux_lookup;   // pos_{in,out} fields

error__t initialise_mux_lookup(void);
