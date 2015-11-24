/* Table access implementation. */

#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdarg.h>

#include "error.h"
#include "classes.h"
#include "attributes.h"

#include "table.h"



static error__t table_get(
    struct class *class, unsigned int ix,
    struct connection_result *result)
{
    return FAIL_("Not implemented");
}

static error__t table_put_table(
    struct class *class, unsigned int ix,
    bool append, struct put_table_writer *writer)
{
    return FAIL_("block.field< not implemented yet");
}

const struct class_methods short_table_class_methods = {
    "table",
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
    .get = table_get,
    .put_table = table_put_table,
    .attrs = (struct attr_methods[]) {
        { "LENGTH", },
        { "B", },
        { "FIELDS", },
    },
    .attr_count = 3,
};

