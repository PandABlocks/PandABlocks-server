/* Support for types. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "error.h"
#include "hashtable.h"
#include "parse.h"

#include "types.h"


struct field_type {
    const char *name;
    struct type_access access;
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Some type definitions. */

static error__t type_uint32_parse(
    const struct type_context *context, const char *string, unsigned int *value)
{
    return
        parse_uint(&string, value)  ?:
        parse_eos(&string);
}


static void type_uint32_format(
    const struct type_context *context,
    unsigned int value, char string[], size_t length)
{
    snprintf(string, length, "%u", value);
}


static const struct field_type field_type_table[] = {
    { "uint32", { .parse = type_uint32_parse, .format = type_uint32_format } },
//     { "enum",   .parse = type_enum_parse,   .format = type_enum_format },
};

static struct hash_table *field_type_map;


error__t get_type_access(
    const struct field_type *type, const struct type_access **access)
{
printf("get_type_access %p\n", type);
    return
        TEST_OK_(type, "No type registered for field")  ?:
        DO(*access = &type->access);
}


error__t lookup_type(const char *name, const struct field_type **type)
{
    return TEST_OK_(
        *type = hash_table_lookup(field_type_map, name),
        "Unknown field type %s", name);
}


error__t initialise_types(void)
{
    field_type_map = hash_table_create(false);
    for (unsigned int i = 0; i < ARRAY_SIZE(field_type_table); i ++)
    {
        const struct field_type *type = &field_type_table[i];
        hash_table_insert_const(field_type_map, type->name, type);
    }

    return ERROR_OK;
}


void terminate_types(void)
{
    if (field_type_map)
        hash_table_destroy(field_type_map);
}
