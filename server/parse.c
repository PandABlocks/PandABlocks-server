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
 * only.  Even digits are forbidden at the moment. */
static bool valid_name_char(char ch)
{
    return isascii(ch)  &&  (isalpha(ch)  ||  ch == '_');
}


error__t parse_name(const char **string, char result[], size_t max_length)
{
    size_t ix = 0;
    while (ix < max_length  &&  valid_name_char(**string))
    {
        result[ix] = *(*string)++;
        ix += 1;
    }
    return
        TEST_OK_(ix > 0, "No name found")  ?:
        TEST_OK_(ix < max_length, "Name too long")  ?:
        DO(result[ix] = '\0');
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
        TEST_OK_(errno == 0, "Error converting number");
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

DEFINE_PARSE_NUM(parse_int,    int,          strtol,  10)
DEFINE_PARSE_NUM(parse_uint,   unsigned int, strtoul,  10)
DEFINE_PARSE_NUM(parse_uint64, uint64_t,     strtoull,  10)
DEFINE_PARSE_NUM(parse_double, double,       strtod)


error__t parse_bit(const char **string, bool *result)
{
    return
        TEST_OK_(strchr("01", **string), "Invalid bit value")  ?:
        DO(*result = *(*string)++ == '1');
}


error__t parse_eos(const char **string)
{
    return TEST_OK_(**string == '\0', "Unexpected character after input");
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Indented file parser. */


struct indent_state {
    size_t indent;          // Character up to this indentation

    /* Indentation parser and context for this indentation level. */
    const struct indent_parser *parser;
    void *context;
};


/* Opens a new indentation. */
static error__t open_indent(
    struct indent_state stack[], unsigned int *sp,
    unsigned int max_indent, size_t indent)
{
    return
        TEST_OK_(*sp < max_indent, "Too much indentation")  ?:
        DO(stack[++*sp].indent = indent)  ?:
        TEST_OK_(stack[*sp].parser, "Cannot parse this indentation");
}


/* Closes any existing indentations deeper than the current line. */
static error__t close_indents(
    struct indent_state stack[], unsigned int *sp, size_t indent)
{
    /* Close all indents until we reach an indent less than or equal to the
     * current line ... it had better be equal, otherwise we're trying to start
     * a new indent in an invalid location. */
    while (true)
    {
        struct indent_state *state = &stack[*sp + 1];
        if (state->parser  &&  state->parser->end)
            state->parser->end(state->context);

        if (indent < stack[*sp].indent)
            *sp -= 1;
        else
            break;
    }

    return
        TEST_OK_(indent == stack[*sp].indent, "Invalid indentation on line");
}


/* Processing for a single line: skip comments and blank lines, keep track of
 * indentation of indentation stack, and parse line using the parser. */
static error__t parse_one_line(
    unsigned int max_indent, char *line,
    struct indent_state indent_stack[], unsigned int *sp)
{
    /* Discard any trailing newline character. */
    *strchrnul(line, '\n') = '\0';

    /* Find indent of the current line. */
    const char *parse_line = skip_whitespace(line);
    size_t indent = (size_t) (parse_line - line);

    /* Ignore comments and blank lines. */
    error__t error = ERROR_OK;
    if (*parse_line != '#'  &&  *parse_line != '\0')
    {
        error =
            IF_ELSE(indent > indent_stack[*sp].indent,
                /* New indent, check we can accomodate it and that we have a
                 * parser for this new level. */
                open_indent(indent_stack, sp, max_indent, indent),
            //else
                /* Close any indentations until flush with current line. */
                close_indents(indent_stack, sp, indent));

        if (!error)
        {
            struct indent_state *state = &indent_stack[*sp];
            struct indent_state *next_state = &indent_stack[*sp + 1];
            *next_state = (struct indent_state) { };
            error = state->parser->parse_line(
                *sp, state->context, parse_line,
                &next_state->parser, &next_state->context);
        }
    }
    return error;
}


error__t parse_indented_file(
    const char *file_name, unsigned int max_indent,
    const struct indent_parser *parser, void *context)
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
        /* We need max_indent+2 entries in the stack: parsing with max_indent=0
         * requires one entry for the current parser, and an extra entry for the
         * (unusable) sub-parser. */
        struct indent_state indent_stack[max_indent + 2];
        /* Start with the given parser and context. */
        indent_stack[0] = (struct indent_state) {
            .indent = 0,
            .parser = parser,
            .context = context,
        };
        /* Initially start with an empty parser to close out. */
        indent_stack[1] = (struct indent_state) { };

        while (!error  &&  fgets(line, sizeof(line), file))
        {
            line_no += 1;
            /* Skip whitespace and compute the current indentation. */
            error = parse_one_line(max_indent, line, indent_stack, &sp);

            /* In this case extend the error with the file name and line number
             * for more helpful reporting. */
            if (error)
                error_extend(error,
                    "parsing line %d of \"%s\"", line_no, file_name);
        }
        fclose(file);

        /* The end parse function is optional. */
        if (!error  &&  parser->end)
            parser->end(indent_stack[0].context);
    }
    return error;
}
