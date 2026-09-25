#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "error.h"
#include "parse.h"

#include "indent_file.h"

#define MAX_LINE_LENGTH 256


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
    skip_whitespace(line);
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
            error = error_extend(
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
