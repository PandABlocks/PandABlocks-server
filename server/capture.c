/* Data capture control. */

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>

#include "error.h"
#include "parse.h"
#include "config_server.h"
#include "classes.h"
#include "locking.h"

#include "capture.h"


// static pthread_mutex_t capture_mutex;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Capture state */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Capture control methods. */

static error__t arm_capture(void)
{
    return FAIL_("Not implemented");
}


static error__t disarm_capture(void)
{
    return FAIL_("Not implemented");
}


static error__t reset_capture(void)
{
    return FAIL_("Not implemented");
}


static error__t capture_status(struct connection_result *result)
{
    return FAIL_("Not implemented");
}


static error__t capture_waiting(struct connection_result *result)
{
    return FAIL_("Not implemented");
}


static error__t capture_downloading(struct connection_result *result)
{
    return FAIL_("Not implemented");
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Software class. */


struct software_methods {
    const char *name;
    error__t (*read)(struct connection_result *result);
    error__t (*write)(void);
};


static const struct software_methods actions[] = {
    { "arm",        .write = arm_capture, },
    { "disarm",     .write = disarm_capture, },
    { "reset",      .write = reset_capture, },
    { "status",     .read = capture_status, },
    { "waiting",    .read = capture_waiting, },
    { "downloading", .read = capture_downloading, },
};


static error__t lookup_action(
    const char *name, const struct software_methods **methods)
{
    for (unsigned int i = 0; i < ARRAY_SIZE(actions); i ++)
        if (strcmp(name, actions[i].name) == 0)
        {
            *methods = &actions[i];
            return ERROR_OK;
        }
    return FAIL_("Unknown software class");
}


static error__t software_init(
    const char **line, unsigned int count,
    struct hash_table *attr_map, void **class_data)
{
    char action[MAX_NAME_LENGTH];
    return
        parse_whitespace(line)  ?:
        parse_name(line, action, sizeof(action))  ?:
        lookup_action(action, (const struct software_methods **) class_data);
}


/* For software methods the class data is unfreeable. */
static void software_destroy(void *class_data) { }


static error__t software_get(
    void *class_data, unsigned int number,
    struct connection_result *result)
{
    const struct software_methods *methods = class_data;
    return
        TEST_OK_(methods->read, "Field not readable")  ?:
        methods->read(result);
}


static error__t software_put(
    void *class_data, unsigned int number, const char *value)
{
    const struct software_methods *methods = class_data;
    return
        TEST_OK_(methods->write, "Field not writeable")  ?:
        parse_eos(&value)  ?:
        methods->write();
}


const struct class_methods software_class_methods = {
    "software",
    .init = software_init,
    .destroy = software_destroy,
    .get = software_get,
    .put = software_put,
};
