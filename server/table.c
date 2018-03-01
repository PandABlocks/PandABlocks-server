/* Table access implementation. */

#include <stdbool.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>

#include "error.h"
#include "parse.h"
#include "hardware.h"
#include "config_server.h"
#include "fields.h"
#include "attributes.h"
#include "enums.h"
#include "base64.h"
#include "locking.h"

#include "table.h"


/* To interoperate nicely with persistence, this needs to be a multiple of 12 so
 * that the persistence layer can read lines back a line at a time -- which
 * needs each line to be a multiple of 4 bytes. */
#define BASE64_LINE_BYTES   48U     // Converts to 64 characters


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Field sets. */

/* Copied from enums.h with the consts removed.  Need to refactor this properly
 * another time. */
struct table_enum_entry { unsigned int value; char *name; };
struct table_enum_set { struct table_enum_entry *enums; size_t count; };

struct table_field {
    unsigned int left;
    unsigned int right;
    char *field_name;
    struct table_enum_set enums;    // If .count >0 then report field as an enum
};

/* This is used to implement a list of fields loaded from the config register
 * and reported to the client on demand. */
struct field_set {
    unsigned int field_count;
    unsigned int capacity;
    struct table_field *fields;
};


static void field_set_destroy(struct field_set *set)
{
    for (unsigned int i = 0; i < set->field_count; i ++)
    {
        struct table_field *field = &set->fields[i];
        free(field->field_name);
        for (unsigned int j = 0; j < field->enums.count; j ++)
            free(field->enums.enums[j].name);
        free(field->enums.enums);
    }
    free(set->fields);
}


static error__t check_field_range(
    struct field_set *set, unsigned int left, unsigned int right)
{
    return ERROR_OK;
    return FAIL_("Range check not implemented yet");
}


static error__t add_table_enum(
    struct table_enum_set *enums, unsigned int value, const char *enum_string)
{
    enums->enums = realloc(
        enums->enums, (enums->count + 1) * sizeof(struct table_enum_entry));
    enums->enums[enums->count++] = (struct table_enum_entry) {
        .value = value,
        .name = strdup(enum_string),
    };
    /* Ought to check for duplicates, but then we're getting closer to
     * duplicating a true enum. */
    return ERROR_OK;
}


static error__t parse_field_enum(
    void *context, const char **line, struct indent_parser *parser)
{
    struct table_enum_set *enums = context;
    unsigned int value;
    const char *enum_string;
    return
        parse_uint(line, &value)  ?:
        parse_whitespace(line)  ?:
        parse_utf8_string(line, &enum_string)  ?:

        add_table_enum(enums, value, enum_string);
}


static void add_new_field(
    struct field_set *set, unsigned int left, unsigned int right,
    const char *field_name, struct indent_parser *parser)
{
    /* Ensure we have enough capacity. */
    if (set->field_count >= set->capacity)
    {
        set->capacity *= 2;
        set->fields =
            realloc(set->fields, set->capacity * sizeof(struct table_field));
    }

    /* Assign the new field. */
    struct table_field *field = &set->fields[set->field_count++];
    *field = (struct table_field) {
        .left = left,
        .right = right,
        .field_name = strdup(field_name),
    };

    /* Allow for parsing enums associated with this field. */
    *parser = (struct indent_parser) {
        .context = &field->enums,
        .parse_line = parse_field_enum,
    };
}


static error__t field_set_parse_attribute(
    void *context, const char **line, struct indent_parser *parser)
{
    struct field_set *set = context;
    unsigned int left;
    unsigned int right;
    char field_name[MAX_NAME_LENGTH];
    return
        parse_uint(line, &left)  ?:
        parse_char(line, ':')  ?:
        parse_uint(line, &right)  ?:
        parse_whitespace(line)  ?:
        parse_alphanum_name(line, field_name, sizeof(field_name))  ?:

        check_field_range(set, left, right)  ?:
        DO(add_new_field(set, left, right, field_name, parser));
}


static error__t field_set_fields_get_many(
    struct field_set *set, struct connection_result *result)
{
    for (unsigned int i = 0; i < set->field_count; i ++)
    {
        struct table_field *field = &set->fields[i];
        format_many_result(result,
            "%u:%u %s%s", field->left, field->right, field->field_name,
            field->enums.count ? " enum" : "");
    }
    return ERROR_OK;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Table structures. */


struct table_block {
    unsigned int number;        // Index of this block
    uint64_t update_index;      // Timestamp of last change

    const uint32_t *data;       // Data area for block
    size_t length;              // Current length of block

    /* Writes to the table are double buffered.  We allocate a dedicated write
     * buffer for use during write, this is protected by write_lock.  When
     * updating the block we need to take the read_lock as well for writing. */
    uint32_t *write_data;       // Transient data area while writing
    size_t write_length;        // Current data write length in words
    size_t write_offset;        // Offset data will start at when completed
    bool write_binary;          // Set if data is being written in binary

    pthread_mutex_t write_lock; // Locks access to write_data area
    pthread_rwlock_t read_lock; // Write access taken when updating length&data
};


struct table_state {
    unsigned int block_count;   // Number of block instances
    struct field_set field_set; // Set of fields in table
    size_t max_length;          // Maximum block length in words
    struct hw_table *table;     // Hardware interface to tables

    struct table_block blocks[];    // Individual table blocks
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Table block methods. */


static void initialise_table_blocks(
    struct table_block blocks[], unsigned int block_count)
{
    for (unsigned int i = 0; i < block_count; i ++)
    {
        blocks[i] = (struct table_block) {
            .number = i,
            .update_index = 1,
            .write_lock = PTHREAD_MUTEX_INITIALIZER,
            .read_lock = PTHREAD_RWLOCK_INITIALIZER,
        };
    }
}


static error__t write_ascii(
    struct table_block *block, struct connection_result *result)
{
    for (unsigned int i = 0; i < block->length; i ++)
        format_many_result(result, "%u", block->data[i]);
    return ERROR_OK;
}


static error__t write_base_64(
    struct table_block *block, struct connection_result *result)
{
    size_t length = block->length * sizeof(uint32_t);
    const void *data = block->data;
    while (length > 0)
    {
        size_t to_write = MIN(length, BASE64_LINE_BYTES);
        char line[MAX_RESULT_LENGTH];
        base64_encode(data, to_write, line);
        result->write_many(result->write_context, line);
        length -= to_write;
        data += to_write;
    }
    return ERROR_OK;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Writing to table. */


/* Fills buffer from a binary stream by reading exactly the requested number of
 * values as bytes. */
static error__t convert_base64_line(
    const char *line, uint32_t *data, size_t length, size_t *converted)
{
    size_t converted_bytes;
    enum base64_status status =
        base64_decode(line, data, length * sizeof(uint32_t), &converted_bytes);
    *converted = converted_bytes / sizeof(uint32_t);
    return
        TEST_OK_(status == BASE64_STATUS_OK,
            "%s", base64_error_string(status))  ?:
        TEST_OK_(converted_bytes % sizeof(uint32_t) == 0,
            "Invalid data length");
}


/* In ASCII mode we accept a sequence of numbers on each line. */
static error__t convert_ascii_line(
    const char *line, uint32_t *data, size_t length, size_t *converted)
{
    *converted = 0;
    error__t error = ERROR_OK;
    while (!error  &&  *line != '\0')
        error =
            TEST_OK_(*converted < length, "Too many points for table")  ?:
            parse_uint32(&line, &data[(*converted)++]);
    return error;
}


/* Called repeatedly to write lines to the table write area. */
static error__t write_table_line(void *context, const char *line)
{
    struct table_block *block = context;
    struct table_state *state =
        container_of(block, struct table_state, blocks[block->number]);

    /* Compute the available buffer length. */
    size_t max_length =
        state->max_length - block->write_offset - block->write_length;
    uint32_t *write_data = block->write_data + block->write_length;
    size_t length;
    return
        IF_ELSE(block->write_binary,
            convert_base64_line(line, write_data, max_length, &length),
        //else
            convert_ascii_line(line, write_data, max_length, &length))  ?:
        DO(block->write_length += length);
}


/* When a write is complete we copy the write data over to the live data.  This
 * is done under a write lock on the read/write lock.  Before releasing the
 * locks, we also do any required hardware finalisation. */
static void complete_table_write(void *context, bool write_ok)
{
    struct table_block *block = context;
    struct table_state *state =
        container_of(block, struct table_state, blocks[block->number]);

    if (write_ok)
    {
        LOCKW(block->read_lock);

        /* Write the data. */
        hw_write_table(
            state->table, block->number,
            block->write_offset, block->write_data, block->write_length);
        block->length = block->write_length + block->write_offset;

        block->update_index = get_change_index();

        UNLOCKRW(block->read_lock);
    }

    UNLOCK(block->write_lock);
}


/* Writing is rejected if there is another write to the same table in progress
 * simultaneously. */
static error__t start_table_write(
    struct table_block *block,
    bool append, bool binary, struct put_table_writer *writer)
{
    int result = pthread_mutex_trylock(&block->write_lock);
    if (result == EBUSY)
        return FAIL_("Table currently being written");
    else if (result)
        return TEST_PTHREAD(result);
    else
    {
        *writer = (struct put_table_writer) {
            .context = block,
            .write = write_table_line,
            .close = complete_table_write,
        };

        /* If appending is requested adjust the data area length accordingly. */
        block->write_offset = append ? block->length : 0;
        block->write_binary = binary;
        block->write_length = 0;
        return ERROR_OK;
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Table methods. */

#define INITIAL_CAPACITY    4

static error__t table_init(
    const char **line, unsigned int block_count,
    struct hash_table *attr_map, void **class_data,
    struct indent_parser *parser)
{
    struct table_state *state = malloc(
        sizeof(struct table_state) +
        block_count * sizeof(struct table_block));
    *state = (struct table_state) {
        .block_count = block_count,
        .field_set = {
            .capacity = INITIAL_CAPACITY,
            .fields = calloc(INITIAL_CAPACITY, sizeof(struct table_field)),
        },
    };
    initialise_table_blocks(state->blocks, block_count);

    *parser = (struct indent_parser) {
        .context = &state->field_set,
        .parse_line = field_set_parse_attribute,
    };

    *class_data = state;
    return ERROR_OK;
}


static void table_destroy(void *class_data)
{
    struct table_state *state = class_data;

    field_set_destroy(&state->field_set);
    for (unsigned int i = 0; i < state->block_count; i ++)
        free(state->blocks[i].write_data);

    if (state->table)
        hw_close_table(state->table);
    free(state);
}


static error__t short_table_parse_register(
    struct table_state *state, struct field *field,
    unsigned int block_base, const char **line)
{
    unsigned int max_length;
    unsigned int init_reg, fill_reg, length_reg;
    return
        /* Register specification is block size followed by the three control
         * registers. */
        parse_uint(line, &max_length)  ?:
        parse_whitespace(line)  ?:
        check_parse_register(field, line, &init_reg)  ?:
        parse_whitespace(line)  ?:
        check_parse_register(field, line, &fill_reg)  ?:
        parse_whitespace(line)  ?:
        check_parse_register(field, line, &length_reg)  ?:

        DO(state->max_length = (size_t) max_length)  ?:
        hw_open_short_table(
            block_base, state->block_count,
            init_reg, fill_reg, length_reg, state->max_length, &state->table);
}


static error__t long_table_parse_register(
    struct table_state *state, struct field *field,
    unsigned int block_base, const char **line)
{
    unsigned int table_order;
    unsigned int base_reg;
    unsigned int length_reg;
    return
        parse_whitespace(line)  ?:
        parse_char(line, '2')  ?:  parse_char(line, '^')  ?:    // 2^order
        parse_uint(line, &table_order)  ?:
        parse_whitespace(line)  ?:
        check_parse_register(field, line, &base_reg)  ?:
        parse_whitespace(line)  ?:
        check_parse_register(field, line, &length_reg)  ?:

        hw_open_long_table(
            block_base, state->block_count, table_order,
            base_reg, length_reg,
            &state->table, &state->max_length);
}


static void allocate_data_areas(struct table_state *state)
{
    size_t block_size = state->max_length * sizeof(uint32_t);
    for (unsigned int i = 0; i < state->block_count; i ++)
    {
        state->blocks[i].data = hw_read_table_data(state->table, i);
        state->blocks[i].write_data = malloc(block_size);
    }
}


static error__t table_parse_register(
    void *class_data, struct field *field, unsigned int block_base,
    const char **line)
{
    struct table_state *state = class_data;
    return
        parse_whitespace(line)  ?:
        IF_ELSE(read_string(line, "short"),
            short_table_parse_register(state, field, block_base, line),
        //else
        IF_ELSE(read_string(line, "long"),
            long_table_parse_register(state, field, block_base, line),
        //else
            FAIL_("Table type not recognised")))  ?:
        DO(allocate_data_areas(state));
}


static error__t table_get_many(
    void *class_data, unsigned int number,
    struct connection_result *result)
{
    struct table_state *state = class_data;
    struct table_block *block = &state->blocks[number];
    return WITH_LOCKR(block->read_lock,
        write_ascii(block, result));
}


static void table_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct table_state *state = class_data;
    for (unsigned int i = 0; i < state->block_count; i ++)
    {
        struct table_block *block = &state->blocks[i];
        LOCKR(block->read_lock);
        changes[i] = block->update_index > report_index;
        UNLOCKRW(block->read_lock);
    }
}


static error__t table_put_table(
    void *class_data, unsigned int number,
    bool append, bool binary, struct put_table_writer *writer)
{
    struct table_state *state = class_data;
    struct table_block *block = &state->blocks[number];
    return start_table_write(block, append, binary, writer);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Attributes. */

static error__t table_length_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct table_state *state = class_data;
    struct table_block *block = &state->blocks[number];
    return WITH_LOCKR(block->read_lock,
        format_string(result, length, "%zu", block->length));
}


static error__t table_max_length_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct table_state *state = class_data;
    return format_string(result, length, "%zu", state->max_length);
}


static error__t table_b_get_many(
    void *owner, void *class_data, unsigned int number,
    struct connection_result *result)
{
    struct table_state *state = class_data;
    struct table_block *block = &state->blocks[number];
    return WITH_LOCKR(block->read_lock,
        write_base_64(block, result));
}


static error__t table_fields_get_many(
    void *owner, void *class_data, unsigned int number,
    struct connection_result *result)
{
    struct table_state *state = class_data;
    return field_set_fields_get_many(&state->field_set, result);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Published interface. */

const struct class_methods table_class_methods = {
    "table",
    .init = table_init,
    .parse_register = table_parse_register,
    .destroy = table_destroy,
    .get_many = table_get_many,
    .put_table = table_put_table,
    .change_set = table_change_set,
    .change_set_index = CHANGE_IX_TABLE,
    .attrs = (struct attr_methods[]) {
        { "LENGTH", "Number of entries in table",
          .format = table_length_format, },
        { "MAX_LENGTH", "Maximum number of entries in table",
          .format = table_max_length_format, },
        { "FIELDS", "List of sub-fields for this table",
          .get_many = table_fields_get_many, },
        { "B", "Table in base-64 representation",
          .get_many = table_b_get_many, },
    },
    .attr_count = 4,
};
