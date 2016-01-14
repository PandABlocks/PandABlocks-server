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
#include "classes.h"
#include "attributes.h"
#include "config_server.h"
#include "base64.h"
#include "locking.h"

#include "table.h"


/* To interoperate nicely with persistence, this needs to be a multiple of 12 so
 * that the persistence layer can read lines back a line at a time -- which
 * needs each line to be a multiple of 4 bytes. */
#define BASE64_LINE_BYTES   48U     // Converts to 64 characters


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Field sets. */

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
    size_t write_offset;        // Offset data will start at when completed

    pthread_mutex_t write_lock; // Locks access to write_data area
    pthread_rwlock_t read_lock; // Write access taken when updating length&data
};


struct table_state {
    unsigned int block_count;   // Number of block instances
    struct field_set field_set; // Set of fields in table
    size_t max_length;          // Maximum block length
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
    {
        char string[MAX_RESULT_LENGTH];
        snprintf(string, sizeof(string), "%u", block->data[i]);
        result->write_many(result->write_context, string);
    }
    result->response = RESPONSE_MANY;
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
    result->response = RESPONSE_MANY;
    return ERROR_OK;
}


/* Writing is rejected if there is another write to the same table in progress
 * simultaneously. */
static error__t start_table_write(struct table_block *block)
{
    int result = pthread_mutex_trylock(&block->write_lock);
    if (result == EBUSY)
        return FAIL_("Table currently being written");
    else
        return TEST_PTHREAD(result);
}


/* When a write is complete we copy the write data over to the live data.  This
 * is done under a write lock on the read/write lock.  Before releasing the
 * locks, we also do any required hardware finalisation. */
static void complete_table_write(void *context, bool write_ok, size_t length)
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
            block->write_offset, block->write_data, length);
        block->length = length + block->write_offset;

        block->update_index = get_change_index();

        UNLOCKRW(block->read_lock);
    }

    UNLOCK(block->write_lock);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Table methods. */


static error__t table_init(
    const char **line, unsigned int block_count,
    struct hash_table *attr_map, void **class_data)
{
    struct table_state *state = malloc(
        sizeof(struct table_state) +
        block_count * sizeof(struct table_block));
    *state = (struct table_state) {
        .block_count = block_count,
    };
    initialise_table_blocks(state->blocks, block_count);

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


static error__t table_parse_attribute(void *class_data, const char **line)
{
    struct table_state *state = class_data;
    return field_set_parse_attribute(&state->field_set, line);
}


static error__t short_table_parse_register(
    struct table_state *state, unsigned int block_base, const char **line)
{
    unsigned int max_length;
    unsigned int init_reg, fill_reg, length_reg;
    return
        /* Register specification is block size followed by the three control
         * registers. */
        parse_uint(line, &max_length)  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &init_reg)  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &fill_reg)  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &length_reg)  ?:

        DO(state->max_length = (size_t) max_length)  ?:
        hw_open_short_table(
            block_base, state->block_count,
            init_reg, fill_reg, length_reg, state->max_length, &state->table);
}


static error__t long_table_parse_register(
    struct table_state *state, unsigned int block_base, const char **line)
{
    unsigned int table_order;
    return
        parse_whitespace(line)  ?:
        parse_char(line, '2')  ?:  parse_char(line, '^')  ?:    // 2^order
        parse_uint(line, &table_order)  ?:

        hw_open_long_table(
            block_base, state->block_count, table_order,
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
            short_table_parse_register(state, block_base, line),
        //else
        IF_ELSE(read_string(line, "long"),
            long_table_parse_register(state, block_base, line),
        //else
            FAIL_("Table type not recognised")))  ?:
        DO(allocate_data_areas(state));
}


static error__t table_get(
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
    bool append, struct put_table_writer *writer)
{
    struct table_state *state = class_data;
    struct table_block *block = &state->blocks[number];

    /* If appending is requested adjust the data area length accordingly. */
    size_t offset = append ? block->length : 0;
    *writer = (struct put_table_writer) {
        .data = block->write_data,
        .max_length = state->max_length - offset,
        .context = block,
        .close = complete_table_write,
    };
    block->write_offset = offset;
    return start_table_write(block);
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
    .parse_attribute = table_parse_attribute,
    .parse_register = table_parse_register,
    .destroy = table_destroy,
    .get = table_get,
    .put_table = table_put_table,
    .change_set = table_change_set,
    .change_set_index = CHANGE_IX_TABLE,
    .attrs = (struct attr_methods[]) {
        { "LENGTH", .format = table_length_format, },
        { "MAX_LENGTH", .format = table_max_length_format, },
        { "FIELDS", .get_many = table_fields_get_many, },
        { "B",      .get_many = table_b_get_many, },
    },
    .attr_count = 4,
};
