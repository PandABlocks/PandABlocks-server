/* Helper functions for lock support. */

/* Basic mutex lock/unlock. */
#define LOCK(mutex)     ASSERT_PTHREAD(pthread_mutex_lock(&(mutex)))
#define UNLOCK(mutex)   ASSERT_PTHREAD(pthread_mutex_unlock(&(mutex)))

/* Read and write locks. */
#define LOCKR(mutex)    ASSERT_PTHREAD(pthread_rwlock_rdlock(&(mutex)))
#define LOCKW(mutex)    ASSERT_PTHREAD(pthread_rwlock_wrlock(&(mutex)))
#define UNLOCKRW(mutex) ASSERT_PTHREAD(pthread_rwlock_unlock(&(mutex)))


/* Return an expression under a lock. */
#define _id_WITH_LOCK(result, lock, unlock, mutex, value) \
    ( { \
        LOCK(mutex); \
        typeof(value) result = (value); \
        UNLOCK(mutex); \
        result; \
    } )

#define WITH_LOCK(args...)  _id_WITH_LOCK(UNIQUE_ID(), LOCK, UNLOCK, args)
#define WITH_LOCKR(args...) _id_WITH_LOCK(UNIQUE_ID(), LOCKR, UNLOCKRW, args)
#define WITH_LOCKW(args...) _id_WITH_LOCK(UNIQUE_ID(), LOCKW, UNLOCKRW, args)
