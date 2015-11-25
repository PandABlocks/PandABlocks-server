#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <math.h>

#include "error.h"
#include "hashtable.h"
#include "parse.h"
#include "config_server.h"
#include "fields.h"
#include "types.h"
#include "attributes.h"
#include "hardware.h"
#include "capture.h"
#include "register.h"
#include "time_class.h"
#include "table.h"

#include "classes.h"



struct class {
    const struct class_methods *methods;    // Class implementation
    unsigned int count;             // Number of instances of this block
    void *class_data;               // Class specific data
};


/*****************************************************************************/
/* Individual class implementations. */


/* Support for classes with simple typed registers. */
struct typed_register {
    struct register_api *reg;
    struct type *type;
};



static error__t typed_register_parse_register(
    void *class_data, const char *block_name, const char *field_name,
    const char **line)
{
    struct typed_register *state = class_data;
    return register_parse_register(line, state->reg);
}


static error__t typed_register_parse_attribute(
    void *class_data, const char **line)
{
    struct typed_register *state = class_data;
    return type_parse_attribute(state->type, line);
}


/* We can delegate the change set calculation to the register class. */
static void typed_register_change_set(
    void *class_data, const uint64_t report_index[], bool changes[])
{
    struct typed_register *state = class_data;
    register_change_set(state->reg, report_index, changes);
}


static error__t typed_register_finalise(
    void *class_data, unsigned int block_base)
{
    struct typed_register *state = class_data;
    return finalise_register(state->reg, block_base);
}


static error__t typed_register_get(
    void *class_data, unsigned int number, struct connection_result *result)
{
    struct typed_register *state = class_data;
    return type_get(state->type, number, result);
}


static error__t typed_register_put(
    void *class_data, unsigned int number, const char *string)
{
    struct typed_register *state = class_data;
    return type_put(state->type, number, string);
}


static struct typed_register *create_typed_register_block(
    struct register_api *reg, struct type *type)
{
    struct typed_register *state = malloc(sizeof(struct typed_register));
    *state = (struct typed_register) {
        .reg = reg,
        .type = type,
    };
    return state;
}


static error__t typed_register_init(
    struct register_api *reg,
    const char **line, unsigned int count, void **class_data)
{
    /* Default type to "uint" if not given. */
    const char *default_type = "uint";
    if (**line == '\0')
        line = &default_type;

    struct type *type;
    return
        create_type(line, count, reg, &type)  ?:
        DO(*class_data = create_typed_register_block(reg, type));
}


static void typed_register_destroy(void *class_data)
{
    struct typed_register *state = class_data;
    destroy_register(state->reg);
    destroy_type(state->type);
}


static error__t param_init(
    const char **line, unsigned int count, void **class_data)
{
    return typed_register_init(
        create_param_register(count), line, count, class_data);
}


static const struct class_methods param_class_methods = {
    "param",
    .init = param_init,
    .destroy = typed_register_destroy,
    .parse_attribute = typed_register_parse_attribute,
    .parse_register = typed_register_parse_register,
    .finalise = typed_register_finalise,
    .get = typed_register_get,
    .put = typed_register_put,
    .change_set = typed_register_change_set
};



static error__t read_init(
    const char **line, unsigned int count, void **class_data)
{
    return typed_register_init(
        create_read_register(count), line, count, class_data);
}

static const struct class_methods read_class_methods = {
    "read",
    .init = read_init,
    .destroy = typed_register_destroy,
    .parse_attribute = typed_register_parse_attribute,
    .parse_register = typed_register_parse_register,
    .finalise = typed_register_finalise,
    .get = typed_register_get,
    .change_set = typed_register_change_set,
};


static error__t  write_init(
    const char **line, unsigned int count, void **class_data)
{
    return typed_register_init(
        create_write_register(count), line, count, class_data);
}

static const struct class_methods write_class_methods = {
    "write",
    .init = write_init,
    .destroy = typed_register_destroy,
    .parse_attribute = typed_register_parse_attribute,
    .parse_register = typed_register_parse_register,
    .finalise = typed_register_finalise,
    .put = typed_register_put,
};




/*****************************************************************************/
/* External API. */

/* Class field access. */

error__t class_get(
    struct class *class, unsigned int number, bool refresh,
    struct connection_result *result)
{
    return
        TEST_OK_(class->methods->get, "Field not readable")  ?:
        class->methods->get(class->class_data, number, result);
}


error__t class_put(
    struct class *class, unsigned int number, const char *string)
{
    return
        TEST_OK_(class->methods->put, "Field not writeable")  ?:
        class->methods->put(class->class_data, number, string);
}


error__t class_put_table(
    struct class *class, unsigned int number,
    bool append, struct put_table_writer *writer)
{
    return
        TEST_OK_(class->methods->put_table, "Field is not a table")  ?:
        class->methods->put_table(class->class_data, number, append, writer);
}


/* Change support. */

void refresh_class_changes(enum change_set change_set)
{
    if (change_set & CHANGES_BITS)
        do_bit_out_refresh();
    if (change_set & CHANGES_POSITION)
        do_pos_out_refresh();
}


void get_class_change_set(
    struct class *class, const uint64_t report_index[], bool changes[])
{
    if (class->methods->change_set)
        class->methods->change_set(class->class_data, report_index, changes);
    else
        memset(changes, 0, sizeof(bool) * class->count);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Class inititialisation. */


/* Top level list of classes. */

static const struct class_methods *classes_table[] = {
    &param_class_methods,           // param

    &read_class_methods,            // read
    &write_class_methods,           // write

    &time_class_methods,            // time

    &bit_out_class_methods,         // bit_out
    &pos_out_class_methods,         // pos_out

    &short_table_class_methods,     // short_table
    &long_table_class_methods,      // long_table
};


static error__t lookup_class(
    const char *name, const struct class_methods **result)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(classes_table); i ++)
    {
        const struct class_methods *methods = classes_table[i];
        if (strcmp(name, methods->name) == 0)
        {
            *result = methods;
            return ERROR_OK;
        }
    }
    return FAIL_("Class %s not found", name);
}

static struct class *create_class_block(
    const struct class_methods *methods, unsigned int count, void *class_data)
{
    struct class *class = malloc(sizeof(struct class));
    *class = (struct class) {
        .methods = methods,
        .count = count,
        .class_data = class_data,
    };
    return class;
}

error__t create_class(
    const char *class_name, const char **line, unsigned int count,
    struct class **class)
{
    const struct class_methods *methods = NULL;
    void *class_data = NULL;
    return
        lookup_class(class_name, &methods)  ?:
        methods->init(line, count, &class_data)  ?:
        DO(*class = create_class_block(methods, count, class_data));
}


void create_class_attributes(
    struct class *class, struct hash_table *attr_map)
{
    for (unsigned int i = 0; i < class->methods->attr_count; i ++)
        create_attribute(
            &class->methods->attrs[i], class, class->class_data,
            class->count, attr_map);
}


error__t class_parse_attribute(struct class *class, const char **line)
{
    return
        TEST_OK_(class->methods->parse_attribute,
            "Cannot add attribute to this field")  ?:
        class->methods->parse_attribute(class->class_data, line);
}


error__t class_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    return
        TEST_OK(class->methods->parse_register)  ?:
        class->methods->parse_register(
            class->class_data, block_name, field_name, line);
}


error__t finalise_class(struct class *class, unsigned int block_base)
{
    return IF(class->methods->finalise,
        class->methods->finalise(class->class_data, block_base));
}

void describe_class(struct class *class, char *string, size_t length)
{
    size_t written =
        (size_t) snprintf(string, length, "%s", class->methods->name);
    if (class->methods->describe)
        snprintf(string + written, length - written, " %s",
            class->methods->describe(class->class_data));
}

void destroy_class(struct class *class)
{
    if (class->methods->destroy)
        class->methods->destroy(class->class_data);
    free(class->class_data);
    free(class);
}
