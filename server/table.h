struct table_subfield;

/* Returns enumeration associated with table subfield. */
const struct enumeration *get_table_subfield_enumeration(
    struct table_subfield *subfield);

/* Returns description for table subfield. */
const char *get_table_subfield_description(struct table_subfield *subfield);

/* Table class api. */
extern const struct class_methods table_class_methods;
