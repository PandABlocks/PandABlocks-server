#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "hashtable.h"
#include "parse.h"
#include "mux_lookup.h"
#include "fields.h"
#include "config_server.h"
#include "types.h"

#include "classes.h"


#define UNASSIGNED_REGISTER ((unsigned int) -1)


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/* Abstract interface to class. */
struct class_methods {
    const char *name;

    /* Type information. */
    const char *default_type;   // Default type.  If NULL no type is created
    bool force_type;            // If set default_type cannot be modified

    /* Called to parse the class definition line for a field.  The corresponding
     * class has already been identified. */
    error__t (*init)(
        const struct class_methods *methods, unsigned int count,
        void **class_data);

    /* Parses the register definition line for this field. */
    error__t (*parse_register)(
        struct class *class, const char *block_name, const char *field_name,
        const char **line);
    /* Called after startup to validate setup. */
    error__t (*validate)(struct class *class);
    /* Called during shutdown to release all class resources. */
    void (*destroy)(struct class *class);

    /* Register read/write methods. */
    uint32_t (*read)(struct class *class);
    void (*write)(struct class *class, uint32_t value);
    void (*refresh)(struct class *class);

    /* Access to table data. */
    error__t (*get_many)(
        struct class *class, unsigned int ix,
        const struct connection_result *result);
    error__t (*put_table)(
        struct class *class, unsigned int ix,
        bool append, struct put_table_writer *writer);

    /* Defines which change set, if any, this class contributes to. */
    int change_set_ix;      // -1 for no change set

    /* Field attributes. */
    const struct attr *attrs;
    unsigned int attr_count;
};


struct class {
    const struct class_methods *methods;
    unsigned int count;
    unsigned int reg;
    void *class_data;
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Class field access. */

error__t class_read(struct class *class, uint32_t *value, bool refresh)
{
    return
        TEST_OK_(class->methods->read, "Field not readable")  ?:
        IF(refresh  &&  class->methods->refresh,
            DO(class->methods->refresh(class)))  ?:
        DO(*value = class->methods->read(class));
}


error__t class_write(struct class *class, uint32_t value)
{
    return
        TEST_OK_(class->methods->write, "Field not writeable")  ?:
        DO(class->methods->write(class, value));
}


error__t class_get(
    struct class *class, unsigned int number,
    const struct connection_result *result)
{
    return FAIL_("Not implemented");
}


error__t class_put_table(
    struct class *class, unsigned int number,
    bool append, struct put_table_writer *writer)
{
    return FAIL_("Not implemented");
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Change support. */

/* This number is used to work out which fields have changed since we last
 * looked.  This is incremented on every update. */
static uint64_t global_change_index = 0;


/* Allocates and returns a fresh change index. */
uint64_t get_change_index(void)
{
    return __sync_add_and_fetch(&global_change_index, 1);
}


void get_class_change_set(
    struct class *class, uint64_t report_index[], bool changes[])
{
    printf("Not implemented\n");
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Attribute access. */

void class_attr_list_get(
    const struct class *class,
    const struct connection_result *result)
{
//     if (class->methods->attrs)
//         for (unsigned int i = 0; i < class->methods->attr_count; i ++)
//             result->write_many(connection, class->methods->attrs[i].name);
}


const struct attr *class_lookup_attr(struct class *class, const char *name)
{
//     const struct attr *attrs = class->methods->attrs;
//     if (attrs)
//         for (unsigned int i = 0; i < class->methods->attr_count; i ++)
//             if (strcmp(name, attrs[i].name) == 0)
//                 return &attrs[i];
    return NULL;
}


error__t class_attr_get(
    const struct class_attr_context *context,
    const struct connection_result *result)
{
    return FAIL_("Not implemented");
}


error__t class_attr_put(
    const struct class_attr_context *context, const char *value)
{
    return FAIL_("Not implemented");
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static error__t default_init(
    const struct class_methods *methods, unsigned int count, void **class_data)
{
    return ERROR_OK;
}

static error__t default_validate(struct class *class)
{
    return ERROR_OK;
    return
        TEST_OK_(class->reg != UNASSIGNED_REGISTER,
            "No register assigned to field");
}

static error__t default_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    return
        TEST_OK_(class->reg == UNASSIGNED_REGISTER,
            "Register already assigned")  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &class->reg);
}


static error__t mux_parse_register(
    struct mux_lookup *lookup,
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    unsigned int indices[class->count];
    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < class->count; i ++)
        error =
            parse_whitespace(line)  ?:
            parse_uint(line, &indices[i]);
    return
        error ?:
        add_mux_indices(lookup, block_name, field_name, class->count, indices);
}

static error__t bit_out_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    return mux_parse_register(
        bit_mux_lookup, class, block_name, field_name, line);
}

static error__t pos_out_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    return mux_parse_register(
        pos_mux_lookup, class, block_name, field_name, line);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Top level list of classes. */

#define CLASS_DEFAULTS \
    .init = default_init, \
    .validate = default_validate, \
    .parse_register = default_parse_register, \
    .change_set_ix = -1

static const struct class_methods classes_table[] = {
    { "bit_in", "bit_mux", true, CLASS_DEFAULTS,
    },
    { "pos_in", "pos_mux", true, CLASS_DEFAULTS, },
    { "bit_out", "bit", CLASS_DEFAULTS,
        .parse_register = bit_out_parse_register, },
    { "pos_out", "position", CLASS_DEFAULTS,
        .parse_register = pos_out_parse_register, },
    { "param", "uint", CLASS_DEFAULTS, },
    { "read", "uint", CLASS_DEFAULTS, },
    { "write", "uint", CLASS_DEFAULTS, },
    { "table", CLASS_DEFAULTS, .parse_register = NULL, },
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Class inititialisation. */


static error__t lookup_class(
    const char *name, const struct class_methods **result)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(classes_table); i ++)
    {
        const struct class_methods *methods = &classes_table[i];
        if (strcmp(name, methods->name) == 0)
        {
            *result = methods;
            return ERROR_OK;
        }
    }
    return FAIL_("Class %s not found", name);
}

static struct class *create_class_block(
    const struct class_methods *methods,
    unsigned int count, void *class_data)
{
    struct class *class = malloc(sizeof(struct class));
    *class = (struct class) {
        .methods = methods,
        .count = count,
        .class_data = class_data,
        .reg = UNASSIGNED_REGISTER,
    };
    return class;
}

error__t create_class(
    const char *class_name, const char **line,
    unsigned int count, struct class **class, struct type **type)
{
    const struct class_methods *methods = NULL;
    void *class_data;
    const char *default_type;
    return
        lookup_class(class_name, &methods)  ?:
        DO(default_type = methods->default_type)  ?:
        methods->init(methods, count, &class_data)  ?:
        DO(*class = create_class_block(methods, count, class_data))  ?:

        /* Figure out which type to generate.  If a type is specified and we
         * don't consume it then an error will be reported. */
        IF(default_type,
            /* If no type specified use the default. */
            IF(**line == '\0', DO(line = &default_type))  ?:
            create_type(line, methods->force_type, count, type));
}

error__t class_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    return
        IF(class->methods->parse_register,
            class->methods->parse_register(
                class, block_name, field_name, line));
}

error__t validate_class(struct class *class)
{
    return
        IF(class->methods->validate,
            class->methods->validate(class));
}

const char *get_class_name(struct class *class)
{
    return class->methods->name;
}

void destroy_class(struct class *class)
{
    if (class->methods->destroy)
        class->methods->destroy(class);
    free(class);
}

error__t initialise_classes(void)
{
    return ERROR_OK;
}

void terminate_classes(void)
{
}
