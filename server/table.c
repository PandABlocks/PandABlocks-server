/* Table access implementation. */

#include <stdbool.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include "error.h"
#include "parse.h"
#include "hardware.h"
#include "classes.h"
#include "attributes.h"
#include "config_server.h"
#include "base64.h"

#include "table.h"


/* To interoperate nicely with persistence, this needs to be a multiple of 12 so
 * that the persistence layer can read lines back a line at a time -- which
 * needs each line to be a multiple of 4 bytes. */
#define BASE64_LINE_BYTES   48U     // Converts to 64 characters


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Common code. */

/* This is used to implement a list of fields loaded from the config register
 * and reported to the client on demand. */
struct field_set {
    unsigned int field_count;
    char **fields;
};


static void field_set_destroy(struct field_set *set)
{
    for (unsigned int i = 0; i < set->field_count; i ++)
        free(set->fields[i]);
    free(set->fields);
}


static error__t field_set_parse_attribute(
    struct field_set *set, const char **line)
{
    /* Alas we don't know how many fields are coming, so just realloc fields as
     * necessary. */
    set->field_count += 1;
    set->fields = realloc(set->fields, set->field_count * sizeof(char *));
    set->fields[set->field_count - 1] = strdup(*line);

    /* Consume the line we just parsed. */
    *line += strlen(*line);
    return ERROR_OK;
}


static error__t field_set_fields_get_many(
    struct field_set *set, struct connection_result *result)
{
    for (unsigned int i = 0; i < set->field_count; i ++)
        result->write_many(result->write_context, set->fields[i]);
    result->response = RESPONSE_MANY;
    return ERROR_OK;
}


static error__t write_ascii(
    const uint32_t *data, size_t length, struct connection_result *result)
{
    for (unsigned int i = 0; i < length; i ++)
    {
        char string[MAX_RESULT_LENGTH];
        snprintf(string, sizeof(string), "%u", data[i]);
        result->write_many(result->write_context, string);
    }
    result->response = RESPONSE_MANY;
    return ERROR_OK;
}


static error__t write_base_64(
    const void *data, size_t length, struct connection_result *result)
{
    while (length > 0)
    {
        size_t to_write = MIN(length, BASE64_LINE_BYTES);
        char line[MAX_RESULT_LENGTH];
        base64_encode(data, to_write, line);
        result->write_many(result->write_context, line);
        length -= to_write;
        data += to_write;
    }
    result->response = RESPONSE_MANY;
    return ERROR_OK;
}


static error__t table_put_table(
    const uint32_t write_data[], size_t write_length,
    uint32_t target_data[], size_t max_length, size_t *current_length)
{
    if (write_length > max_length - *current_length)
        return FAIL_("Too much data written to table");
    else
    {
        memcpy(
            target_data + *current_length, write_data,
            write_length * sizeof(uint32_t));
        *current_length += write_length;
        return ERROR_OK;
    }
}


static error__t lock_table(bool *lock)
{
    return TEST_OK_(__sync_bool_compare_and_swap(lock, false, true),
        "Write to table in progress");
}

static void unlock_table(bool *lock)
{
    ASSERT_OK(__sync_bool_compare_and_swap(lock, true, false));
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Short table. */

struct short_table_state {
    unsigned int block_count;   // Number of block instances
    unsigned int block_base;    // Block base address
    unsigned int init_reg;      // Register for starting block write
    unsigned int fill_reg;      // Register for completing block write
    struct field_set field_set; // Set of fields in table
    unsigned int max_length;    // Maximum block length

    /* This part contains the block specific information.  To help the table
     * writing code, the max_length field is repeated for each block! */
    struct short_table_block {
        uint64_t update_index;  // Timestamp of last change
        unsigned int number;    // Index of this block
        size_t length;          // Current table length
        uint32_t *data;         // Data current written to table
        bool lock;              // Lock for table write access
    } blocks[];
};


static error__t short_table_init(
    const char **line, unsigned int block_count,
    struct hash_table *attr_map, void **class_data)
{
    struct short_table_state *state = malloc(
        sizeof(struct short_table_state) +
        block_count * sizeof(struct short_table_block));
    *state = (struct short_table_state) {
        .block_count = block_count,
    };
    for (unsigned int i = 0; i < block_count; i ++)
        state->blocks[i] = (struct short_table_block) {
            .number = i,
        };

    *class_data = state;
    return ERROR_OK;
}


static void short_table_destroy(void *class_data)
{
    struct short_table_state *state = class_data;
    for (unsigned int i = 0; i < state->block_count; i ++)
        free(state->blocks[i].data);
    field_set_destroy(&state->field_set);
}


static error__t short_table_parse_attribute(void *class_data, const char **line)
{
    struct short_table_state *state = class_data;
    return field_set_parse_attribute(&state->field_set, line);
}


static void allocate_short_table_data(struct short_table_state *state)
{
    for (unsigned int i = 0; i < state->block_count; i ++)
        state->blocks[i].data = malloc(state->max_length * sizeof(uint32_t));
}

static error__t short_table_parse_register(
    void *class_data, const char *block_name, const char *field_name,
    const char **line)
{
    struct short_table_state *state = class_data;
    return
        parse_uint(line, &state->max_length)  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &state->init_reg)  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &state->fill_reg)  ?:
        DO(allocate_short_table_data(state));
}


static error__t short_table_finalise(void *class_data, unsigned int block_base)
{
    struct short_table_state *state = class_data;
    state->block_base = block_base;
    return ERROR_OK;
}


static error__t short_table_get(
    void *class_data, unsigned int number,
    struct connection_result *result)
{
    struct short_table_state *state = class_data;
    return write_ascii(
        state->blocks[number].data, state->blocks[number].length, result);
}


/* Methods for writing data into the block. */
static error__t short_table_put_table_write(
    void *context, const uint32_t data[], size_t length)
{
    struct short_table_block *block = context;
    struct short_table_state *state =
        container_of(block, struct short_table_state, blocks[block->number]);

    return table_put_table(
        data, length, block->data, state->max_length, &block->length);
}

static void short_table_put_table_close(void *context)
{
    struct short_table_block *block = context;
    struct short_table_state *state =
        container_of(block, struct short_table_state, blocks[block->number]);

    /* Pad rest of table with zeros and send to hardware. */
    memset(block->data + block->length, 0,
        sizeof(uint32_t) * (state->max_length - block->length));
    hw_write_short_table(
        state->block_base, block->number, state->init_reg, state->fill_reg,
        block->data, state->max_length);

    block->update_index = get_change_index();
    unlock_table(&block->lock);
}


static error__t short_table_put_table(
    void *class_data, unsigned int number,
    bool append, struct put_table_writer *writer)
{
    struct short_table_state *state = class_data;
    struct short_table_block *block = &state->blocks[number];
    *writer = (struct put_table_writer) {
        .context = block,
        .write = short_table_put_table_write,
        .close = short_table_put_table_close,
    };

    return
        lock_table(&block->lock)  ?:
        IF(!append, DO(block->length = 0));
}


static error__t short_table_length_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct short_table_state *state = class_data;
    return format_string(result, length, "%zu", state->blocks[number].length);
}


static error__t short_table_max_length_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct short_table_state *state = class_data;
    return format_string(result, length, "%u", state->max_length);
}


static error__t short_table_b_get_many(
    void *owner, void *class_data, unsigned int number,
    struct connection_result *result)
{
    struct short_table_state *state = class_data;
    struct short_table_block *block = &state->blocks[number];
    return write_base_64(block->data, block->length * sizeof(uint32_t), result);
}


static error__t short_table_fields_get_many(
    void *owner, void *class_data, unsigned int number,
    struct connection_result *result)
{
    struct short_table_state *state = class_data;
    return field_set_fields_get_many(&state->field_set, result);
}


static void short_table_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct short_table_state *state = class_data;
    for (unsigned int i = 0; i < state->block_count; i ++)
        changes[i] = state->blocks[i].update_index >= report_index;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Long table. */

struct long_table_state {
    unsigned int block_count;   // Number of block instances
    unsigned int table_order;   // Configured size as a power of 2
    size_t max_length;          // Maximum table length (in words)
    struct field_set field_set; // Set of fields in table
    struct long_table_block {
        uint64_t update_index;  // Timestamp of last change
        struct hw_long_table *table;
        unsigned int number;    // Index of this block
        size_t length;
        uint32_t *data;
        bool lock;              // Lock for table write access
    } blocks[];
};


static error__t long_table_init(
    const char **line, unsigned int block_count,
    struct hash_table *attr_map, void **class_data)
{
    struct long_table_state *state = malloc(
        sizeof(struct long_table_state) +
        block_count * sizeof(struct long_table_block));
    *state = (struct long_table_state) {
        .block_count = block_count,
    };
    for (unsigned int i = 0; i < block_count; i ++)
        state->blocks[i] = (struct long_table_block) {
            .number = i,
        };

    *class_data = state;
    return ERROR_OK;
}


static void long_table_destroy(void *class_data)
{
    struct long_table_state *state = class_data;
    field_set_destroy(&state->field_set);
    for (unsigned int i = 0; i < state->block_count; i ++)
        hw_close_long_table(state->blocks[i].table);
}


static error__t long_table_parse_attribute(void *class_data, const char **line)
{
    struct long_table_state *state = class_data;
    return field_set_parse_attribute(&state->field_set, line);
}


static error__t long_table_parse_register(
    void *class_data, const char *block_name, const char *field_name,
    const char **line)
{
    struct long_table_state *state = class_data;
    return parse_uint(line, &state->table_order);
}


static error__t long_table_finalise(void *class_data, unsigned int block_base)
{
    struct long_table_state *state = class_data;
    error__t error = ERROR_OK;
    for (unsigned int i = 0; !error  &&  i < state->block_count; i ++)
        error = hw_open_long_table(
            block_base, i, state->table_order,
            &state->blocks[i].table, &state->blocks[i].data,
            &state->max_length);        // umm.
    return error;
}


static error__t long_table_get(
    void *class_data, unsigned int number,
    struct connection_result *result)
{
    struct long_table_state *state = class_data;
    return write_ascii(
        state->blocks[number].data, state->blocks[number].length, result);
}


static error__t long_table_put_table_write(
    void *context, const uint32_t data[], size_t length)
{
    struct long_table_block *block = context;
    struct long_table_state *state =
        container_of(block, struct long_table_state, blocks[block->number]);
    return table_put_table(
        data, length, block->data, state->max_length, &block->length);
}

static void long_table_put_table_close(void *context)
{
    struct long_table_block *block = context;
    hw_write_long_table_length(block->table, block->length);
    block->update_index = get_change_index();
    unlock_table(&block->lock);
}

static error__t long_table_put_table(
    void *class_data, unsigned int number,
    bool append, struct put_table_writer *writer)
{
    struct long_table_state *state = class_data;
    struct long_table_block *block = &state->blocks[number];
    *writer = (struct put_table_writer) {
        .context = block,
        .write = long_table_put_table_write,
        .close = long_table_put_table_close,
    };
    return
        lock_table(&block->lock)  ?:
        IF(!append, DO(block->length = 0));
}


static error__t long_table_length_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct long_table_state *state = class_data;
    return format_string(result, length, "%zu", state->blocks[number].length);
}


static error__t long_table_max_length_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct long_table_state *state = class_data;
    return format_string(result, length, "%zu", state->max_length);
}


static error__t long_table_b_get_many(
    void *owner, void *class_data, unsigned int number,
    struct connection_result *result)
{
    struct long_table_state *state = class_data;
    struct long_table_block *block = &state->blocks[number];
    return write_base_64(block->data, block->length * sizeof(uint32_t), result);
}


static error__t long_table_fields_get_many(
    void *owner, void *class_data, unsigned int number,
    struct connection_result *result)
{
    struct long_table_state *state = class_data;
    return field_set_fields_get_many(&state->field_set, result);
}


static void long_table_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct long_table_state *state = class_data;
    for (unsigned int i = 0; i < state->block_count; i ++)
        changes[i] = state->blocks[i].update_index >= report_index;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Classes. */

const struct class_methods short_table_class_methods = {
    "short_table",
    .init = short_table_init,
    .parse_attribute = short_table_parse_attribute,
    .parse_register = short_table_parse_register,
    .finalise = short_table_finalise,
    .destroy = short_table_destroy,
    .get = short_table_get,
    .put_table = short_table_put_table,
    .change_set = short_table_change_set,
    .change_set_index = CHANGE_IX_TABLE,
    .attrs = (struct attr_methods[]) {
        { "LENGTH",     .format = short_table_length_format, },
        { "MAX_LENGTH", .format = short_table_max_length_format, },
        { "FIELDS",     .get_many = short_table_fields_get_many, },
        { "B",          .get_many = short_table_b_get_many, },
    },
    .attr_count = 4,
};

const struct class_methods long_table_class_methods = {
    "table",
    .init = long_table_init,
    .parse_attribute = long_table_parse_attribute,
    .parse_register = long_table_parse_register,
    .finalise = long_table_finalise,
    .destroy = long_table_destroy,
    .get = long_table_get,
    .put_table = long_table_put_table,
    .change_set = long_table_change_set,
    .change_set_index = CHANGE_IX_TABLE,
    .attrs = (struct attr_methods[]) {
        { "LENGTH", .format = long_table_length_format, },
        { "MAX_LENGTH", .format = long_table_max_length_format, },
        { "FIELDS", .get_many = long_table_fields_get_many, },
        { "B",      .get_many = long_table_b_get_many, },
    },
    .attr_count = 4,
};
