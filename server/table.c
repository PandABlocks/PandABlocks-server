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
/* Table blocks. */

/* The table block implementation is shared between short and long tables. */


struct table_block {
    uint64_t update_index;  // Timestamp of last change
    unsigned int number;    // Index of this block

    uint32_t *data;         // Data area for block
    size_t length;          // Current length of block

    /* Writes to the table are double buffered.  We allocate a dedicated write
     * buffer for use during write, this is protected by write_lock.  When
     * updating the block we need to take the read_lock as well for writing. */
    uint32_t *write_data;   // Transient data area while writing
    size_t write_length;    // Length written so far

    pthread_mutex_t write_lock; // Locks access to write_data area
    pthread_rwlock_t read_lock; // Write access taken when updating length&data
};


static void initialise_table_blocks(
    struct table_block blocks[], unsigned int block_count)
{
    for (unsigned int i = 0; i < block_count; i ++)
    {
        blocks[i] = (struct table_block) {
            .number = i,
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
    void *data = block->data;
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


static error__t write_to_table_block(
    struct table_block *block, size_t max_length,
    const uint32_t data[], size_t length)
{
    if (block->write_length + length > max_length)
        return FAIL_("Too much data written to table");
    else
    {
        memcpy(block->write_data + block->write_length, data,
            length * sizeof(uint32_t));
        block->write_length += length;
        return ERROR_OK;
    }
}


static void lock_table_read(struct table_block *block)
{
    ASSERT_PTHREAD(pthread_rwlock_rdlock(&block->read_lock));
}

static void unlock_table_read(struct table_block *block)
{
    ASSERT_PTHREAD(pthread_rwlock_unlock(&block->read_lock));
}

/* Writing can be rejected if there is another write to the same table in
 * progress simultaneously. */
static error__t start_table_write(struct table_block *block, bool append)
{
    int result = pthread_mutex_trylock(&block->write_lock);
    if (result == EBUSY)
        return FAIL_("Table currently being written");
    else
    {
        if (!append)
            block->write_length = 0;
        return TEST_PTHREAD(result);
    }
}

static void complete_table_write(
    struct table_block *block, void (*finalise)(struct table_block *block))
{
    ASSERT_PTHREAD(pthread_rwlock_wrlock(&block->read_lock));

    memcpy(block->data, block->write_data,
        sizeof(uint32_t) * block->write_length);
    block->length = block->write_length;
    block->update_index = get_change_index();

    if (finalise)
        finalise(block);

    ASSERT_PTHREAD(pthread_rwlock_unlock(&block->read_lock));
    ASSERT_PTHREAD(pthread_mutex_unlock(&block->write_lock));
}


/* A helper macro for locked reads. */
#define _id_LOCKED_READ(error, block, result) \
    ( { \
        lock_table_read(block); \
        error__t error = (result); \
        unlock_table_read(block); \
        error; \
    } )
#define LOCKED_READ(args...) _id_LOCKED_READ(UNIQUE_ID(), args)


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Table methods. */

struct table_state {
    unsigned int block_count;   // Number of block instances
    struct field_set field_set; // Set of fields in table
    size_t max_length;          // Maximum block length

    enum table_type { SHORT_TABLE, LONG_TABLE } table_type;
    union {
        struct short_table_state {
            unsigned int block_base;    // Block base address
            unsigned int init_reg;      // Register for starting block write
            unsigned int fill_reg;      // Register for completing block write
        } short_state;
        struct long_table_state {
            unsigned int table_order;   // Configured size as a power of 2
            struct hw_long_table *table;
        } long_state;
    };

    /* This part contains the block specific information.  To help the table
     * writing code, the max_length field is repeated for each block! */
    struct table_block blocks[];
};


static error__t table_init(
    const char **line, unsigned int block_count,
    struct hash_table *attr_map, void **class_data)
{
    enum table_type table_type;
    error__t error =
        parse_whitespace(line)  ?:
        IF_ELSE(read_string(line, "short"),
            DO(table_type = SHORT_TABLE),
        //else
        IF_ELSE(read_string(line, "long"),
            DO(table_type = LONG_TABLE),
        //else
            FAIL_("Table type not recognised")));

    if (!error)
    {
        struct table_state *state = malloc(
            sizeof(struct table_state) +
            block_count * sizeof(struct table_block));
        *state = (struct table_state) {
            .block_count = block_count,
            .table_type = table_type,
        };
        initialise_table_blocks(state->blocks, block_count);

        *class_data = state;
    }
    return error;
}


static void table_destroy(void *class_data)
{
    struct table_state *state = class_data;

    field_set_destroy(&state->field_set);
    for (unsigned int i = 0; i < state->block_count; i ++)
        free(state->blocks[i].write_data);

    switch (state->table_type)
    {
        case SHORT_TABLE:
            for (unsigned int i = 0; i < state->block_count; i ++)
                free(state->blocks[i].data);
            break;
        case LONG_TABLE:
            if (state->long_state.table)
                hw_close_long_table(state->long_state.table);
            break;
    }
}


static error__t table_parse_attribute(void *class_data, const char **line)
{
    struct table_state *state = class_data;
    return field_set_parse_attribute(&state->field_set, line);
}


static error__t short_table_parse_register(
    struct table_state *state, const char **line)
{
    unsigned int max_length;
    return
        parse_uint(line, &max_length)  ?:
        DO(state->max_length = (size_t) max_length)  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &state->short_state.init_reg)  ?:
        parse_whitespace(line)  ?:
        parse_uint(line, &state->short_state.fill_reg);
}


static error__t long_table_parse_register(
    struct table_state *state, const char **line)
{
    return
        parse_whitespace(line)  ?:
        parse_char(line, '2')  ?:  parse_char(line, '^')  ?:    // 2^order
        parse_uint(line, &state->long_state.table_order);
}


static error__t table_parse_register(
    void *class_data, struct field *field, const char **line)
{
    struct table_state *state = class_data;
    switch (state->table_type)
    {
        case SHORT_TABLE:
            return short_table_parse_register(state, line);
        case LONG_TABLE:
            return long_table_parse_register(state, line);
        default:
            ASSERT_FAIL();
    }
}


static error__t short_table_finalise(
    struct table_state *state, unsigned int block_base)
{
    state->short_state.block_base = block_base;

    size_t block_size = state->max_length * sizeof(uint32_t);
    for (unsigned int i = 0; i < state->block_count; i ++)
    {
        state->blocks[i].data = malloc(block_size);
        state->blocks[i].write_data = malloc(block_size);
    }
    return ERROR_OK;
}


static error__t long_table_finalise(
    struct table_state *state, unsigned int block_base)
{
    error__t error = hw_open_long_table(
        block_base, state->block_count, state->long_state.table_order,
        &state->long_state.table, &state->max_length);

    if (!error)
    {
        size_t block_size = state->max_length * sizeof(uint32_t);
        for (unsigned int i = 0; i < state->block_count; i ++)
        {
            hw_read_long_table_area(
                state->long_state.table, i, &state->blocks[i].data);
            state->blocks[i].write_data = malloc(block_size);
        }
    }
    return error;
}


static error__t table_finalise(void *class_data, unsigned int block_base)
{
    struct table_state *state = class_data;
    switch (state->table_type)
    {
        case SHORT_TABLE:
            return short_table_finalise(state, block_base);
        case LONG_TABLE:
            return long_table_finalise(state, block_base);
        default:
            ASSERT_FAIL();
    }
}


static error__t table_get(
    void *class_data, unsigned int number,
    struct connection_result *result)
{
    struct table_state *state = class_data;
    struct table_block *block = &state->blocks[number];
    return LOCKED_READ(block, write_ascii(block, result));
}


static void table_change_set(
    void *class_data, const uint64_t report_index, bool changes[])
{
    struct table_state *state = class_data;
    for (unsigned int i = 0; i < state->block_count; i ++)
    {
        struct table_block *block = &state->blocks[i];
        lock_table_read(block);
        changes[i] = block->update_index >= report_index;
        unlock_table_read(block);
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Table writing. */

/* Methods for writing data into the block. */
static error__t table_put_table_write(
    void *context, const uint32_t data[], size_t length)
{
    struct table_block *block = context;
    struct table_state *state =
        container_of(block, struct table_state, blocks[block->number]);
    return write_to_table_block(block, state->max_length, data, length);
}


static void short_table_put_table_close(void *context)
{
    struct table_block *block = context;
    struct table_state *state =
        container_of(block, struct table_state, blocks[block->number]);

    /* Pad rest of table with zeros and send to hardware. */
    memset(block->write_data + block->write_length, 0,
        sizeof(uint32_t) * (state->max_length - block->write_length));
    hw_write_short_table(
        state->short_state.block_base, block->number,
        state->short_state.init_reg, state->short_state.fill_reg,
        block->write_data, state->max_length);

    complete_table_write(block, NULL);
}


/* This is called under both locks to bring the hardware (which is observing the
 * read area) up to date. */
static void long_table_put_finalise(struct table_block *block)
{
    struct table_state *state =
        container_of(block, struct table_state, blocks[block->number]);
    hw_write_long_table_length(
        state->long_state.table, block->number, block->length);
}

static void long_table_put_table_close(void *context)
{
    struct table_block *block = context;
    complete_table_write(block, long_table_put_finalise);
}


static error__t table_put_table(
    void *class_data, unsigned int number,
    bool append, struct put_table_writer *writer)
{
    struct table_state *state = class_data;
    struct table_block *block = &state->blocks[number];
    switch (state->table_type)
    {
        case SHORT_TABLE:
            *writer = (struct put_table_writer) {
                .context = block,
                .write = table_put_table_write,
                .close = short_table_put_table_close,
            };
            break;
        case LONG_TABLE:
            *writer = (struct put_table_writer) {
                .context = block,
                .write = table_put_table_write,
                .close = long_table_put_table_close,
            };
            break;
    }
    return start_table_write(block, append);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Attributes. */

static error__t table_length_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct table_state *state = class_data;
    struct table_block *block = &state->blocks[number];
    return LOCKED_READ(block,
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
    return LOCKED_READ(block, write_base_64(block, result));
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
    .finalise = table_finalise,
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
