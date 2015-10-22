/* Interface for configuration commands. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "error.h"

#include "config_command.h"


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static bool process_entity_put(
    struct config_connection *connection,
    const char *name, const char *value, command_error_t *error)
{
    printf("process_entity_put %s %s\n", name, value);
    *error = "put not implemented";
    return true;
}


static void process_entity_get(
    struct config_connection *connection,
    const char *name, char result[], command_error_t *error, void **multiline)
{
    printf("process_entity_get %s\n", name);
    *multiline = NULL;
    *error = "get not implemented";
}


static bool process_entity_get_more(
    struct config_connection *connection, void *multiline, char result[])
{
    ASSERT_FAIL();
}


const struct config_command_set entity_commands = {
    .put = process_entity_put,
    .get = process_entity_get,
    .get_more = process_entity_get_more,
};


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* System commands. */

static bool process_system_put(
    struct config_connection *connection,
    const char *name, const char *value, command_error_t *error)
{
    printf("process_system_put %s %s\n", name, value);
    *error = "*put not implemented";
    return true;
}


static void process_system_get(
    struct config_connection *connection,
    const char *name, char result[], command_error_t *error, void **multiline)
{
    printf("process_system_get %s\n", name);
    *multiline = NULL;
    *error = "*get not implemented";
}


static bool process_system_get_more(
    struct config_connection *connection, void *multiline, char result[])
{
    ASSERT_FAIL();
}


const struct config_command_set system_commands = {
    .put = process_system_put,
    .get = process_system_get,
    .get_more = process_system_get_more,
};
