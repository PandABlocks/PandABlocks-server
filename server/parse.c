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


error__t parse_uint_array(
    const char **line, unsigned int array[], size_t length)
{
    error__t error = ERROR_OK;
    for (size_t i = 0; !error  &&  i < length; i ++)
        error =
            IF(i > 0,
                parse_whitespace(line))  ?:
            parse_uint(line, &array[i]);
    return error;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Simple UTF-8 validation. */

enum utf8_decode_status {
    UTF8_DECODE_OK,
    UTF8_DECODE_MALFORMED,
    UTF8_DECODE_NONCANONICAL,
    UTF8_DECODE_SURROGATE,
    UTF8_DECODE_TOO_LARGE,
};


/* A valid UTF-8 sequence representing a single Unicode code point is one of the
 * following:
 *  0xxxxxxx                                7 bit characters
 *  110xxxxx 10xxxxxx                       8 to 11 bit characters
 *  1110xxxx 10xxxxxx 10xxxxxx              12 to 16 bit characters
 *  11110xxx 10xxxxxx 10xxxxxx 10xxxxxx     21 to 17 bit characters
 * To ensure the UTF-8 representation is unique no code point can be represented
 * by a longer sequence of bytes.  Also no code point can be more than 0x10FFFF,
 * and finally the "surrogate" characters 0xD800-DFFF cannot be used.
 *    This function returns either the number of UTF-8 bytes successfully read,
 * or a negative utf8_decode_status value. */
static int check_utf8_char(const uint8_t *utf8)
{
    uint8_t ch = utf8[0];
    if (ch < 0x80)                      // 0xxxxxxx
        return 1;
    else if (ch < 0xC0)                 // 10xxxxxx
        return -UTF8_DECODE_MALFORMED;
    else if (ch < 0xE0)                 // 110xxxxx
    {
        uint8_t ch2 = utf8[1];
        if ((ch2 & 0xC0) != 0x80)
            return -UTF8_DECODE_MALFORMED;
        else if (ch < 0xC2)
            return -UTF8_DECODE_NONCANONICAL;
        else
            return 2;
    }
    else if (ch < 0xF0)                 // 1110xxxx
    {
        uint8_t ch2 = utf8[1];
        if ((ch2 & 0xC0) != 0x80)
            return -UTF8_DECODE_MALFORMED;
        uint8_t ch3 = utf8[2];
        if ((ch3 & 0xC0) != 0x80)
            return -UTF8_DECODE_MALFORMED;
        unsigned int unicode =
            ((ch & 0x0Fu) << 12) | ((ch2 & 0x3Fu) << 6) | (ch3 & 0x3Fu);
        if (unicode < 0x800)
            return -UTF8_DECODE_NONCANONICAL;
        else if ((unicode & 0xF800) == 0xD800)
            return -UTF8_DECODE_SURROGATE;
        else
            return 3;
    }
    else if (ch < 0xF8)                 // 11110xxx
    {
        uint8_t ch2 = utf8[1];
        if ((ch2 & 0xC0) != 0x80)
            return -UTF8_DECODE_MALFORMED;
        uint8_t ch3 = utf8[2];
        if ((ch3 & 0xC0) != 0x80)
            return -UTF8_DECODE_MALFORMED;
        uint8_t ch4 = utf8[3];
        if ((ch4 & 0xC0) != 0x80)
            return -UTF8_DECODE_MALFORMED;
        unsigned int unicode =
            ((ch & 0x07u) << 18) | ((ch2 & 0x3Fu) << 12) |
            ((ch3 & 0x3Fu) << 6) | (ch4 & 0x3Fu);
        if (unicode < 0x1000)
            return -UTF8_DECODE_NONCANONICAL;
        else if (unicode < 0x110000)
            return 4;
        else
            return -UTF8_DECODE_TOO_LARGE;
    }
    else                                // 11111xxx
        return -UTF8_DECODE_MALFORMED;
}

error__t parse_utf8_string(const char **input, const char **result)
{
    static const char *error_strings[] = {
        [UTF8_DECODE_MALFORMED]     = "Malformed UTF-8 encoding",
        [UTF8_DECODE_NONCANONICAL]  = "Non-canonical UTF-8 encoding",
        [UTF8_DECODE_SURROGATE]     = "Unicode surrogate characters disallowed",
        [UTF8_DECODE_TOO_LARGE]     = "UTF-8 value too large for Unicode",
    };

    *result = *input;
    const uint8_t *utf8 = (const uint8_t *) *input;
    while (*utf8 != 0x00)
    {
        if (*utf8 < 0x20)
            return FAIL_("Unexpected control code in string");
        else if (*utf8 < 0x80)
            utf8 += 1;
        else
        {
            int check = check_utf8_char(utf8);
            if (check < 0)
                return FAIL_(error_strings[-check]);
            utf8 += check;
        }
    }
    *input = (const char *) utf8;
    return ERROR_OK;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Indented file parser. */


struct indent_state {
    size_t indent;          // Character up to this indentation

    /* Indentation parser and context for this indentation level. */
    struct indent_parser parser;
};


/* Opens a new indentation. */
static error__t open_indent(
    struct indent_state stack[], unsigned int *sp,
    unsigned int max_indent, size_t indent)
{
    return
        TEST_OK_(*sp < max_indent, "Too much indentation")  ?:
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
    unsigned int max_indent, const char **line,
    struct indent_state indent_stack[], unsigned int *sp)
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
                open_indent(indent_stack, sp, max_indent, indent),
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
    const char *file_name, unsigned int max_indent,
    const struct indent_parser *parser)
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
            error = parse_one_line(max_indent, &parsed_line, indent_stack, &sp);

            /* In this case extend the error with the file name and line number
             * for more helpful reporting. */
            if (error)
                error_extend(error,
                    "parsing line %d of \"%s\"", line_no, file_name);
        }
        fclose(file);

        /* The end parse function is optional. */
        if (!error  &&  parser->end)
            parser->end(indent_stack[0].parser.context);
    }
    return error;
}
