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


#define BASE64_LINE_BYTES   57U     // Converts to 76 characters


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


/* Converts binary data to base 64.  The output buffer must be at least
 * ceiling(4/3*length)+1 bytes long. */
static void to_base_64(const void *data, size_t length, char out[])
{
    const unsigned char *data_in = data;
    static const char convert[64] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (; length >= 3; length -= 3)
    {
        unsigned char a = *data_in++;
        unsigned char b = *data_in++;
        unsigned char c = *data_in++;

        *out++ = convert[a >> 2];
        *out++ = convert[((a << 4) | (b >> 4)) & 0x3F];
        *out++ = convert[((b << 2) | (c >> 6)) & 0x3F];
        *out++ = convert[c & 0x3F];
    }
    switch (length)
    {
        case 2:
        {
            unsigned char a = *data_in++;
            unsigned char b = *data_in++;
            *out++ = convert[a >> 2];
            *out++ = convert[((a << 4) | (b >> 4)) & 0x3F];
            *out++ = convert[(b << 2) & 0x3F];
            *out++ = '=';
            break;
        }
        case 1:
        {
            unsigned char a = *data_in++;
            *out++ = convert[a >> 2];
            *out++ = convert[(a << 4) & 0x3F];
            *out++ = '=';
            *out++ = '=';
            break;
        }
    }
    *out++ = '\0';
}


static error__t write_base_64(
    const void *data, size_t length, struct connection_result *result)
{
    while (length > 0)
    {
        size_t to_write = MIN(length, BASE64_LINE_BYTES);
        char line[MAX_RESULT_LENGTH];
        to_base_64(data, to_write, line);
        result->write_many(result->write_context, line);
        length -= to_write;
        data += to_write;
    }
    result->response = RESPONSE_MANY;
    return ERROR_OK;
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
        TEST_OK_(short_table_lock_unlock(block, true),
            "Write to table in progress")  ?:
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


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Long table. */

struct long_table_state {
    unsigned int block_count;   // Number of block instances
    struct field_set field_set; // Set of fields in table
    struct long_table_block {
        unsigned int number;    // Index of this block
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
    (void) state;
    return ERROR_OK;
}


static error__t long_table_finalise(void *class_data, unsigned int block_base)
{
    struct long_table_state *state = class_data;
    (void) state;
    return ERROR_OK;
}


static error__t long_table_get(
    void *class_data, unsigned int number,
    struct connection_result *result)
{
    struct long_table_state *state = class_data;
    (void) state;
    return FAIL_("Not implemented");
}

static error__t long_table_put_table_write(
    void *context, const unsigned int data[], size_t length)
{
    return FAIL_("Not implemented");
}

static void long_table_put_table_close(void *context)
{
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
    return FAIL_("block.field< not implemented yet");
}


static error__t long_table_length_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct long_table_state *state = class_data;
    (void) state;
    return FAIL_("Not implemented");
}


static error__t long_table_max_length_format(
    void *owner, void *class_data, unsigned int number,
    char result[], size_t length)
{
    struct long_table_state *state = class_data;
    (void) state;
    return FAIL_("Not implemented");
}


static error__t long_table_fields_get_many(
    void *owner, void *class_data, unsigned int number,
    struct connection_result *result)
{
    struct long_table_state *state = class_data;
    return field_set_fields_get_many(&state->field_set, result);
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
    .attrs = (struct attr_methods[]) {
        { "LENGTH", .format = long_table_length_format, },
        { "MAX_LENGTH", .format = long_table_max_length_format, },
        { "FIELDS", .get_many = long_table_fields_get_many, },
//         { "B",      .get_many = long_table_b_get_many, },
    },
    .attr_count = 3,
};
