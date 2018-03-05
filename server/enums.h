/* Implementation of enums. */

struct enumeration;


/* An enumeration can be specified from a static list of entries, or can be
 * built dynamically. */
struct enum_entry { unsigned int value; const char *name; };
struct enum_set { struct enum_entry *enums; size_t count; };



/* Converts converts enumeration number to string or returns NULL if index not a
 * valid enumeration value. */
const char *enum_index_to_name(
    const struct enumeration *enumeration, unsigned int value);

/* Converts string to enumeration index if possible, or returns false. */
bool enum_name_to_index(
    const struct enumeration *enumeration,
    const char *name, unsigned int *value);


/* Outputs list of enumerations to given connection. */
void write_enum_labels(
    const struct enumeration *enumeration, struct connection_result *result);


/* Constructs enumeration from static enum_set.  The enum_set.enums array is not
 * copied and must remain valid. */
const struct enumeration *create_static_enumeration(
    const struct enum_set *enum_set);

/* Constructs dynamic enumeration with the given number of index entries. */
struct enumeration *create_dynamic_enumeration(void);

/* Destroys enumeration created by either of the calls above. */
void destroy_enumeration(const struct enumeration *enumeration);


/* Adds enumeration entry to dynamic enumeration.  Fails with error code if
 * index out of range or if enumeration entry already present. */
error__t add_enumeration(
    struct enumeration *enumeration, const char *name, unsigned int index);

/* Helper for adding parsed indent lines to dynamic enumeration. */
void set_enumeration_parser(
    struct enumeration *enumeration, struct indent_parser *parser);


/* Type methods for enum. */
extern const struct type_methods enum_type_methods;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Helper methods for building types from an unwrapped enumeration. */

/* Formats value according to the given enumeration into string, returning a
 * suitable error on failure. */
error__t format_enumeration(
    const struct enumeration *enumeration,
    unsigned int value, char string[], size_t length);
