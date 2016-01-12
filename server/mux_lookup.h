/* Conversion between index and name, used for bit_mux and pos_mux types. */

/* Initialises bit_mux and pos_mux lookup structures. */
void initialise_mux_lookup(void);

/* Releases internal resources. */
void terminate_mux_lookup(void);


struct mux_lookup;


/* Adds given mux lookup indicies for given field. */
error__t add_mux_indices(
    struct mux_lookup *lookup, struct field *field,
    unsigned int count, const unsigned int indices[]);


/* Type interface for bit_mux and pos_mux. */
extern const struct type_methods bit_mux_type_methods;
extern const struct type_methods pos_mux_type_methods;


/* Access to mux. */

/* Returns name at given index or NULL if no name.  ix must be in range. */
const char *mux_lookup_get_name(struct mux_lookup *lookup, unsigned int ix);

/* Returns number of entries in lookup. */
size_t mux_lookup_get_length(struct mux_lookup *lookup);


/* External declarations of mux lookups. */
extern struct mux_lookup bit_mux_lookup;
extern struct mux_lookup pos_mux_lookup;
