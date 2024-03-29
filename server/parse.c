/* Simple parsing support. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include "error.h"
#include "utf8_check.h"

#include "parse.h"


#define MAX_LINE_LENGTH     256


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Simple parsing support. */


const char *skip_whitespace(const char *string)
{
    while (*string == ' '  ||  *string == '\t')
        string += 1;
    return string;
}


/* Expects whitespace and skips it. */
error__t parse_whitespace(const char **string)
{
    const char *start = *string;
    *string = skip_whitespace(*string);
    return TEST_OK_(*string > start, "Whitespace expected");
}


/* Test for valid character in a name.  We allow ASCII letters and underscores,
 * only. */
static bool valid_name_char(char ch)
{
    return isascii(ch)  &&  (isalpha(ch)  ||  ch == '_');
}

/* Allow numbers as well. */
static bool valid_alphanum_char(char ch)
{
    return isascii(ch)  &&  (isalpha(ch)  ||  ch == '_'  ||  isdigit(ch));
}


static error__t parse_filtered_name(
    const char **string, bool (*filter_char)(char),
    char result[], size_t max_length)
{
    size_t ix = 0;
    while (ix < max_length  &&  filter_char(**string))
    {
        result[ix] = *(*string)++;
        ix += 1;
    }
    return
        TEST_OK_(ix > 0, "No name found")  ?:
        TEST_OK_(ix < max_length, "Name too long")  ?:
        DO(result[ix] = '\0');
}


error__t parse_name(const char **string, char result[], size_t max_length)
{
    return parse_filtered_name(string, valid_name_char, result, max_length);
}


error__t parse_alphanum_name(
    const char **string, char result[], size_t max_length)
{
    return
        TEST_OK_(valid_name_char(**string), "No name found")  ?:
        parse_filtered_name(string, valid_alphanum_char, result, max_length);
}


error__t parse_block_name(
    const char **string, char result[], size_t max_length)
{
    const char *start = *string;
    return
        parse_alphanum_name(string, result, max_length)  ?:
        DO(
            /* Finally remove any trailing digits from the parse. */
            while (isdigit((*string)[-1]))
                *string -= 1;
            result[*string - start] = '\0';
        );
}


bool read_char(const char **string, char ch)
{
    if (**string == ch)
    {
        *string += 1;
        return true;
    }
    else
        return false;
}


bool read_string(const char **string, const char *expected)
{
    size_t length = strlen(expected);
    if (strncmp(*string, expected, length) == 0)
    {
        *string += length;
        return true;
    }
    else
        return false;
}


error__t parse_char(const char **string, char ch)
{
    return TEST_OK_(read_char(string, ch), "Character '%c' expected", ch);
}


/* Called after a C library conversion function checks that anything was
 * converted and that the conversion was successful.  Relies on errno being zero
 * before conversion started. */
static error__t check_number(const char *start, const char *end)
{
    return
        TEST_OK_(end > start, "Number missing")  ?:
        TEST_OK_IO_(errno == 0, "Error converting number");
}


/* Parsing numbers is rather boilerplate.  This macro encapsulates everything in
 * one common form. */
#define DEFINE_PARSE_NUM(name, type, convert, extra...) \
    error__t name(const char **string, type *result) \
    { \
        errno = 0; \
        const char *start = *string; \
        char *end; \
        *result = (type) convert(start, &end, ##extra); \
        *string = end; \
        return check_number(start, *string); \
    }

DEFINE_PARSE_NUM(parse_int,    int,          strtol, 0)
DEFINE_PARSE_NUM(parse_uint,   unsigned int, strtoul, 0)
DEFINE_PARSE_NUM(parse_uint32, uint32_t,     strtoul, 0)
DEFINE_PARSE_NUM(parse_uint64, uint64_t,     strtoull, 0)
DEFINE_PARSE_NUM(parse_double, double,       strtod)


error__t parse_bit(const char **string, bool *result)
{
    return
        TEST_OK_(**string == '0'  ||  **string == '1', "Invalid bit value")  ?:
        DO(*result = *(*string)++ == '1');
}


error__t parse_eos(const char **string)
{
    return TEST_OK_(**string == '\0', "Unexpected character after input");
}


error__t parse_uint_array(
    const char **line, unsigned int array[], size_t length)
{
    error__t error = ERROR_OK;
    for (size_t i = 0; !error  &&  i < length; i ++)
        error =
            IF(i > 0, parse_whitespace(line))  ?:
            parse_uint(line, &array[i]);
    return error;
}


error__t parse_utf8_string(const char **input, const char **result)
{
    *result = *input;
    *input = utf8_check(*input);
    return TEST_OK_(**input == '\0', "Malformed UTF-8 encoding");
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Indented file parser. */


/* Somewhat arbitrary limit on maximum valid indentation, but if you indent more
 * than this then you're doing it wrong!  (Or just increase this number...) */
#define MAX_INDENT      10


struct indent_state {
    size_t indent;          // Character up to this indentation

    /* Indentation parser and context for this indentation level. */
    struct indent_parser parser;
};


/* Opens a new indentation. */
static error__t open_indent(
    struct indent_state stack[], unsigned int *sp, size_t indent)
{
    return
        TEST_OK_(*sp < MAX_INDENT, "Too much indentation")  ?:
        DO(stack[++*sp].indent = indent);
}


/* Closes any existing indentations deeper than the current line. */
static error__t close_indents(
    struct indent_state stack[], unsigned int *sp, size_t indent)
{
    /* Close all indents until we reach an indent less than or equal to the
     * current line ... it had better be equal, otherwise we're trying to start
     * a new indent in an invalid location. */
    error__t error = ERROR_OK;
    while (!error)
    {
        struct indent_state *state = &stack[*sp + 1];
        if (state->parser.end)
            error = state->parser.end(state->parser.context);

        if (indent < stack[*sp].indent)
            *sp -= 1;
        else
            break;
    }

    return
        error  ?:
        TEST_OK_(indent == stack[*sp].indent, "Invalid indentation on line");
}


/* Processing for a single line: skip comments and blank lines, keep track of
 * indentation of indentation stack, and parse line using the parser. */
static error__t parse_one_line(
    const char **line, struct indent_state indent_stack[], unsigned int *sp)
{
    /* Find indent of the current line. */
    const char *line_in = *line;
    *line = skip_whitespace(*line);
    size_t indent = (size_t) (*line - line_in);

    /* Ignore comments and blank lines. */
    error__t error = ERROR_OK;
    if (**line != '#'  &&  **line != '\0')
    {
        error =
            IF_ELSE(indent > indent_stack[*sp].indent,
                /* New indent, check we can accomodate it and that we have a
                 * parser for this new level. */
                open_indent(indent_stack, sp, indent),
            //else
                /* Close any indentations until flush with current line. */
                close_indents(indent_stack, sp, indent));

        if (!error)
        {
            struct indent_state *state = &indent_stack[*sp];
            struct indent_parser *next_parser = &indent_stack[*sp+1].parser;
            *next_parser = (struct indent_parser) { };
            error =
                TEST_OK_(state->parser.parse_line,
                    "Cannot parse this indentation")  ?:
                state->parser.parse_line(
                    state->parser.context, line, next_parser)  ?:
                parse_eos(line);
        }
    }
    return error;
}


error__t parse_indented_file(
    const char *file_name, const struct indent_parser *parser)
{
    FILE *file;
    error__t error = TEST_OK_IO_(file = fopen(file_name, "r"),
        "Unable to open file \"%s\"", file_name);
    if (!error)
    {
        char line[MAX_LINE_LENGTH];
        int line_no = 0;

        /* The indentation stack is used to keep track of indents as they're
         * opened and closed. */
        unsigned int sp = 0;
        /* We need MAX_INDENT+2 entries in the stack: parsing with MAX_INDENT=0
         * requires one entry for the current parser, and an extra entry for the
         * (unusable) sub-parser. */
        struct indent_state indent_stack[MAX_INDENT + 2];
        /* Start with the given parser and context. */
        indent_stack[0] = (struct indent_state) {
            .indent = 0,
            .parser = *parser,
        };
        /* Initially start with an empty parser to close out. */
        indent_stack[1] = (struct indent_state) { };

        while (!error  &&  fgets(line, sizeof(line), file))
        {
            /* Discard any trailing newline character. */
            *strchrnul(line, '\n') = '\0';

            line_no += 1;
            /* Skip whitespace and compute the current indentation. */
            const char *parsed_line = line;
            error = ERROR_EXTEND(
                parse_one_line(&parsed_line, indent_stack, &sp),
                /* Extend the error with file name, line number, and column
                 * where the parse error occurred. */
                "Parsing line %d of \"%s\" at column %zd",
                line_no, file_name, parsed_line - line + 1);
        }
        fclose(file);

        /* The end parse function is optional. */
        if (!error  &&  parser->end)
            parser->end(indent_stack[0].parser.context);
    }
    return error;
}
