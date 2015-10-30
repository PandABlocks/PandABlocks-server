/* Simple parsing support. */

#include <stdbool.h>
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


error__t parse_name(const char **string, char result[], int max_length)
{
    int ix = 0;
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


error__t parse_char(const char **string, char ch)
{
    return TEST_OK_(read_char(string, ch), "Character '%c' expected", ch);
}


error__t parse_uint(const char **string, unsigned int *result)
{
    errno = 0;
    const char *start = *string;
    char *end;
    *result = (unsigned int) strtoul(start, &end, 10);
    *string = end;
    return
        TEST_OK_(end > start, "Number missing")  ?:
        TEST_OK_IO_(errno == 0, "Error converting number");
}


error__t parse_eos(const char **string)
{
    return TEST_OK_(**string == '\0', "Unexpected character");
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Indented file parser. */


struct indent_state {
    size_t indent;          // Character up to this indentation
    void *context;          // Parse context for this indentation
};


/* Opens a new indentation. */
static error__t open_indent(
    struct indent_state stack[], unsigned int *sp, size_t indent,
    unsigned int max_indent, void *context)
{
    if (*sp < max_indent)
    {
        *sp += 1;
        stack[*sp] = (struct indent_state) {
            .indent = indent, .context = context, };
        return ERROR_OK;
    }
    else
        return FAIL_("Too much indentation");
}


/* Closes any existing indentations deeper than the current line. */
static error__t close_indents(
    const struct indent_parser *parser,
    struct indent_state stack[], unsigned int *sp, size_t indent)
{
    /* Close all indents until we reach an indent less than or equal to the
     * current line ... it had better be equal, otherwise we're trying to start
     * a new indent in an invalid location. */
    error__t error = ERROR_OK;
    while (!error  &&  indent < stack[*sp].indent)
    {
        if (parser->end_parse_line)
            error = parser->end_parse_line(*sp, stack[*sp].context);
        *sp -= 1;
    }
    return
        error  ?:
        TEST_OK_(indent == stack[*sp].indent, "Invalid indentation on line");
}


/* Processing for a single line: skip comments and blank lines, keep track of
 * indentation of indentation stack, and parse line using the parser. */
static error__t parse_one_line(
    unsigned int max_indent, char *line,
    struct indent_state indent_stack[], unsigned int *sp,
    const struct indent_parser *parser, void **new_context)
{
    /* Discard any trailing newline character. */
    *strchrnul(line, '\n') = '\0';

    /* Find indent of the current line. */
    const char *parse_line = skip_whitespace(line);
    size_t indent = (size_t) (parse_line - line);

    /* Ignore comments and blank lines. */
    if (*parse_line != '#'  &&  *parse_line != '\0')
        return
            /* Prepare the appropriate indent level. */
            IF_ELSE(indent > indent_stack[*sp].indent,
                /* New line is more indented, open new indent. */
                open_indent(indent_stack, sp, indent, max_indent, *new_context),
            // else
                /* Close any indentations until flush with current line. */
                close_indents(parser, indent_stack, sp, indent))  ?:

            /* Process the line. */
            parser->parse_line(
                *sp, indent_stack[*sp].context, parse_line, new_context);
    else
        return ERROR_OK;
}


error__t parse_indented_file(
    const char *file_name, unsigned int max_indent,
    const struct indent_parser *parser)
{
    FILE *file;
    error__t error = TEST_NULL_(file = fopen(file_name, "r"),
        "Unable to open file \"%s\"", file_name);
    if (!error)
    {
        char line[MAX_LINE_LENGTH];
        int line_no = 0;

        /* The indentation stack is used to keep track of indents as they're
         * opened and closed. */
        unsigned int sp = 0;
        struct indent_state indent_stack[max_indent + 1];
        indent_stack[0] = (struct indent_state) {
            .indent = 0,
            .context = parser->start ? parser->start() : NULL, };

        /* This context is written each time we parse a line and then used for
         * lines with higher indentation. */
        void *new_context = NULL;

        while (!error  &&  fgets(line, sizeof(line), file))
        {
            line_no += 1;
            /* Skip whitespace and compute the current indentation. */
            error = parse_one_line(
                max_indent, line, indent_stack, &sp, parser, &new_context);
            /* In this case extend the error with the file name and line number
             * for more helpful reporting. */
            if (error)
                error_extend(error,
                    "parsing line %d of \"%s\"", line_no, file_name);
        }
        fclose(file);

        /* The end parse function is optional. */
        if (!error  &&  parser->end)
            error = parser->end(indent_stack[0].context);
    }
    return error;
}
