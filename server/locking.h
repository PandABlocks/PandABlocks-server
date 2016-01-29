/* Helper functions for lock support. */

/* Basic mutex lock/unlock. */
#define LOCK(mutex)     ASSERT_PTHREAD(pthread_mutex_lock(&(mutex)))
#define UNLOCK(mutex)   ASSERT_PTHREAD(pthread_mutex_unlock(&(mutex)))

/* Condition waiting and signalling. */
#define WAIT(mutex, signal) \
    ASSERT_PTHREAD(pthread_cond_wait(&(signal), &(mutex)))
#define SIGNAL(signal)  ASSERT_PTHREAD(pthread_cond_signal(&(signal)))
#define BROADCAST(signal) \
    ASSERT_PTHREAD(pthread_cond_broadcast(&(signal)))

/* Read and write locks. */
#define LOCKR(mutex)    ASSERT_PTHREAD(pthread_rwlock_rdlock(&(mutex)))
#define LOCKW(mutex)    ASSERT_PTHREAD(pthread_rwlock_wrlock(&(mutex)))
#define UNLOCKRW(mutex) ASSERT_PTHREAD(pthread_rwlock_unlock(&(mutex)))

/* Compute and return result under mutex. */
#define WITH_LOCK(mutex, result) \
    ( { \
        LOCK(mutex); \
        DO_FINALLY(result, UNLOCK(mutex)); \
    } )

/* Comput and return result under read lock. */
#define WITH_LOCKR(mutex, result) \
    ( { \
        LOCKR(mutex); \
        DO_FINALLY(result, UNLOCKRW(mutex)); \
    } )


/* A handy macro for timeouts in ns. */
#define NSECS   1000000000      // 1e9

/* pthread condition timed wait.  To be completely safe against clock
 * misbehaviour we also have to initialise the condition specially. */
void pwait_initialise(pthread_cond_t *signal);

/* Performs timed wait on given (mutex,signal) pair.  Must be called as usual
 * with the mutex held, returns true if the signal was received, false if a
 * timeout intervened. */
bool pwait_timeout(
    pthread_mutex_t *mutex, pthread_cond_t *signal,
    unsigned int secs, unsigned long nsecs);
