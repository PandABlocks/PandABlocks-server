/* Entity definitions. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include "error.h"
#include "hashtable.h"

#include "database.h"


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Simple parsing support. */


/* Returns pointer to first non space character in string. */
static const char *skip_whitespace(const char *string)
{
    while (*string == ' '  ||  *string == '\t')
        string += 1;
    return string;
}


/* Expects whitespace and skips it. */
static bool parse_whitespace(const char **string)
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


/* This parses out a sequence of letters and underscores into the result array.
 * The given max_length includes the trailing null character. */
static bool parse_name(const char **input, char result[], int max_length)
{
    int ix = 0;
    while (ix < max_length  &&  valid_name_char(**input))
    {
        result[ix] = *(*input)++;
        ix += 1;
    }
    return
        TEST_OK_(ix > 0, "No name found")  &&
        TEST_OK_(ix < max_length, "Name too long")  &&
        DO(result[ix] = '\0');
}


static bool read_char(const char **string, char ch)
{
    return **string == ch  &&  DO(*string += 1);
}


/* Consumes given character. */
static bool parse_char(const char **string, char ch)
{
    return TEST_OK_(read_char(string, ch), "Character '%c' expected", ch);
}


/* Parses an unsigned integer. */
static bool parse_uint(const char **input, unsigned int *result)
{
    errno = 0;
    const char *start = *input;
    char *end;
    *result = (unsigned int) strtoul(start, &end, 10);
    *input = end;
    return
        TEST_OK_(end > start, "Number missing")  &&
        TEST_OK_(errno == 0, "Error converting number");
}


/* Checks whether a string has been fully parsed. */
static bool parse_eos(const char **string)
{
    return TEST_OK_(**string == '\0', "Unexpected character");
}



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Indented file parser. */


struct indent_parser {
    /* This is called at the start of the parse to establish the top level
     * context.  This context is passed to each level 0 line parsed. */
    void *(*start)(void);
    /* Parses one line using the given indentation and parse context.  Returns a
     * new context to be used by an lines indented under this line. */
    bool (*parse_line)(
        int indent, void *context, const char *line, void **indent_context);
    /* Called at the end of parsing if the rest of parsing was successful. */
    bool (*end)(void *context);
};


#define MAX_INDENT  3       // Support up to three levels of indentation

struct indent_state {
    size_t indent;          // Character up to this indentation
    void *context;          // Parse context for this indentation
};


/* Opens a new indentation. */
static bool open_indent(
    int *sp, struct indent_state stack[], size_t indent, void *context)
{
    if (*sp < MAX_INDENT)
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
    int *sp, struct indent_state stack[], size_t indent)
{
    /* Close all indents until we reach an indent less than or equal to the
     * current line ... it had better be equal, otherwise we're trying to start
     * a new indent in an invalid location. */
    while (indent < stack[*sp].indent)
        *sp -= 1;
    return TEST_OK_(indent == stack[*sp].indent, "Invalid indentation on line");
}



static bool parse_indented_file(
    const char *file_name, const struct indent_parser *parser)
{
    FILE *file;
    bool ok = TEST_NULL_(file = fopen(file_name, "r"),
        "Unable to open file \"%s\"", file_name);
    if (ok)
    {
        char line[256];
        int line_no = 0;

        struct indent_state indent_stack[MAX_INDENT + 1];
        int sp = 0;
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
                        open_indent(&sp, indent_stack, indent, new_context),
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

        ok = ok &&
            parser->end(indent_stack[0].context);
    }
    return ok;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Config database parsing. */

/* Each field in an entity is of one of the following classes. */
enum field_class {
    FIELD_PARAM,            // Settable parameter
    FIELD_WRITE,            // Writeable action field
    FIELD_READ,             // Read only field
    FIELD_BIT_OUT,          // Bit on internal bit bus
    FIELD_BIT_IN,           // Multiplexer bit selection
    FIELD_POSITION_OUT,     // Position on internal position bus
    FIELD_POSITION_IN,      // Multiplexer position selection
    FIELD_TABLE,            // Table of values
};

/* The top level configuration database is a map from entity names to entities.
 * At present that's all we have. */
struct config_database {
    struct hash_table *map;             // Map from entity name to config block
};

/* Each entry in the entity map documents the entity. */
struct config_block {
    unsigned int count;                 // Number of entities of this kind
    struct hash_table *map;             // Map from field name to field
};

/* Each field has a detailed description. */
struct field_entry {
    enum field_class class;
    struct field_type *type;
};


/* We'll reject identifiers longer than this. */
#define MAX_NAME_LENGTH     20


static struct config_database config_database;


/* This array of class names is used to identify the appropriate field class
 * from an input string.  Note that the sequence of these names must match the
 * field_class definition. */
static const char *class_names[] = {
    "param",
    "write",
    "read",
    "bit_out",
    "bit_in",
    "pos_out",
    "pos_in",
    "table",
};

/* Converts class name to enum. */
static bool lookup_class_name(const char *name, enum field_class *class)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(class_names); i ++)
        if (strcmp(name, class_names[i]) == 0)
        {
            *class = (enum field_class) i;
            return true;
        }
    /* Not present, fail. */
    return FAIL_("Field class %s not recognised", name);
}



static void *config_start(void)
{
    printf("config_start\n");
    config_database.map = hash_table_create(true);
    return &config_database;
}



/* Parses a block definition header.  This is simply a name, optionally followed
 * by a number in square brackets, and should be followed by a number of field
 * definitions. */
static bool config_parse_header_line(
    void *context, const char *line, void **indent_context)
{
    struct config_database *database = context;

    /* Parse input of form <name> [ "[" <count> "]" ]. */
    char name[MAX_NAME_LENGTH];
    unsigned int count = 1;
    bool ok =
        parse_name(&line, name, sizeof(name))  &&
        IF(*line == '[',
            parse_char(&line, '[')  &&
            parse_uint(&line, &count)  &&
            parse_char(&line, ']'))  &&
        parse_eos(&line);

    if (ok)
    {
        /* Create a new configuration block with the computed name and count. */
        struct config_block *block = malloc(sizeof(struct config_block));
        block->count = count;
        block->map = hash_table_create(true);
        *indent_context = block;

        printf("Inserting %s[%d]\n", name, count);
        ok = TEST_OK_(!hash_table_insert(database->map, name, block),
            "Entity name %s repeated", name);
    }
    return ok;
}


static bool config_parse_field_line(void *context, const char *line)
{
    struct config_block *block = context;

    char class_name[MAX_NAME_LENGTH];
    enum field_class class;
    char field_name[MAX_NAME_LENGTH];
    bool ok =
        parse_name(&line, class_name, sizeof(class_name))  &&
        lookup_class_name(class_name, &class)  &&
        parse_whitespace(&line)  &&
        parse_name(&line, field_name, sizeof(field_name))  &&
        DO(line = skip_whitespace(line));

    if (ok)
    {
        struct field_entry *field = malloc(sizeof(struct field_entry));
        *field = (struct field_entry) {
            .class = class,
            .type = NULL,
        };
        ok = TEST_OK_(!hash_table_insert(block->map, field_name, field),
            "Field name %s repeated", field_name);
    }
    return ok;
}


static bool config_parse_line(
    int indent, void *context, const char *line, void **indent_context)
{
    printf("config_parse_line %d %p \"%s\"\n",
        indent, context, line);

    switch (indent)
    {
        case 0:
            return config_parse_header_line(
                context, line, indent_context);
        case 1:
            *indent_context = NULL;
            return config_parse_field_line(context, line);
        default:
            return FAIL_("Too much indentation");
    }
}


static bool config_end(void *context)
{
    printf("config_end\n");
    return true;
}

static const struct indent_parser config_indent_parser = {
    .start = config_start,
    .parse_line = config_parse_line,
    .end = config_end,
};


static bool load_config_database(const char *db_name)
{
    log_message("Loading configuration database from \"%s\"", db_name);

    return parse_indented_file(db_name, &config_indent_parser);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static bool load_types_database(const char *db)
{
    log_message("Loading types database from \"%s\"", db);
    return true;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static bool load_register_database(const char *db)
{
    log_message("Loading register database from \"%s\"", db);
    return true;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

bool load_config_databases(
    const char *config_db, const char *types_db, const char *register_db)
{
    return
        load_types_database(types_db)  &&
        load_config_database(config_db)  &&
        load_register_database(register_db)  &&
        FAIL_("ok");
}
