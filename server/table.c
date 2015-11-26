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

#include "table.h"


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Short table. */

struct short_table_state {
    unsigned int block_count;   // Number of block instances
    unsigned int block_base;    // Block base address
    unsigned int init_reg;      // Register for starting block write
    unsigned int fill_reg;      // Register for completing block write
    unsigned int field_count;   // Number of fields in fields[]
    char **fields;              // Array of fields
    unsigned int max_length;    // Maximum block length

    /* This part contains the block specific information.  To help the table
     * writing code, the max_length field is repeated for each block! */
    struct short_table_block {
        unsigned int number;    // Index of this block
        size_t length;          // Current table length
        uint32_t *data;         // Data current written to table
        bool lock;              // Lock for table write access
        struct put_table_writer writer;
    } blocks[];
};


static bool short_table_lock_unlock(struct short_table_block *block, bool lock)
{
    return __sync_bool_compare_and_swap(&block->lock, !lock, lock);
}

/* Methods for writing data into the block. */
static error__t short_table_put_table_write(
    void *context, const unsigned int data[], size_t length)
{
    struct short_table_block *block = context;
    struct short_table_state *state =
        container_of(block, struct short_table_state, blocks[block->number]);

    if (length > state->max_length - block->length)
        return FAIL_("Too much data written to table");
    else
    {
        memcpy(block->data + block->length, data, length * sizeof(uint32_t));
        block->length += length;
        return ERROR_OK;
    }
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

    ASSERT_OK(short_table_lock_unlock(context, false));
}



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
            .writer = {
                .context = &state->blocks[i],
                .write = short_table_put_table_write,
                .close = short_table_put_table_close,
            },
        };

    *class_data = state;
    return ERROR_OK;
}


static void short_table_destroy(void *class_data)
{
    struct short_table_state *state = class_data;
    for (unsigned int i = 0; i < state->field_count; i ++)
        free(state->fields[i]);
    for (unsigned int i = 0; i < state->block_count; i ++)
        free(state->blocks[i].data);
    free(state->fields);
}


static error__t short_table_parse_attribute(void *class_data, const char **line)
{
    struct short_table_state *state = class_data;

    /* Alas we don't know how many fields are coming, so just realloc fields as
     * necessary. */
    state->field_count += 1;
    state->fields = realloc(state->fields, state->field_count * sizeof(char *));
    state->fields[state->field_count - 1] = strdup(*line);

    /* Consume the line we just parsed. */
    *line += strlen(*line);
    return ERROR_OK;
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
    uint32_t *data = state->blocks[number].data;
    for (unsigned int i = 0; i < state->blocks[number].length; i ++)
    {
        char string[MAX_RESULT_LENGTH];
        snprintf(string, sizeof(string), "%u", data[i]);
        result->write_many(result->write_context, string);
    }
    result->response = RESPONSE_MANY;
    return ERROR_OK;
}


static error__t short_table_put_table(
    void *class_data, unsigned int number,
    bool append, struct put_table_writer *writer)
{
    struct short_table_state *state = class_data;
    struct short_table_block *block = &state->blocks[number];
    return
        TEST_OK_(short_table_lock_unlock(block, true),
            "Write to table in progress")  ?:
        DO(*writer = block->writer)  ?:
        IF(!append, DO(block->length = 0));
}


static error__t short_table_length_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct short_table_state *state = class_data;
    return format_string(result, length, "%zu", state->blocks[number].length);
}


// static error__t short_table_b_get_many(
//     void *owner, void *class_data, unsigned int number,
//     struct connection_result *result)
// {
//     struct short_table_state *state = class_data;
//     (void) state;
//     return FAIL_("Not implemented");
// }


static error__t short_table_fields_get_many(
    void *owner, void *class_data, unsigned int number,
    struct connection_result *result)
{
    struct short_table_state *state = class_data;
    for (unsigned int i = 0; i < state->field_count; i ++)
        result->write_many(result->write_context, state->fields[i]);
    result->response = RESPONSE_MANY;
    return ERROR_OK;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Long table. */

static error__t long_table_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data)
{
    return ERROR_OK;
}

static error__t long_table_parse_register(
    void *class_data, const char *block_name, const char *field_name,
    const char **line)
{
    return ERROR_OK;
}

static error__t long_table_get(
    void *class_data, unsigned int number,
    struct connection_result *result)
{
    return FAIL_("Not implemented");
}

static error__t long_table_put_table(
    void *class_data, unsigned int number,
    bool append, struct put_table_writer *writer)
{
    return FAIL_("block.field< not implemented yet");
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
    .attrs = (struct attr_methods[]) {
        { "LENGTH", .format = short_table_length_format, },
        { "FIELDS", .get_many = short_table_fields_get_many, },
//         { "B",      .get_many = short_table_b_get_many, },
    },
    .attr_count = 2,
};

const struct class_methods long_table_class_methods = {
    "table",
    .init = long_table_init,
    .parse_register = long_table_parse_register,
    .get = long_table_get,
    .put_table = long_table_put_table,
    .attrs = (struct attr_methods[]) {
        { "LENGTH", },
        { "B", },
        { "FIELDS", },
    },
    .attr_count = 3,
};

