/* Fields and field classes. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "error.h"
#include "hardware.h"
#include "hashtable.h"
#include "parse.h"
#include "config_server.h"
#include "types.h"
#include "attributes.h"
#include "output.h"
#include "bit_out.h"
#include "register.h"
#include "time_position.h"
#include "table.h"
#include "locking.h"
#include "metadata.h"

#include "fields.h"



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Core structure definitions. */

/* The top level entities are blocks. Each block has a name, a number of
 * instances, a register base used for all block register methods, and a table
 * of fields. */
struct block {
    char *name;                 // Block name
    unsigned int count;         // Number of instances of this block
    unsigned int base;          // Block register base
    struct hash_table *fields;  // Map from field name to fields
    char *description;          // User readable description
    uint32_t reg_used[BLOCK_REGISTER_COUNT / 32];   // Register assignment map
};


/* The field structure contains all of the data associated with a field.  Each
 * field has a name and a class and (depending on the class) possibly a type.
 * There is also a register associated with each field, and depending on the
 * class there may be further type specific data. */
struct field {
    struct block *block;            // Parent block
    char *name;                     // Field name
    const struct class_methods *methods;    // Class implementation
    unsigned int sequence;          // Field sequence number
    char *description;              // User readable description
    struct hash_table *attrs;       // Attribute lookup table
    void *class_data;               // Class specific data
    bool initialised;               // Checked during finalisation
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Top level block and field API. */


/* Map of block names. */
static struct hash_table *block_map;


/* A couple of helpers for walking the block and field maps. */
#define _id_FOR_EACH_TYPE(ix, type, test, map, value) \
    size_t ix = 0; \
    for (type value; \
         (test)  &&  hash_table_walk(map, &ix, NULL, (void **) &value); )
/* Called thus:
 *  FOR_EACH_TYPE(type, test, map, value) { loop }
 *
 * type     Type of values in map
 * test     Condition for testing loop: loops while test is true
 * map      Hash table containing set of value for iteration
 * value    Name of variable to which each value is assigned. */
#define FOR_EACH_TYPE(args...)  _id_FOR_EACH_TYPE(UNIQUE_ID(), args)

#define FOR_EACH_BLOCK_WHILE(cond, block_var) \
    FOR_EACH_TYPE(struct block *, cond, block_map, block_var)
#define FOR_EACH_BLOCK(block_var) FOR_EACH_BLOCK_WHILE(true, block_var)

#define FOR_EACH_FIELD_WHILE(args...) FOR_EACH_TYPE(struct field *, args)
#define FOR_EACH_FIELD(args...) FOR_EACH_FIELD_WHILE(true, args)


error__t lookup_block(
    const char *name, struct block **block, unsigned int *count)
{
    return
        TEST_OK_(*block = hash_table_lookup(block_map, name),
            "No such block")  ?:
        DO(if (count)  *count = (*block)->count);
}


error__t lookup_field(
    const struct block *block, const char *name, struct field **field)
{
    return TEST_OK_(
        *field = hash_table_lookup(block->fields, name),
        "No such field");
}


error__t lookup_attr(
    const struct field *field, const char *name, struct attr **attr)
{
    /* Both classes and types can have attributes.  Try the type attribute
     * first, fail if neither succeeds. */
    return
        TEST_OK_(*attr = hash_table_lookup(field->attrs, name),
            "No such attribute");
}


const char *get_block_description(struct block *block)
{
    return block->description;
}

const char *get_field_description(struct field *field)
{
    return field->description;
}


error__t block_list_get(struct connection_result *result)
{
    FOR_EACH_BLOCK(block)
        format_many_result(result, "%s %d", block->name, block->count);
    return ERROR_OK;
}


static error__t describe_field(
    struct field *field, char string[], size_t length)
{
    const char *extra = field->methods->describe ?
        field->methods->describe(field->class_data) : NULL;
    if (extra)
        return format_string(
            string, length, "%s %s", field->methods->name, extra);
    else
        return format_string(string, length, "%s", field->methods->name);
}


error__t field_list_get(
    const struct block *block, struct connection_result *result)
{
    FOR_EACH_FIELD(block->fields, field)
    {
        size_t length = (size_t) snprintf(
            result->string, result->length,
            "%s %u ", field->name, field->sequence);
        error__t error = describe_field(
            field, result->string + length, result->length - length);
        ASSERT_OK(!error);

        result->write_many(result->write_context, result->string);
    }
    return ERROR_OK;
}


error__t attr_list_get(struct field *field, struct connection_result *result)
{
    size_t ix = 0;
    const void *key;
    while (hash_table_walk(field->attrs, &ix, &key, NULL))
        result->write_many(result->write_context, key);
    return ERROR_OK;
}


const struct enumeration *get_field_enumeration(const struct field *field)
{
    if (field->methods->get_enumeration)
        return field->methods->get_enumeration(field->class_data);
    else
        return NULL;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Field read and write. */

error__t field_get(
    struct field *field, unsigned int number, struct connection_result *result)
{
    if (field->methods->refresh)
        field->methods->refresh(field->class_data, number);

    if (field->methods->get)
    {
        result->response = RESPONSE_ONE;
        return field->methods->get(
            field->class_data, number, result->string, result->length);
    }
    else if (field->methods->get_many)
    {
        result->response = RESPONSE_MANY;
        return field->methods->get_many(field->class_data, number, result);
    }
    else
        return FAIL_("Field not readable");
}


error__t field_put(struct field *field, unsigned int number, const char *string)
{
    return
        TEST_OK_(field->methods->put, "Field not writeable")  ?:
        field->methods->put(field->class_data, number, string);
}


error__t field_put_table(
    struct field *field, unsigned int number,
    bool append, bool binary, struct put_table_writer *writer)
{
    return
        TEST_OK_(field->methods->put_table, "Field is not a table")  ?:
        field->methods->put_table(
            field->class_data, number, append, binary, writer);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Change set management. */

static pthread_mutex_t change_mutex = PTHREAD_MUTEX_INITIALIZER;


/* Alas it is possible for an error to be detected during formatting when
 * generating a change report.  If this occurs we back up over the value being
 * written and write an error mark instead. */
static void handle_error_report(
    char string[], size_t length, size_t prefix, error__t error)
{
    if (error)
    {
        prefix -= 1;
        string[prefix] = '\0';
        ERROR_REPORT(error, "Error reporting *CHANGES for %s", string);
        snprintf(string + prefix, length - prefix, " (error)");
    }
}


size_t format_field_name(
    char string[], size_t length,
    const struct field *field, const struct attr *attr,
    unsigned int number, char suffix)
{
    size_t result;
    if (field->block->count == 1)
        result = (size_t) snprintf(string, length, "%s.%s",
            field->block->name, field->name);
    else
        result = (size_t) snprintf(string, length, "%s%d.%s",
            field->block->name, number + 1, field->name);
    ASSERT_OK(result < length);
    if (attr)
        result += (size_t) snprintf(string + result, length - result,
            ".%s", get_attr_name(attr));
    ASSERT_OK(result < length + 1);
    string[result++] = suffix;
    string[result] = '\0';
    return result;
}


/* Placeholder for formatting result.  If the class wants to return a multi-line
 * result then this let's us just throw it away. */
static void dummy_write_many(void *write_context, const char *string) { }

static void report_changed_value(
    const struct field *field, unsigned int number,
    struct connection_result *result)
{
    char string[MAX_RESULT_LENGTH];
    size_t prefix =
        format_field_name(string, sizeof(string), field, NULL, number, '=');

    handle_error_report(string, sizeof(string), prefix,
        TEST_OK_(field->methods->get, "Field not readable")  ?:
        field->methods->get(
            field->class_data, number,
            string + prefix, sizeof(string) - prefix));
    result->write_many(result->write_context, string);
}


static void report_changed_attr(
    struct field *field, struct attr *attr, unsigned int number,
    struct connection_result *result)
{
    char string[MAX_RESULT_LENGTH];
    size_t prefix =
        format_field_name(string, sizeof(string), field, attr, number, '=');

    struct connection_result format_result = {
        .string = string + prefix,
        .length = sizeof(string) - prefix,
        .write_many = dummy_write_many,
    };
    handle_error_report(string, sizeof(string), prefix,
        attr_get(attr, number, &format_result)  ?:
        TEST_OK(format_result.response == RESPONSE_ONE));
    result->write_many(result->write_context, string);
}


static void report_changed_table(
    struct field *field, unsigned int number,
    struct connection_result *result, bool print_table)
{
    char string[MAX_RESULT_LENGTH];
    format_field_name(string, sizeof(string), field, NULL, number, '<');
    if (print_table)
    {
        strcat(string, "B");
        struct attr *bin_attr;
        result->write_many(result->write_context, string);
        error_report(
            lookup_attr(field, "B", &bin_attr)  ?:
            attr_get(bin_attr, number, result));
        result->write_many(result->write_context, "");
    }
    else
        result->write_many(result->write_context, string);
}


static void generate_attr_change_sets(
    struct connection_result *result, struct field *field,
    uint64_t report_index)
{
    /* Also work through all attributes for their change sets. */
    FOR_EACH_TYPE(struct attr *, true, field->attrs, attr)
    {
        bool changes[field->block->count];
        get_attr_change_set(attr, report_index, changes);
        for (unsigned int i = 0; i < field->block->count; i ++)
            if (changes[i])
                report_changed_attr(field, attr, i, result);
    }
}


static void refresh_change_index(
    struct change_set_context *change_set_context,
    enum change_set change_set, uint64_t report_index[])
{
    LOCK(change_mutex);
    uint64_t change_index = update_change_index(
        change_set_context, change_set, report_index);
    if (change_set & CHANGES_BITS)
        do_bit_out_refresh(change_index);
    if (change_set & CHANGES_POSITION)
        do_pos_out_refresh(change_index);
    UNLOCK(change_mutex);
}


static void get_field_change_set(
    struct field *field, enum change_set change_set,
    const uint64_t report_index[], bool changes[])
{
    unsigned int ix = field->methods->change_set_index;
    if (field->methods->change_set  &&  (change_set & (1U << ix)))
        field->methods->change_set(
            field->class_data, report_index[ix], changes);
    else
        memset(changes, 0, sizeof(bool) * field->block->count);
}



/* Walks all fields and generates a change event for all changed fields. */
void generate_change_sets(
    struct connection_result *result, enum change_set change_set,
    bool print_tables)
{
    /* Get the change index for this connection and update it so the next
     * changes request will be up to date.  Use a fresh index for this. */
    uint64_t report_index[CHANGE_SET_SIZE];
    refresh_change_index(result->change_set_context, change_set, report_index);

    /* Work through all fields in all blocks. */
    FOR_EACH_BLOCK(block)
    {
        FOR_EACH_FIELD(block->fields, field)
        {
            bool changes[block->count];
            get_field_change_set(
                field, change_set & (enum change_set) ~CHANGES_TABLE,
                report_index, changes);
            for (unsigned int i = 0; i < block->count; i ++)
                if (changes[i])
                    report_changed_value(field, i, result);

            /* We need to report table changes separately, totally different
             * reporting syntax! */
            get_field_change_set(
                field, change_set & CHANGES_TABLE,
                report_index, changes);
            for (unsigned int i = 0; i < block->count; i ++)
                if (changes[i])
                    report_changed_table(field, i, result, print_tables);

            if (change_set & CHANGES_ATTR)
                generate_attr_change_sets(
                    result, field, report_index[CHANGE_IX_ATTR]);
        }
    }

    if (change_set & CHANGES_METADATA)
        generate_metadata_change_set(
            result, report_index[CHANGE_IX_METADATA], print_tables);
}


/* This is a cut down version of generate_change_sets without reporting. */
bool check_change_set(
    struct change_set_context *change_set_context, enum change_set change_set)
{
    uint64_t report_index[CHANGE_SET_SIZE];
    refresh_change_index(change_set_context, change_set, report_index);

    FOR_EACH_BLOCK(block)
    {
        FOR_EACH_FIELD(block->fields, field)
        {
            bool changes[block->count];
            get_field_change_set(
                field, change_set, report_index, changes);
            for (unsigned int i = 0; i < block->count; i ++)
                if (changes[i])
                    return true;

            if (change_set & CHANGES_ATTR)
            {
                FOR_EACH_TYPE(struct attr *, true, field->attrs, attr)
                {
                    get_attr_change_set(
                        attr, report_index[CHANGE_IX_ATTR], changes);
                    for (unsigned int i = 0; i < block->count; i ++)
                        if (changes[i])
                            return true;
                }
            }
        }
    }

    if ((change_set & CHANGES_METADATA)  &&
            check_metadata_change_set(report_index[CHANGE_IX_METADATA]))
        return true;
    return false;
}


/* To reset the change set it's enough to request a fresh index, and then
 * discard the result. */
void reset_change_set(
    struct change_set_context *context, enum change_set change_set,
    enum reset_change_set_action action)
{
    switch (action)
    {
        case RESET_START:
            reset_change_index(context, change_set);
            break;
        case RESET_END:
        {
            uint64_t report_index[CHANGE_SET_SIZE];
            refresh_change_index(context, change_set, report_index);
            break;
        }
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Initialisation and shutdown. */


error__t initialise_fields(void)
{
    /* Top level block map.  This will be initialised from the configuration
     * file. */
    block_map = hash_table_create(false);
    return ERROR_OK;
}


/* We implement orderly shutdown so that we can easily detect memory leaks
 * during development. */

static void destroy_field(struct field *field)
{
    free(field->name);
    if (field->methods->destroy)
        field->methods->destroy(field->class_data);
    else
        free(field->class_data);
    free(field->description);
    delete_attributes(field->attrs);
    hash_table_destroy(field->attrs);
    free(field);
}

static void destroy_block(struct block *block)
{
    FOR_EACH_FIELD(block->fields, field)
        destroy_field(field);
    hash_table_destroy(block->fields);
    free(block->name);
    free(block->description);
    free(block);
}

void terminate_fields(void)
{
    if (block_map)
    {
        FOR_EACH_BLOCK(block)
            destroy_block(block);
        hash_table_destroy(block_map);
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Top level list of classes. */

static const struct class_methods *classes_table[] = {
    &param_class_methods,           // param

    &read_class_methods,            // read
    &write_class_methods,           // write

    &time_class_methods,            // time

    &bit_out_class_methods,         // bit_out
    &pos_out_class_methods,         // pos_out
    &ext_out_class_methods,         // ext_out

    &bit_mux_class_methods,         // bit_mux
    &pos_mux_class_methods,         // pos_mux

    &table_class_methods,           // table
};



/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Block creation. */


error__t create_block(
    struct block **block, const char *name, unsigned int count)
{
    *block = malloc(sizeof(struct block));
    **block = (struct block) {
        .name = strdup(name),
        .count = count,
        .base = UNASSIGNED_REGISTER,
        .fields = hash_table_create(false),
    };
    return TEST_OK_(
        hash_table_insert(block_map, (*block)->name, *block) == NULL,
        "Block %s already exists", name);
}


error__t block_set_register(struct block *block, unsigned int base)
{
    return
        TEST_OK_(block->base == UNASSIGNED_REGISTER,
            "Register already assigned")  ?:
        DO(block->base = base);
}


error__t block_set_description(struct block *block, const char *description)
{
    return
        TEST_OK_(block->description == NULL, "Description already set")  ?:
        DO(block->description = strdup(description));
}


error__t check_parse_register(
    struct field *field, const char **line, unsigned int *reg)
{
    return
        parse_uint(line, reg)  ?:
        TEST_OK_(*reg < BLOCK_REGISTER_COUNT, "Register value too large")  ?:
        TEST_OK_(
            (field->block->reg_used[*reg / 32] & (1U << (*reg % 32))) == 0,
            "Register %u already in use", *reg)  ?:
        DO(field->block->reg_used[*reg / 32] |= 1U << (*reg % 32));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Field and database creation and validation. */


static error__t info_format(
    void *owner, void *data, unsigned int number,
    char result[], size_t length)
{
    return describe_field(owner, result, length);
}


static struct attr_methods info_attribute = {
    "INFO", "Class information for field",
    .format = info_format,
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


static error__t create_field_attributes(struct field *field)
{
    create_attributes(
        field->methods->attrs, field->methods->attr_count,
        field, field->class_data, field->block->count, field->attrs);
    create_attributes(
        &info_attribute, 1, field, field->class_data,
        field->block->count, field->attrs);
    return ERROR_OK;
}


static struct field *create_field_block(
    struct block *block, const char *name,
    const struct class_methods *methods)
{
    struct field *field = malloc(sizeof(struct field));
    *field = (struct field) {
        .block = block,
        .name = strdup(name),
        .methods = methods,
        .sequence = (unsigned int) hash_table_count(block->fields),
        .attrs = hash_table_create(false),
    };
    return field;
}


error__t create_field(
    const char **line, struct field **field, struct block *block)
{
    char field_name[MAX_NAME_LENGTH];
    char class_name[MAX_NAME_LENGTH];
    const struct class_methods *methods = NULL;
    return
        parse_alphanum_name(line, field_name, sizeof(field_name))  ?:
        parse_whitespace(line)  ?:
        parse_name(line, class_name, sizeof(class_name))  ?:
        lookup_class(class_name, &methods)  ?:

        DO(*field = create_field_block(block, field_name, methods))  ?:
        TRY_CATCH(
            methods->init(
                line, block->count, (*field)->attrs, &(*field)->class_data)  ?:
            create_field_attributes(*field)  ?:
            /* Insert the field into the blocks map of fields. */
            TEST_OK_(
                hash_table_insert(
                    block->fields, (*field)->name, *field) == NULL,
                "Field %s.%s already exists", block->name, field_name),
        //catch
            DO(destroy_field(*field)));
}


error__t field_parse_attribute(struct field *field, const char **line)
{
    return
        TEST_OK_(field->methods->parse_attribute,
            "Cannot add attribute to this field")  ?:
        field->methods->parse_attribute(field->class_data, line);
}


error__t field_parse_registers(struct field *field, const char **line)
{
    return
        TEST_OK_(field->methods->parse_register,
            "No register assignment expected for this class")  ?:
        TEST_OK_(!field->initialised, "Register already assigned")  ?:
        field->methods->parse_register(
            field->class_data, field, field->block->base, line)  ?:
        DO(field->initialised = true);
}


error__t field_set_description(struct field *field, const char *description)
{
    return
        TEST_OK_(field->description == NULL, "Description already set")  ?:
        DO(field->description = strdup(description));
}


/* Ensure that every block and field has valid register assignments. */
error__t validate_fields(void)
{
    error__t error = ERROR_OK;
    FOR_EACH_BLOCK_WHILE(!error, block)
    {
        error = TEST_OK_(block->base != UNASSIGNED_REGISTER,
            "No base address for block %s", block->name);
        if (block->description == NULL)
            log_message("No description for block %s", block->name);
        else if (*block->description == '\0')
            log_message("Empty description for block %s", block->name);
        FOR_EACH_FIELD_WHILE(!error, block->fields, field)
        {
            error =
                TEST_OK_(field->initialised,
                    "No register assigned for class")  ?:
                IF(field->methods->finalise,
                    field->methods->finalise(field->class_data));
            if (error)
                error_extend(error,
                    "Checking field %s.%s", block->name, field->name);
            if (field->description == NULL)
                log_message("No description for field %s.%s",
                    block->name, field->name);
            else if (*field->description == '\0')
                log_message("Empty description for field %s.%s",
                    block->name, field->name);
        }
    }
    return error;
}
