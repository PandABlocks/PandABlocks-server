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
#include "output.h"
#include "locking.h"

#include "capture.h"


static pthread_mutex_t capture_mutex;


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Capture state */

static enum capture_state capture_state = CAPTURE_IDLE;

enum capture_state lock_capture_state(void)
{
    LOCK(capture_mutex);
    return capture_state;
}


void release_capture_state(void)
{
    UNLOCK(capture_mutex);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* Capture control methods. */

error__t arm_capture(void)
{
    LOCK(capture_mutex);
    error__t error =
        TEST_OK_(capture_state == CAPTURE_IDLE, "Capture in progress");
    if (!error)
    {
        printf("arming capture\n");
        capture_state = CAPTURE_ACTIVE;
        write_capture_masks();
    }
    UNLOCK(capture_mutex);
    return error;
}


error__t disarm_capture(void)
{
    LOCK(capture_mutex);
    error__t error =
        TEST_OK_(capture_state == CAPTURE_ACTIVE, "Capture not in progress");
    if (!error)
    {
        printf("disarming capture\n");
        capture_state = CAPTURE_IDLE;
    }
    UNLOCK(capture_mutex);
    return error;
}


error__t reset_capture(void)
{
    LOCK(capture_mutex);
    printf("reset capture\n");
    capture_state = CAPTURE_IDLE;
    UNLOCK(capture_mutex);
    return ERROR_OK;
}


error__t capture_status(struct connection_result *result)
{
    const char *string;
    switch (capture_state)
    {
        case CAPTURE_IDLE:      string = "Idle"; break;
        case CAPTURE_ACTIVE:    string = "Active"; break;
        case CAPTURE_CLOSING:   string = "Closing"; break;
        default:    ASSERT_FAIL();
    }
    return write_one_result(result, "%s", string);
}


error__t capture_waiting(struct connection_result *result)
{
    return FAIL_("Not implemented");
}
