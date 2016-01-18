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
#include "output.h"
#include "register.h"
#include "time_position.h"
#include "table.h"
#include "capture.h"

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
    if (refresh  &&  class->methods->refresh)
        class->methods->refresh(class->class_data, number);
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

void refresh_class_changes(enum change_set change_set, uint64_t change_index)
{
    if (change_set & CHANGES_BITS)
        do_bit_out_refresh(change_index);
    if (change_set & CHANGES_POSITION)
        do_pos_out_refresh(change_index);
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


error__t describe_class(struct class *class, char *string, size_t length)
{
    if (class->methods->describe)
        return format_string(string, length, "%s %s",
            class->methods->name, class->methods->describe(class->class_data));
    else
        return format_string(string, length, "%s", class->methods->name);
}


struct enumeration *get_class_enumeration(const struct class *class)
{
    if (class->methods->get_enumeration)
        return class->methods->get_enumeration(class->class_data);
    else
        return NULL;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Global class attributes. */


static error__t info_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    return describe_class(owner, result, length);
}


static struct attr_methods info_attribute = {
    "INFO", "Class information for field",
    .format = info_format,
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

    &software_class_methods,        // software
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
        .initialised = !methods->parse_register,
    };
    return class;
}

static void create_class_attributes(
    struct class *class, struct hash_table *attr_map)
{
    create_attributes(
        class->methods->attrs, class->methods->attr_count,
        class, class->class_data, class->count, attr_map);

    create_attributes(
        &info_attribute, 1, class, class->class_data, class->count, attr_map);
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
    struct class *class, struct field *field, unsigned int block_base,
    const char **line)
{
    return
        TEST_OK_(class->methods->parse_register,
            "No register assignment expected for this class")  ?:
        TEST_OK_(!class->initialised, "Register already assigned")  ?:
        class->methods->parse_register(
            class->class_data, field, block_base, line)  ?:
        DO(class->initialised = true);
}


error__t finalise_class(struct class *class)
{
    return
        /* Alas at this point we don't have a name or location to report. */
        TEST_OK_(class->initialised, "No register assigned for class")  ?:
        IF(class->methods->finalise,
            class->methods->finalise(class->class_data));
}

void destroy_class(struct class *class)
{
    if (class->methods->destroy)
        class->methods->destroy(class->class_data);
    else
        free(class->class_data);
    free(class);
}
