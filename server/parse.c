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


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Simple parsing support. */


const char *skip_whitespace(const char *string)
{
    while (*string == ' '  ||  *string == '\t')
        string += 1;
    return string;
}


/* Expects whitespace and skips it. */
bool parse_whitespace(const char **string)
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


bool parse_name(const char **string, char result[], int max_length)
{
    int ix = 0;
    while (ix < max_length  &&  valid_name_char(**string))
    {
        result[ix] = *(*string)++;
        ix += 1;
    }
    return
        TEST_OK_(ix > 0, "No name found")  &&
        TEST_OK_(ix < max_length, "Name too long")  &&
        DO(result[ix] = '\0');
}


bool read_char(const char **string, char ch)
{
    return **string == ch  &&  DO(*string += 1);
}


bool parse_char(const char **string, char ch)
{
    return TEST_OK_(read_char(string, ch), "Character '%c' expected", ch);
}


bool parse_uint(const char **string, unsigned int *result)
{
    errno = 0;
    const char *start = *string;
    char *end;
    *result = (unsigned int) strtoul(start, &end, 10);
    *string = end;
    return
        TEST_OK_(end > start, "Number missing")  &&
        TEST_OK_(errno == 0, "Error converting number");
}


bool parse_eos(const char **string)
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
static bool open_indent(
    unsigned int *sp, struct indent_state stack[], size_t indent,
    unsigned int max_indent, void *context)
{
    if (*sp < max_indent)
    {
        *sp += 1;
        stack[*sp] = (struct indent_state) {
            .indent = indent, .context = context };
        return true;
    }
    else
        return FAIL_("Too much indentation");
}


/* Closes any existing indentations deeper than the current line. */
static bool close_indents(
    unsigned int *sp, struct indent_state stack[], size_t indent)
{
    /* Close all indents until we reach an indent less than or equal to the
     * current line ... it had better be equal, otherwise we're trying to start
     * a new indent in an invalid location. */
    while (indent < stack[*sp].indent)
        *sp -= 1;
    return TEST_OK_(indent == stack[*sp].indent, "Invalid indentation on line");
}


bool parse_indented_file(
    const char *file_name, unsigned int max_indent,
    const struct indent_parser *parser)
{
    FILE *file;
    bool ok = TEST_NULL_(file = fopen(file_name, "r"),
        "Unable to open file \"%s\"", file_name);
    if (ok)
    {
        char line[256];
        int line_no = 0;

        struct indent_state indent_stack[max_indent + 1];
        unsigned int sp = 0;
        indent_stack[sp] = (struct indent_state) {
            .indent = 0,
            .context = parser->start() };
        void *new_context = NULL;

        while (ok  &&  fgets(line, sizeof(line), file))
        {
            line_no += 1;

            /* Discard any trailing newline character. */
            *strchrnul(line, '\n') = '\0';

            /* Skip whitespace and compute the current indentation. */
            const char *parse_line = skip_whitespace(line);
            size_t indent = (size_t) (parse_line - line);

            /* Ignore comments and blank lines. */
            if (*parse_line != '#'  &&  *parse_line != '\0')
                ok =
                    IF_ELSE(indent > indent_stack[sp].indent,
                        /* Open new ident if appropriate. */
                        open_indent(
                            &sp, indent_stack, indent, max_indent, new_context),
                    // else
                        /* Close any indentations. */
                        close_indents(&sp, indent_stack, indent))  &&
                    parser->parse_line(
                        sp, indent_stack[sp].context, parse_line, &new_context);
            if (!ok)
                log_message(
                    "Error parsing line %d of \"%s\"", line_no, file_name);
        }
        fclose(file);

        /* The end parse function is optional. */
        if (ok  &&  parser->end)
            ok = parser->end(indent_stack[0].context);
    }
    return ok;
}
