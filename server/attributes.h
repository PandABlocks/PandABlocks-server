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
        void *owner, void *data, unsigned int number,
        const char *value);

    /* Returns enumeration assocated with type, if appropriate. */
    const struct enumeration *(*get_enumeration)(void *data);
};


/* Retrieves current value of attribute:  block<n>.field.attr?  */
error__t attr_get(
    struct attr *attr, unsigned int number,
    struct connection_result *result);

/* Writes value to attribute:  block<n>.field.attr=value  */
error__t attr_put(struct attr *attr, unsigned int number, const char *value);

/* Retrieves change set for attribute. */
void get_attr_change_set(
    struct attr *attr, uint64_t report_index, bool change_set[]);

/* Name of attribute. */
const char *get_attr_name(const struct attr *attr);

/* Associated enumeration or NULL. */
const struct enumeration *get_attr_enumeration(const struct attr *attr);

/* Description string for attribute. */
const char *get_attr_description(const struct attr *attr);


/* This function creates an attribute with the given ownder and data pointers
 * and inserts it into the given attr_map.  If an attribute with the same name
 * is already present it is silently replaced. */
void create_attributes(
    const struct attr_methods methods[], unsigned int attr_count,
    void *owner, void *data, unsigned int count,
    struct hash_table *attr_map);

/* Thus function walks the given map of attributes and deletes all attributes.
 * The map should be deleted after this. */
void delete_attributes(struct hash_table *attr_map);
