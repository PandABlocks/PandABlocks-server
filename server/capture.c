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
#include "data_server.h"
#include "classes.h"
#include "output.h"
#include "locking.h"
#include "hardware.h"

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


error__t arm_capture(void)
{
    return WITH_CAPTURE_STATE(state,
        TEST_OK_(state == CAPTURE_IDLE, "Capture in progress")  ?:
        DO(
            write_capture_masks();
            hw_write_arm(true);
            start_data_capture();
            capture_state = CAPTURE_ACTIVE;
        ));
}


error__t disarm_capture(void)
{
    return WITH_CAPTURE_STATE(state,
        TEST_OK_(state == CAPTURE_ACTIVE, "Capture not in progress")  ?:
        DO(hw_write_arm(false)));
}


error__t reset_capture(void)
{
    LOCK(capture_mutex);
    hw_write_arm(false);
    UNLOCK(capture_mutex);
    reset_data_capture();
    return ERROR_OK;
}


void data_capture_complete(void)
{
    LOCK(capture_mutex);
    ASSERT_OK(capture_state == CAPTURE_ACTIVE);
    capture_state = CAPTURE_CLOSING;
    UNLOCK(capture_mutex);
}


void data_clients_complete(void)
{
    LOCK(capture_mutex);
    ASSERT_OK(capture_state == CAPTURE_CLOSING);
    capture_state = CAPTURE_IDLE;
    UNLOCK(capture_mutex);
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
    unsigned int reading, waiting;
    get_data_capture_counts(&reading, &waiting);
    return write_one_result(result, "%u %u", reading, waiting);
}
