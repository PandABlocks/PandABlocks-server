/* Table access implementation. */

#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdarg.h>

#include "error.h"
#include "classes.h"
#include "attributes.h"

#include "table.h"


static error__t table_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data)
{
    return ERROR_OK;
}

static error__t table_parse_register(
    void *class_data, const char *block_name, const char *field_name,
    const char **line)
{
    return ERROR_OK;
}

static error__t table_get(
    void *class_data, unsigned int ix,
    struct connection_result *result)
{
    return FAIL_("Not implemented");
}

static error__t table_put_table(
    void *class_data, unsigned int ix,
    bool append, struct put_table_writer *writer)
{
    return FAIL_("block.field< not implemented yet");
}

const struct class_methods short_table_class_methods = {
    "table",
    .init = table_init,
    .parse_register = table_parse_register,
    .get = table_get,
    .put_table = table_put_table,
    .attrs = (struct attr_methods[]) {
        { "LENGTH", },
        { "B", },
        { "FIELDS", },
    },
    .attr_count = 3,
};

const struct class_methods long_table_class_methods = {
    "short_table",
    .init = table_init,
    .parse_register = table_parse_register,
    .get = table_get,
    .put_table = table_put_table,
    .attrs = (struct attr_methods[]) {
        { "LENGTH", },
        { "B", },
        { "FIELDS", },
    },
    .attr_count = 3,
};

