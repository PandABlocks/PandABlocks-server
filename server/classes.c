#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
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
    bool initialised;               // Checked during finalisation
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
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
    struct class *class, enum change_set change_set,
    const uint64_t report_index[], bool changes[])
{
    unsigned int ix = class->methods->change_set_index;
    if (class->methods->change_set  &&  (change_set & (1U << ix)))
        class->methods->change_set(
            class->class_data, report_index[ix], changes);
    else
        memset(changes, 0, sizeof(bool) * class->count);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Global class attributes. */


static error__t info_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    describe_class(owner, result, length);
    return ERROR_OK;
}


static struct attr_methods info_attribute = {
    "INFO", .format = info_format,
};


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

    &table_class_methods,           // table
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

static void create_class_attributes(
    struct class *class, struct hash_table *attr_map)
{
    for (unsigned int i = 0; i < class->methods->attr_count; i ++)
        create_attribute(
            &class->methods->attrs[i], class, class->class_data,
            class->count, attr_map);

    create_attribute(
        &info_attribute, class, class->class_data, class->count, attr_map);
}

error__t create_class(
    const char **line, unsigned int count,
    struct hash_table *attr_map, struct class **class)
{
    char class_name[MAX_NAME_LENGTH];
    const struct class_methods *methods = NULL;
    void *class_data = NULL;
    return
        parse_name(line, class_name, sizeof(class_name))  ?:
        lookup_class(class_name, &methods)  ?:
        methods->init(line, count, attr_map, &class_data)  ?:
        DO(
            *class = create_class_block(methods, count, class_data);
            create_class_attributes(*class, attr_map));
}


error__t class_parse_attribute(struct class *class, const char **line)
{
    return
        TEST_OK_(class->methods->parse_attribute,
            "Cannot add attribute to this field")  ?:
        class->methods->parse_attribute(class->class_data, line);
}


error__t class_parse_register(
    struct class *class, struct field *field, const char **line)
{
    return
        TEST_OK(class->methods->parse_register)  ?:
        class->methods->parse_register(class->class_data, field, line)  ?:
        DO(class->initialised = true);
}


error__t finalise_class(struct class *class, unsigned int block_base)
{
    return
        /* Alas at this point we don't have a name or location to report. */
        TEST_OK_(class->initialised, "No register assigned for class")  ?:
        IF(class->methods->finalise,
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
