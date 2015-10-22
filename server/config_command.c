/* Interface for configuration commands. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "error.h"

#include "config_command.h"


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static bool process_entity_put(
    const char *name, const char *value, FILE *sock, command_error_t *error)
{
    printf("process_entity_put %s %s\n", name, value);
    *error = "put not implemented";
    return true;
}


static command_error_t process_entity_get(
    const char *name, char result[], size_t result_length, void **multiline)
{
    printf("process_entity_get %s %p %zu\n", name, result, result_length);
    *multiline = NULL;
    return "get not implemented";
}


static bool process_entity_get_more(
    void *multiline, char result[], size_t result_length)
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

static command_error_t process_system_put(const char *name, const char *value)
{
    printf("process_system_put %s %s\n", name, value);
    return "*put not implemented";
}


static command_error_t process_system_get(
    const char *name, char result[], size_t result_length, void **multiline)
{
    printf("process_system_get %s %p %zu\n", name, result, result_length);
    *multiline = NULL;
    return "*get not implemented";
}


static bool process_system_get_more(
    void *multiline, char result[], size_t result_length)
{
    ASSERT_FAIL();
}


static bool wrap_process_system_put(
    const char *name, const char *value, FILE *sock, command_error_t *error)
{
    *error = process_system_put(name, value);
    return true;
}


const struct config_command_set system_commands = {
    .put = wrap_process_system_put,
    .get = process_system_get,
    .get_more = process_system_get_more,
};
