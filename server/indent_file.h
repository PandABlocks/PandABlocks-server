/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Indented file parser. */

struct indent_parser {
    /* Context passed through to associated methods. */
    void *context;
    /* Parses one line using the given indentation, parse context, and parser.
     * Must return a new indent_parser and parse context if sub-context lines
     * are to be parsed. */
    error__t (*parse_line)(
        void *context, const char **line, struct indent_parser *parser);
    /* This is called when the indent parser is finished with, and is optional:
     * should be set to NULL if not required. */
    error__t (*end)(void *context);
};


/* Uses intent_parser methods to parse the given file. */
error__t parse_indented_file(
    const char *file_name, const struct indent_parser *parser);