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

/* Compute and return result under specified lock. */
#define _WITH_LOCK_UNLOCK(lock, unlock, mutex, result) \
    ( { \
        lock(mutex); \
        DO_FINALLY(result, unlock(mutex)); \
    } )
#define WITH_LOCK(args...)      _WITH_LOCK_UNLOCK(LOCK, UNLOCK, args)
#define WITH_LOCKR(args...)     _WITH_LOCK_UNLOCK(LOCKR, UNLOCKRW, args)
#define WITH_LOCKW(args...)     _WITH_LOCK_UNLOCK(LOCKW, UNLOCKRW, args)


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
    const struct timespec *timeout);

/* As for pwait_timeout, but in this case the timeout is an absolute time. */
bool pwait_deadline(
    pthread_mutex_t *mutex, pthread_cond_t *signal,
    const struct timespec *deadline);

/* Computes deadline from timeout. */
void compute_deadline(
    const struct timespec *timeout, struct timespec *deadline);
