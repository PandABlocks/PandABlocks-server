/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Simple parsing support. */

/* Returns pointer to first non space character in string. */
const char *skip_whitespace(const char *string);

/* Advances *string past whitespace, fails with error if no whitespace found.
 * Use skip_whitespace() if space was optional. */
error__t parse_whitespace(const char **string);

/* This parses out a sequence of letters and underscores into the result array.
 * The given max_length includes the trailing null character. */
error__t parse_name(const char **string, char result[], int max_length);

/* Tests whether the next character in *string is ch and if so consumes it and
 * returns true, otherwise returns false. */
bool read_char(const char **string, char ch);

/* Expects next character to be ch, fails if not. */
error__t parse_char(const char **string, char ch);

/* Parses an unsigned integer from *string. */
error__t parse_uint(const char **string, unsigned int *result);

/* Parses a double from *string. */
error__t parse_double(const char **string, double *result);

/* Checks for end of input string. */
error__t parse_eos(const char **string);



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Indented file parser. */

struct indent_parser {
    /* This is called at the start of the parse to establish the top level
     * context.  This context is passed to each level 0 line parsed and the
     * end() function. */
    void *(*start)(void);
    /* Parses one line using the given indentation and parse context.  Returns a
     * new context to be used by an lines indented under this line. */
    error__t (*parse_line)(
        unsigned int indent, void *context,
        const char *line, void **indent_context);
    /* This optional function is called after parsing all the indents after each
     * line.  The original indent_context is passed. */
    error__t (*end_parse_line)(unsigned int indent, void *indent_context);
    /* Called at the end of parsing if the rest of parsing was successful.  This
     * function is optional and can be set to NULL if not required. */
    error__t (*end)(void *context);
};


/* Uses intent_parser methods to parse the given file. */
error__t parse_indented_file(
    const char *file_name, unsigned int max_indent,
    const struct indent_parser *parser);
