/* Implementation of pthread timeout helper functions. */

#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>

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


void compute_deadline(
    const struct timespec *timeout, struct timespec *deadline)
{
    ASSERT_IO(clock_gettime(CLOCK_MONOTONIC, deadline));

    deadline->tv_sec  += timeout->tv_sec;
    deadline->tv_nsec += timeout->tv_nsec;
    if (deadline->tv_nsec >= NSECS)
    {
        deadline->tv_nsec -= NSECS;
        deadline->tv_sec += 1;
    }
}


bool pwait_deadline(
    pthread_mutex_t *mutex, pthread_cond_t *signal,
    const struct timespec *deadline)
{
    int rc = pthread_cond_timedwait(signal, mutex, deadline);
    if (rc == ETIMEDOUT)
        return false;
    else
    {
        ASSERT_PTHREAD(rc);
        return true;
    }
}


bool pwait_timeout(
    pthread_mutex_t *mutex, pthread_cond_t *signal,
    const struct timespec *timeout)
{
    struct timespec deadline;
    compute_deadline(timeout, &deadline);
    return pwait_deadline(mutex, signal, &deadline);
}
