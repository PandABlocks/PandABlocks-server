/* Helper functions for lock support. */

/* Basic mutex lock/unlock. */
#define LOCK(mutex)     ASSERT_PTHREAD(pthread_mutex_lock(&(mutex)))
#define UNLOCK(mutex)   ASSERT_PTHREAD(pthread_mutex_unlock(&(mutex)))

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
