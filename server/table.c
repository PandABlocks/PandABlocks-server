/* Table access implementation. */

#include <stdbool.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdarg.h>

#include "error.h"
#include "classes.h"

#include "table.h"



error__t table_get(
    struct class *class, unsigned int ix,
    struct connection_result *result)
{
    return FAIL_("Not implemented");
}

error__t table_put_table(
    struct class *class, unsigned int ix,
    bool append, struct put_table_writer *writer)
{
    return FAIL_("block.field< not implemented yet");
}
