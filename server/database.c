/* Entity definitions. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "error.h"
#include "hashtable.h"

#include "database.h"


/* Each field in an entity is of one of the following classes. */
enum field_class {
    FIELD_PARAM,            // Settable parameter
    FIELD_WRITE,            // Writeable action field
    FIELD_READ,             // Read only field
    FIELD_BIT_OUT,          // Bit on internal bit bus
    FIELD_BIT_IN,           // Multiplexer bit selection
    FIELD_POSITION_OUT,     // Position on internal position bus
    FIELD_POSITION_IN,      // Multiplexer position selection
};

/* The top level configuration database is a map from entity names to entities.
 * At present that's all we have. */
struct config_database {
    struct hash_table *map;             // Map from entity name to config block
};

/* Each entry in the entity map documents the entity. */
struct config_block {
    int count;                          // Number of entities of this kind
    struct hash_table *field_map;       // Map from field name to field
};

/* Each field has a detailed description. */
struct field_entry {
    enum field_class class;
    struct field_type *type;
};



static struct config_database config_database;


#if 0
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
};

/* Converts class name to enum. */
static bool lookup_class_name(const char *name, enum field_class *class)
{
    for (int i = 0; i < ARRAY_SIZE(class_names); i ++)
        if (strcmp(name, class_names[i]) == 0)
        {
            *class = (enum field_class) i;
            return true;
        }
    /* Not present, fail. */
    return FAIL_("Field class %s not recognised", name);
}


static bool create_config_block(
    const char *name, int count, struct config_block *block)
{
    *block = malloc(sizeof(struct config_block));
    (*block)->count = count;
    (*block)->field_map = hash_table_create(true);
    return TEST_OK(!hash_table_insert(config_database.map, name, block),
        "Entity name %s repeated", name);
}


static bool process_line(char *line, int line_no, int *indent)
{
    /* Tokenise line by whitespace.  We use the first token to classify the
     * string altogether. */
    char *token_context;
    char *token = strtok_r(line, " \t\n", &token_context);

    if (token == NULL  ||  token[0] = '#')
        /* Just discard blank lines and comment lines. */
        return true;
    else
    {
        int new_indent = token - line;

        if (new_indent)
    }
}
#endif


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Indented file parser. */


struct indent_parser {
    /* This is called at the start of the parse to establish the top level
     * context.  This context is passed to each level 0 line parsed. */
    void *(*start)(void);
    /* Called at the end of parsing if the rest of parsing was successful. */
    bool (*end)(void *context);
    /* This is called at the start of an indentation block.  The current
     * indentation level and context are passed together with the current line
     * and line number, and a new indentation context is returned. */
    bool (*start_indent)(
        int indent, void *context, int line_no, char *line, void **new_context);
    /* Called at the end of an indentation block.  The indent level being
     * closed, the context created at the start of this block, and the current
     * line number are passed. */
    bool (*end_indent)(int indent, void *context, int line_no);
    /* Parses one line using the given indentation and parse context. */
    bool (*parse_line)(int indent, void *context, int line_no, char *line);
};


#define MAX_INDENT  3       // Support up to three levels of indentation

struct indent_state {
    int indent;             // Character up to this indentation
    void *context;          // Parse context for this indentation
};


/* Opens a new indentation. */
static bool open_indent(
    const struct indent_parser *parser,
    int indent, int *sp, struct indent_state stack[],
    int line_no, char *line)
{
    void *context;
    bool ok =
        TEST_OK_(*sp < MAX_INDENT,
            "No more indentation allowed on line %d", line_no)  &&
        parser->start_indent(
            *sp, stack[*sp].context, line_no, line, &context);
        DO(
            *sp += 1;
            stack[*sp] = (struct indent_state) BRACES(
                .indent = indent, .context = context));
#if 0
    if (ok)
    {
        *sp += 1;
        stack[*sp] = (struct indent_state) {
            .indent = indent, .context = context, };
    }
#endif
    return ok;
}


/* Closes any existing indentations deeper than the current line. */
static bool close_indents(
    const struct indent_parser *parser,
    int indent, int *sp, struct indent_state stack[], int line_no)
{
    bool ok = true;
    /* Close all indents until we reach an indent less than or equal to the
     * current line ... it had better be equal, otherwise we're trying to start
     * a new indent in an invalid location. */
    while (ok  &&  indent < stack[*sp].indent)
        ok = parser->end_indent(*sp, stack[*sp].context, line_no)  &&
        DO(*sp -= 1);
    return ok  &&
        TEST_OK_(indent == stack[*sp].indent,
            "Invalid indentation on line %d", line_no);
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

        while (ok  &&  fgets(line, sizeof(line), file))
        {
            line_no += 1;

            /* Discard any trailing newline character. */
            *strchrnul(line, '\n') = '\0';

            /* Skip whitespace and compute the current indentation. */
            int indent = 0;
            while (line[indent] == ' ')
                indent += 1;
            char *parse_line = &line[indent];

            /* Ignore comments and blank lines. */
            if (*parse_line != '#'  &&  *parse_line != '\0')
                ok =
                    IF_ELSE(indent > indent_stack[sp].indent,
                        open_indent(
                            parser, indent, &sp, indent_stack,
                            line_no, parse_line),
                        close_indents(
                            parser, indent, &sp, indent_stack, line_no))  &&
                    parser->parse_line(
                        sp, indent_stack[sp].context, line_no, parse_line);
        }
        fclose(file);

        ok = ok &&
            close_indents(parser, 0, &sp, indent_stack, line_no)  &&
            parser->end(indent_stack[0].context);
    }
    return ok;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Config database parsing. */


static void *config_start(void)
{
    printf("config_start\n");
    return (void *) 1;
}

static bool config_end(void *context)
{
    printf("config_end\n");
    return true;
}

static bool config_start_indent(
    int indent, void *context, int line_no, char *line, void **new_context)
{
    printf("config_start_indent %d %p %d \"%s\"\n",
        indent, context, line_no, line);
    *new_context = context + 1;
    return true;
}

static bool config_end_indent(int indent, void *context, int line_no)
{
    printf("config_end_indent %d %p %d\n", indent, context, line_no);
    return true;
}

static bool config_parse_line(
    int indent, void *context, int line_no, char *line)
{
    printf("config_parse_line %d %p %d \"%s\"\n",
        indent, context, line_no, line);
    return true;
}

static const struct indent_parser config_indent_parser = {
    .start = config_start,
    .end = config_end,
    .start_indent = config_start_indent,
    .end_indent = config_end_indent,
    .parse_line = config_parse_line,
};


static bool load_config_database(const char *db_name)
{
    log_message("Loading configuration database from \"%s\"", db_name);
    config_database.map = hash_table_create(true);

    return parse_indented_file(db_name, &config_indent_parser);
}


static bool load_types_database(const char *db)
{
    log_message("Loading types database from \"%s\"", db);
    return true;
}


static bool load_register_database(const char *db)
{
    log_message("Loading register database from \"%s\"", db);
    return true;
}


bool load_config_databases(
    const char *config_db, const char *types_db, const char *register_db)
{
    return
        load_config_database(config_db)  &&
        load_types_database(types_db)  &&
        load_register_database(register_db)  &&
        FAIL_("ok");
}
