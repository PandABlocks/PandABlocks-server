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



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*****************************************************************************/
/* Individual class implementations. */


/* Support for classes with simple typed registers. */
struct typed_register {
    struct register_api *reg;
    struct type *type;
};


// !!!!!! These two methods will disappear shortly!

static uint32_t typed_register_read(struct class *class, unsigned int number)
{
    struct typed_register *state = class->class_data;
    return read_register(state->reg, number);
}

static void typed_register_write(
    struct class *class, unsigned int number, uint32_t value)
{
    struct typed_register *state = class->class_data;
    write_register(state->reg, number, value);
}



static error__t typed_register_parse_register(
    struct class *class, const char *block_name, const char *field_name,
    const char **line)
{
    struct typed_register *state = class->class_data;
    return register_parse_register(line, state->reg);
}


/* We can delegate the change set calculation to the register class. */
static void typed_register_change_set(
    struct class *class, const uint64_t report_index[], bool changes[])
{
    struct typed_register *state = class->class_data;
    register_change_set(state->reg, report_index, changes);
}


static error__t typed_register_validate(
    struct class *class, unsigned int block_base)
{
    struct typed_register *state = class->class_data;
    return validate_register(state->reg, block_base);
}


/* Formatting with register and type. */
static error__t type_and_register_get(
    struct register_api *reg, struct type *type,
    unsigned int number, struct connection_result *result)
{
    uint32_t value = read_register(reg, number);
    return
        type_format(type, number, value, result->string, result->length)  ?:
        DO(result->response = RESPONSE_ONE);
}

static error__t type_and_register_put(
    struct register_api *reg, struct type *type,
    unsigned int number, const char *string)
{
    uint32_t value;
    return
        type_parse(type, number, string, &value)  ?:
        DO(write_register(reg, number, value));
}


static error__t typed_register_get(
    struct class *class, unsigned int number, struct connection_result *result)
{
    struct typed_register *state = class->class_data;
    return type_and_register_get(state->reg, state->type, number, result);
}


static error__t typed_register_put(
    struct class *class, unsigned int number, const char *string)
{
    struct typed_register *state = class->class_data;
    return type_and_register_put(state->reg, state->type, number, string);
}


static void typed_register_init(
    struct register_api *reg, void **class_data)
{
    struct typed_register *state = malloc(sizeof(struct typed_register));
    *state = (struct typed_register) {
        .reg = reg,
        // .type to be moved down here shortly
    };
    *class_data = state;
}


static void typed_register_destroy(struct class *class)
{
    struct typed_register *state = class->class_data;
    destroy_register(state->reg);
}


static void param_init(unsigned int count, void **class_data)
{
    typed_register_init(create_param_register(count), class_data);
}


#define PARAM_CLASS_METHODS \
    .init = param_init, \
    .destroy = typed_register_destroy, \
    .parse_register = typed_register_parse_register, \
    .validate = typed_register_validate, \
    .read = typed_register_read, \
    .write = typed_register_write, \
    .get = typed_register_get, \
    .put = typed_register_put, \
    .change_set = typed_register_change_set


static const struct class_methods param_class_methods = {
    "param", "uint",
    PARAM_CLASS_METHODS,
};

static const struct class_methods bit_in_class_methods = {
    "bit_in", "bit_mux", true,
    PARAM_CLASS_METHODS,
};

static const struct class_methods pos_in_class_methods = {
    "pos_in", "pos_mux", true,
    PARAM_CLASS_METHODS,
};



static void read_init(unsigned int count, void **class_data)
{
    typed_register_init(create_read_register(count), class_data);
}

static const struct class_methods read_class_methods = {
    "read", "uint",
    .init = read_init,
    .destroy = typed_register_destroy,
    .parse_register = typed_register_parse_register,
    .validate = typed_register_validate,
    .read = typed_register_read,
    .get = typed_register_get,
    .change_set = typed_register_change_set,
};


static void write_init(unsigned int count, void **class_data)
{
    typed_register_init(create_write_register(count), class_data);
}

static const struct class_methods write_class_methods = {
    "write", "uint",
    .init = write_init,
    .destroy = typed_register_destroy,
    .parse_register = typed_register_parse_register,
    .validate = typed_register_validate,
    .write = typed_register_write,
    .put = typed_register_put,
};




/*****************************************************************************/
/* External API. */

/* Class field access. */

error__t class_read(
    struct class *class, unsigned int number, uint32_t *value, bool refresh)
{
    return
        TEST_OK_(class->methods->read, "Field not readable")  ?:
        IF(refresh  &&  class->methods->refresh,
            DO(class->methods->refresh(class, number)))  ?:
        DO(*value = class->methods->read(class, number));
}


error__t class_write(struct class *class, unsigned int number, uint32_t value)
{
    return
        TEST_OK_(class->methods->write, "Field not writeable")  ?:
        DO(class->methods->write(class, number, value));
}


error__t class_get(
    struct class *class, unsigned int number, bool refresh,
    struct connection_result *result)
{
    /* For the moment we delegate this method to class_read if there is a type.
     * This is going to be rewritten shortly. */
    if (class->type)
    {
        uint32_t value;
        return
            class_read(class, number, &value, refresh)  ?:
            type_format(
                class->type, number, value, result->string, result->length)  ?:
            DO(result->response = RESPONSE_ONE);
    }
    else
        return
            TEST_OK_(class->methods->get, "Field not readable")  ?:
            class->methods->get(class, number, result);
}


error__t class_put(
    struct class *class, unsigned int number, const char *string)
{
    /* Same story as for class_get */
    if (class->type)
    {
        uint32_t value;
        return
            type_parse(class->type, number, string, &value)  ?:
            class_write(class, number, value);
    }
    else
        return
            TEST_OK_(class->methods->put, "Field not writeable")  ?:
            class->methods->put(class, number, string);
}


error__t class_put_table(
    struct class *class, unsigned int number,
    bool append, struct put_table_writer *writer)
{
    return
        TEST_OK_(class->methods->put_table, "Field is not a table")  ?:
        class->methods->put_table(class, number, append, writer);
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
        class->methods->change_set(class, report_index, changes);
    else
        memset(changes, 0, sizeof(bool) * class->count);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Class inititialisation. */


/* Top level list of classes. */

static const struct class_methods *classes_table[] = {
    &param_class_methods,           // param
    &bit_in_class_methods,          // bit_in
    &pos_in_class_methods,          // pos_in

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
        .block_base = UNASSIGNED_REGISTER,
        .field_register = UNASSIGNED_REGISTER,
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
    const char *default_type;
    return
        lookup_class(class_name, &methods)  ?:
        DO(default_type = methods->default_type)  ?:
        IF(methods->init,
            DO(methods->init(count, &class_data)))  ?:
        DO(*class = create_class_block(methods, count, class_data))  ?:

        /* Figure out which type to generate.  If a type is specified and we
         * don't consume it then an error will be reported. */
        IF(default_type,
            /* If no type specified use the default. */
            IF(**line == '\0', DO(line = &default_type))  ?:
            create_type(line, methods->force_type, count, &(*class)->type));
}


void create_class_attributes(
    struct class *class, struct hash_table *attr_map)
{
    for (unsigned int i = 0; i < class->methods->attr_count; i ++)
        create_attribute(
            &class->methods->attrs[i], class, class->class_data,
            class->count, attr_map);
    if (class->type)
        create_type_attributes(class, class->type, attr_map);
}


error__t class_parse_attribute(struct class *class, const char **line)
{
    return
        TEST_OK_(class->type, "Cannot add attribute to this field")  ?:
        type_parse_attribute(class->type, line);
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


error__t validate_class(struct class *class, unsigned int block_base)
{
    class->block_base = block_base;
    return
        IF(class->methods->validate,
            class->methods->validate(class, block_base));
}

void describe_class(struct class *class, char *string, size_t length)
{
    size_t written =
        (size_t) snprintf(string, length, "%s", class->methods->name);
    if (class->type)
        snprintf(string + written, length - written, " %s",
            get_type_name(class->type));
}

void destroy_class(struct class *class)
{
    if (class->methods->destroy)
        class->methods->destroy(class);
    free(class->class_data);
    if (class->type)
        destroy_type(class->type);
    free(class);
}
