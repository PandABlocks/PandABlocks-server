/* Helper functions for lock support. */

/* Basic mutex lock/unlock. */
#define LOCK(mutex)     ASSERT_PTHREAD(pthread_mutex_lock(&(mutex)))
#define UNLOCK(mutex)   ASSERT_PTHREAD(pthread_mutex_unlock(&(mutex)))

/* Read and write locks. */
#define LOCKR(mutex)    ASSERT_PTHREAD(pthread_rwlock_rdlock(&(mutex)))
#define LOCKW(mutex)    ASSERT_PTHREAD(pthread_rwlock_wrlock(&(mutex)))
#define UNLOCKRW(mutex) ASSERT_PTHREAD(pthread_rwlock_unlock(&(mutex)))
