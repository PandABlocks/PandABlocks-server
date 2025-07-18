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
#include "attributes.h"
#include "fields.h"
#include "enums.h"
#include "base64.h"
#include "locking.h"
#include "hashtable.h"

#include "table.h"


/* To interoperate nicely with persistence, this needs to be a multiple of 12 so
 * that the persistence layer can read lines back a line at a time -- which
 * needs each line to be a multiple of 4 bytes. */
#define BASE64_LINE_BYTES   48U     // Converts to 64 characters

#define INITIAL_CAPACITY    4


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Field sets. */

struct table_subfield {
    unsigned int left;
    unsigned int right;
    char *field_name;
    char *subtype;
    struct enumeration *enums;
    char *description;
};

/* This is used to implement a list of fields loaded from the config register
 * and reported to the client on demand.  We maintain the list of fields in
 * order so that they can be reported in order. */
struct field_set {
    struct hash_table *fields;
    unsigned int field_count;
    unsigned int capacity;
    struct table_subfield **ordered_fields;
    unsigned int row_words;
    bool *used_bits;
};


static void field_set_destroy(struct field_set *set)
{
    hash_table_destroy(set->fields);

    for (unsigned int i = 0; i < set->field_count; i ++)
    {
        struct table_subfield *field = set->ordered_fields[i];
        free(field->field_name);
        free(field->subtype);
        if (field->enums)
            destroy_enumeration(field->enums);
        free(field->description);
        free(field);
    }
    free(set->ordered_fields);
    free(set->used_bits);
}


static error__t check_field_range(
    struct field_set *set, unsigned int left, unsigned int right)
{
    unsigned int row_bits = 32 * set->row_words;
    error__t error =
        TEST_OK_(left >= right, "Invalid ordering of bit field")  ?:
        TEST_OK_(left < row_bits, "Bit field extends outside row");
    for (unsigned int i = right; !error  &&  i <= left; i ++)
    {
        error = TEST_OK_(!set->used_bits[i],
            "Bit field overlaps at bit %u", i);
        set->used_bits[i] = true;
    }
    return error;
}


const struct enumeration *get_table_subfield_enumeration(
    struct table_subfield *subfield)
{
    return subfield->enums;
}


const char *get_table_subfield_description(struct table_subfield *subfield)
{
    return subfield->description;
}


static error__t add_new_field(
    struct field_set *set,
    unsigned int left, unsigned int right, const char *field_name,
    const char *subtype, struct indent_parser *parser)
{
    /* Create the new field. */
    struct table_subfield *field = malloc(sizeof(struct table_subfield));
    *field = (struct table_subfield) {
        .left = left,
        .right = right,
        .field_name = strdup(field_name),
        .subtype = strdup(subtype),
    };

    /* Ensure we have enough capacity and add the field to the ordered list. */
    if (set->field_count >= set->capacity)
    {
        if (set->capacity)
            set->capacity *= 2;
        else
            set->capacity = INITIAL_CAPACITY;
        set->ordered_fields = realloc(
            set->ordered_fields,
            set->capacity * sizeof(struct table_subfield *));
    }
    set->ordered_fields[set->field_count++] = field;

    /* If enums requested then create enumeration and set up to parse the enum
     * definitions which follow. */
    if (strcmp(subtype, "enum") == 0)
    {
        field->enums = create_dynamic_enumeration();
        set_enumeration_parser(field->enums, parser);
    }

    /* Insert the hash table. */
    return TEST_OK_(
        !hash_table_insert(set->fields, field->field_name, field),
        "Duplicate table field");
}


static error__t validate_subtype(const char *subtype)
{
    /* We only support a limited set of subtype names here. */
    static const char *valid_subtypes[] = { "uint", "int", "enum" };
    for (unsigned int i = 0; i < ARRAY_SIZE(valid_subtypes); i ++)
        if (strcmp(subtype, valid_subtypes[i]) == 0)
            return ERROR_OK;
    return FAIL_("Invalid table field subtype");
}


static error__t field_set_parse_attribute(
    void *context, const char **line, struct indent_parser *parser)
{
    struct field_set *set = context;
    unsigned int left;
    unsigned int right;
    char field_name[MAX_NAME_LENGTH];
    char subtype[MAX_NAME_LENGTH] = "uint";
    return
        parse_uint(line, &left)  ?:
        parse_char(line, ':')  ?:
        parse_uint(line, &right)  ?:
        parse_whitespace(line)  ?:
        parse_alphanum_name(line, field_name, sizeof(field_name))  ?:
        IF(**line == ' ',
            parse_whitespace(line)  ?:
            parse_name(line, subtype, sizeof(subtype))  ?:
            validate_subtype(subtype))  ?:

        check_field_range(set, left, right)  ?:
        add_new_field(set, left, right, field_name, subtype, parser);
}


static error__t field_set_fields_get_many(
    struct field_set *set, struct connection_result *result)
{
    for (unsigned int i = 0; i < set->field_count; i ++)
    {
        struct table_subfield *field = set->ordered_fields[i];
        format_many_result(result,
            "%u:%u %s %s", field->left, field->right, field->field_name,
            field->subtype);
    }
    return ERROR_OK;
}


/* Parses a single line of description for a sub field. */
static error__t table_parse_description(
    void *context, const char **line, struct indent_parser *parser)
{
    struct field_set *set = context;
    char field_name[MAX_NAME_LENGTH];
    struct table_subfield *field;
    const char *description;
    return
        parse_alphanum_name(line, field_name, sizeof(field_name))  ?:
        TEST_OK_(field = hash_table_lookup(set->fields, field_name),
            "Sub-field not in table")  ?:
        TEST_OK_(!field->description, "Field already described")  ?:
        parse_whitespace(line)  ?:
        parse_utf8_string(line, &description)  ?:
        DO(field->description = strdup(description));
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
    bool streaming_mode;        // Used to indicate we sent a streaming table
    bool last_table;            // Used to indicate we sent the last table
    bool write_binary;          // Set if data is being written in binary
    bool reset_required;        // Set if table needs to be reset
    enum table_mode {
        TABLE_INIT, TABLE_FIXED, TABLE_STREAMING, TABLE_STREAMING_LAST
    } mode;

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
            .mode = TABLE_INIT,
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
        state->max_length - block->write_length;
    uint32_t *write_data = block->write_data + block->write_length;
    size_t length;
    return
        IF_ELSE(block->write_binary,
            convert_base64_line(line, write_data, max_length, &length),
        //else
            convert_ascii_line(line, write_data, max_length, &length))  ?:
        DO(block->write_length += length);
}


static error__t update_table_mode(
    struct table_block *block, bool streaming_mode, bool last_table,
    size_t write_length)
{
    struct table_state *state =
        container_of(block, struct table_state, blocks[block->number]);
    if (streaming_mode && !hw_table_supports_streaming(state->table))
        return FAIL_("Table do not support streaming mode");

    if (write_length == 0)
    {
        if (streaming_mode)
            return FAIL_("Streaming table must have data");
        block->reset_required = true;
        block->mode = TABLE_INIT;
        return ERROR_OK;
    }

    block->reset_required = false;
    switch (block->mode)
    {
        case TABLE_INIT:
            if (streaming_mode)
                block->mode =
                    last_table ? TABLE_STREAMING_LAST : TABLE_STREAMING;
            else
                block->mode = TABLE_FIXED;
            break;
        case TABLE_FIXED:
            block->reset_required = true;
            if (streaming_mode)
                block->mode = TABLE_STREAMING;
            break;
        case TABLE_STREAMING:
            if (!streaming_mode)
                return FAIL_("Table is in streaming mode");
            if (last_table)
                block->mode = TABLE_STREAMING_LAST;
            break;
        case TABLE_STREAMING_LAST:
            return FAIL_("Last streaming table was sent");
    }
    return ERROR_OK;
}


/* When a write is complete we copy the write data over to the live data.  This
 * is done under a write lock on the read/write lock.  Before releasing the
 * locks, we also do any required hardware finalisation. */
static error__t complete_table_write(void *context, bool write_ok)
{
    struct table_block *block = context;
    struct table_state *state =
        container_of(block, struct table_state, blocks[block->number]);

    error__t error = IF(write_ok,
        TEST_OK_(block->write_length % state->field_set.row_words == 0,
            "Table write is not a whole number of rows"));
    if (write_ok  &&  !error)
        WITH_MUTEX_W(block->read_lock)
        {
            error = update_table_mode(
                block, block->streaming_mode, block->last_table,
                block->write_length);
            if (block->reset_required)
                hw_reset_table(state->table, block->number);

            if (!error)
            {
                /* Write the data. */
                error = hw_write_table(
                    state->table, block->number,
                    block->write_data, block->write_length,
                    block->streaming_mode, block->last_table);
                /* A streaming table shouldn't be showed to the user */
                block->length =
                    block->mode == TABLE_FIXED ? block->write_length : 0;
                block->update_index = get_change_index();
            }
        }

    _UNLOCK(block->write_lock);
    return error;
}


/* Writing is rejected if there is another write to the same table in progress
 * simultaneously. */
static error__t start_table_write(
    struct table_block *block,
    bool streaming_mode, bool last_table, bool binary,
    struct put_table_writer *writer)
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

        block->write_binary = binary;
        block->write_length = 0;
        block->streaming_mode = streaming_mode;
        block->last_table = last_table;
        return ERROR_OK;
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Table methods. */


static struct table_state *create_table(
    unsigned int block_count, unsigned int row_words,
    struct indent_parser *parser)
{
    struct table_state *state = malloc(
        sizeof(struct table_state) +
        block_count * sizeof(struct table_block));
    *state = (struct table_state) {
        .block_count = block_count,
        .field_set = {
            .fields = hash_table_create(false),
            .row_words = row_words,
            .used_bits = calloc(32 * row_words, sizeof(bool)),
        },
    };
    initialise_table_blocks(state->blocks, block_count);

    *parser = (struct indent_parser) {
        .context = &state->field_set,
        .parse_line = field_set_parse_attribute,
    };
    return state;
}


static error__t table_init(
    const char **line, unsigned int block_count,
    struct hash_table *attr_map, void **class_data,
    struct indent_parser *parser)
{
    unsigned int row_words = 1;
    return
        IF(**line == ' ',
            parse_whitespace(line)  ?:
            parse_uint(line, &row_words)  ?:
            TEST_OK_(row_words > 0, "Invalid table row size"))  ?:
        DO(*class_data = create_table(block_count, row_words, parser));
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
    unsigned int max_nbuffers;
    return
        parse_char(line, '2')  ?:  parse_char(line, '^')  ?:    // 2^order
        parse_uint(line, &table_order)  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &max_nbuffers)  ?:
        parse_whitespace(line)  ?:
        check_parse_register(field, line, &base_reg)  ?:
        parse_whitespace(line)  ?:
        check_parse_register(field, line, &length_reg)  ?:

        hw_open_long_table(
            block_base, state->block_count, table_order, max_nbuffers,
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
        IF_ELSE(read_string(line, "short"),
            parse_whitespace(line)  ?:
            short_table_parse_register(state, field, block_base, line),
        //else
        IF_ELSE(read_string(line, "long"),
            parse_whitespace(line)  ?:
            long_table_parse_register(state, field, block_base, line),
        //else
            FAIL_("Table type not recognised")))  ?:
        DO(allocate_data_areas(state));
}


static void table_set_description_parse(
    void *class_data, struct indent_parser *parser)
{
    struct table_state *state = class_data;
    *parser = (struct indent_parser) {
        .context = &state->field_set,
        .parse_line = table_parse_description,
    };
}


static error__t table_get_many(
    void *class_data, unsigned int number,
    struct connection_result *result)
{
    struct table_state *state = class_data;
    struct table_block *block = &state->blocks[number];
    return ERROR_WITH_MUTEX_R(block->read_lock,
        write_ascii(block, result));
}


static void table_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct table_state *state = class_data;
    for (unsigned int i = 0; i < state->block_count; i ++)
    {
        struct table_block *block = &state->blocks[i];
        WITH_MUTEX_R(block->read_lock)
            changes[i] = block->update_index > report_index;
    }
}


static error__t table_put_table(
    void *class_data, unsigned int number,
    bool streaming_mode, bool last_table, bool binary,
    struct put_table_writer *writer)
{
    struct table_state *state = class_data;
    struct table_block *block = &state->blocks[number];
    return start_table_write(block, streaming_mode, last_table, binary, writer);
}


static struct table_subfield *table_get_subfield(
    void *class_data, const char *name)
{
    struct table_state *table = class_data;
    return hash_table_lookup(table->field_set.fields, name);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Attributes. */

static error__t table_length_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct table_state *state = class_data;
    struct table_block *block = &state->blocks[number];
    return ERROR_WITH_MUTEX_R(block->read_lock,
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
    return ERROR_WITH_MUTEX_R(block->read_lock,
        write_base_64(block, result));
}


static error__t table_fields_get_many(
    void *owner, void *class_data, unsigned int number,
    struct connection_result *result)
{
    struct table_state *state = class_data;
    return field_set_fields_get_many(&state->field_set, result);
}


static error__t table_row_words_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct table_state *state = class_data;
    return format_string(result, length, "%u", state->field_set.row_words);
}


static error__t table_queued_lines_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct table_state *state = class_data;
    return format_string(
        result, length, "%zu",
        hw_get_queued_words(state->table, number) / state->field_set.row_words);
}


static error__t table_mode_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct table_state *state = class_data;
    struct table_block *block = &state->blocks[number];
    return format_string(result, length, "%s",
        block->mode == TABLE_INIT ? "INIT" :
        block->mode == TABLE_FIXED ? "FIXED" :
        block->mode == TABLE_STREAMING ? "STREAMING" :
        block->mode == TABLE_STREAMING_LAST ? "STREAMING_LAST" :
        "UNKNOWN");
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Published interface. */

const struct class_methods table_class_methods = {
    "table",
    .init = table_init,
    .parse_register = table_parse_register,
    .destroy = table_destroy,
    .set_description_parse = table_set_description_parse,
    .get_many = table_get_many,
    .put_table = table_put_table,
    .get_subfield = table_get_subfield,
    .change_set = table_change_set,
    .change_set_index = CHANGE_IX_TABLE,
    .attrs = DEFINE_ATTRIBUTES(
        { "LENGTH", "Number of entries in table",
          .format = table_length_format, },
        { "MAX_LENGTH", "Maximum number of entries in table",
          .format = table_max_length_format, },
        { "FIELDS", "List of sub-fields for this table",
          .get_many = table_fields_get_many, },
        { "B", "Table in base-64 representation",
          .get_many = table_b_get_many, },
        { "ROW_WORDS", "Number of words per table row",
          .format = table_row_words_format, },
        { "QUEUED_LINES", "Number of lines scheduled",
          .format = table_queued_lines_format,
          .polled_change_set = true, },
        { "MODE", "Current table mode",
          .format = table_mode_format, },
    ),
};
