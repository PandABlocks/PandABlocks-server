/* Interface for entity configuration commands. */

/* Top level implementation of name? and name=value commands. */
extern const struct config_command_set entity_commands;


/* Result of parsing the following syntax:
 *
 *  block [number] "." ( "*" | field [ "." ( "*" | attr ) ]
 *
 * The last non-NULL field determines the parse degree. */
struct entity_context {
    struct block *block;                    // Block database entry
    unsigned int number;                    // Block number, within valid range
    struct field *field;                    // Field database entry
    struct attr *attr;                      // Attribute data, may be absent
};


/* Parser for field or attribute specification.  If star_present is NULL then
 * the * patterns are not accepted.  If number_present is NULL then it is
 * treated as optional, otherwise it's only optional if * is seen or there is
 * only one block. */
error__t parse_block_entity(
    const char **input, struct entity_context *parse,
    bool *number_present, bool *star_present);
