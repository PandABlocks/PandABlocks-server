/* Attribute definitions.
 *
 * The definitions of attributes are more exposed than for other entites because
 * their implementation is shared between classes and types. */

struct attr;
struct connection_result;
struct hash_table;


struct attr_methods {
    /* Name of this attribute. */
    const char *name;
    /* Fixed description string for attribute. */
    const char *description;
    /* Set if this attribute contributes to the ATTR change set. */
    bool in_change_set;
    /* Set if this attribute is polled for changes when the attribute change set
     * is requested. If this is set then format() must be implemented.
     * It also enforces in_change_set to be true. */
    bool polled_change_set;

    error__t (*format)(
        void *owner, void *data, unsigned int number,
        char result[], size_t length);

    /* Reads attribute value.  Only need to implement this for multi-line
     * results, otherwise just implement format. */
    error__t (*get_many)(
        void *owner, void *data, unsigned int number,
        struct connection_result *result);

    /* Writes attribute value. */
    error__t (*put)(
        void *owner, void *data, unsigned int number, const char *value);

    /* Returns enumeration assocated with type, if appropriate. */
    const struct enumeration *(*get_enumeration)(void *data);
};

/* Used to represent an array of attributes.  MUST be initialised with
 * DEFINE_ATTRIBUTES macro below. */
struct attr_array {
    struct attr_methods *methods;
};


/* Retrieves current value of attribute:  block<n>.field.attr?  */
error__t attr_get(
    struct attr *attr, unsigned int number,
    struct connection_result *result);

/* Writes value to attribute:  block<n>.field.attr=value  */
error__t attr_put(struct attr *attr, unsigned int number, const char *value);

/* Called to report that the attribute has changed. */
void attr_changed(struct attr *attr, unsigned int number);

/* Retrieves change set for attribute. */
void get_attr_change_set(
    struct attr *attr, uint64_t report_index, bool change_set[]);

/* Name of attribute. */
const char *get_attr_name(const struct attr *attr);

/* Associated enumeration or NULL. */
const struct enumeration *get_attr_enumeration(const struct attr *attr);

/* Description string for attribute. */
const char *get_attr_description(const struct attr *attr);


/* Creates a single attribute with the given owner and data pointers, adds it
 * to the given attr_map, and returns the attribute just created. */
struct attr *add_one_attribute(
    const struct attr_methods *methods,
    void *owner, void *data, unsigned int count,
    struct hash_table *attr_map);

/* Creates a list of attributes and adds them to the given attr_map.  If an
 * attribute with the same name is already present it is silently replaced. */
void add_attributes(
    const struct attr_array array,
    void *owner, void *data, unsigned int count,
    struct hash_table *attr_map);

/* Thus function walks the given map of attributes and deletes all attributes.
 * The map should be deleted after this. */
void delete_attributes(struct hash_table *attr_map);


/* This macro should be used when statically initialising lists of attributes to
 * ensure that the number of attributes is correctly initialised.
 *    Unfortunately the compiler version we're using for this project generates
 * an "initializer element is not constant" error when compiling
 * sizeof(attributes) in gnu99 mode (this seems to be a known bug fixed in later
 * compilers and back ported to RHEL7).  So instead we use a sentinel, setting
 * the .name field to NULL, to identify the end of the list.  This means that
 * the attributes list *must* end with a comma. */
#define DEFINE_ATTRIBUTES(attributes...) \
    { \
        .methods = (struct attr_methods[]) { attributes { .name = NULL, }, }, \
    }
