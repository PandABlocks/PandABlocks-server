/* Implementation of pthread timeout helper functions. */

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>

#include "error.h"
#include "locking.h"


/* Initialise the condition attribute to use the monotonic clock.  This means we
 * shouldn't have problems if the real time clock starts dancing about. */
void pwait_initialise(pthread_cond_t *signal)
{
    pthread_condattr_t attr;
    ASSERT_PTHREAD(pthread_condattr_init(&attr));
    ASSERT_PTHREAD(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC));
    ASSERT_PTHREAD(pthread_cond_init(signal, &attr));
}


bool pwait_timeout(
    pthread_mutex_t *mutex, pthread_cond_t *signal,
    unsigned int secs, unsigned long nsecs)
{
    struct timespec timeout;
    ASSERT_IO(clock_gettime(CLOCK_MONOTONIC, &timeout));

    timeout.tv_sec += (int) secs;
    timeout.tv_nsec += (long) nsecs;
    if (timeout.tv_nsec >= NSECS)
    {
        timeout.tv_nsec -= NSECS;
        timeout.tv_sec += 1;
    }

    int rc = pthread_cond_timedwait(signal, mutex, &timeout);
    if (rc == ETIMEDOUT)
        return false;
    else
    {
        ASSERT_PTHREAD(rc);
        return true;
    }
}
