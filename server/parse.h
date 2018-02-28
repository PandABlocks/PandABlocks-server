/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Simple parsing support. */

/* Returns pointer to first non space character in string. */
const char *skip_whitespace(const char *string);

/* Advances *string past whitespace, fails with error if no whitespace found.
 * Use skip_whitespace() if space was optional. */
error__t parse_whitespace(const char **string);

/* This parses out a sequence of letters and underscores into the result array.
 * The given max_length includes the trailing null character. */
error__t parse_name(const char **string, char result[], size_t max_length);

/* As for parse_name, but also accepts numbers after the leading character. */
error__t parse_alphanum_name(
    const char **string, char result[], size_t max_length);

/* Tests whether the next character in *string is ch and if so consumes it and
 * returns true, otherwise returns false. */
bool read_char(const char **string, char ch);

/* Tests whether input string matches given comparison string, if so consumes it
 * and returns true, otherwise returns false. */
bool read_string(const char **string, const char *expected);

/* Expects next character to be ch, fails if not. */
error__t parse_char(const char **string, char ch);

/* Parses an unsigned integer from *string. */
error__t parse_uint(const char **string, unsigned int *result);

/* Parses a 32-bit unsigned integer from *string. */
error__t parse_uint32(const char **string, uint32_t *result);

/* Parses a 64-bit unsigned integer from *string. */
error__t parse_uint64(const char **string, uint64_t *result);

/* Parses a signed integer from *string. */
error__t parse_int(const char **string, int *result);

/* Parses a double from *string. */
error__t parse_double(const char **string, double *result);

/* Parses bit from *string. */
error__t parse_bit(const char **string, bool *result);

/* Checks for end of input string. */
error__t parse_eos(const char **string);

/* Assigns *string to *result and skips *string to end after checking the rest
 * of the string to ensure it only contains valid UTF-8 characters. */
error__t parse_utf8_string(const char **input, const char **result);


/* Parses an array of uints. */
error__t parse_uint_array(
    const char **line, unsigned int array[], size_t length);


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
