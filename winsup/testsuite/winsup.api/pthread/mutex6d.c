/* 
 * mutex6d.c
 *
 * Tests PTHREAD_MUTEX_DEFAULT mutex type.
 * The thread should behave the same way than an errorchecking mutex.
 *
 * Depends on API functions: 
 *      pthread_create()
 *      pthread_mutexattr_init()
 *      pthread_mutexattr_settype()
 *      pthread_mutexattr_gettype()
 *      pthread_mutex_init()
 *	pthread_mutex_lock()
 *	pthread_mutex_unlock()
 */

#include "test.h"

static int lockCount = 0;

static pthread_mutex_t mutex;
static pthread_mutexattr_t mxAttr;

void * locker(void * arg)
{
  assert(pthread_mutex_lock(&mutex) == 0);
  lockCount++;
  assert(pthread_mutex_lock(&mutex) == EDEADLK);
  lockCount++;
  assert(pthread_mutex_unlock(&mutex) == 0);
  assert(pthread_mutex_unlock(&mutex) == EPERM);
 
  return (void *) 555;
}
 
int
main()
{
  pthread_t t;
  int result = 0;
  int mxType = -1;

  assert(pthread_mutexattr_init(&mxAttr) == 0);
  assert(pthread_mutexattr_settype(&mxAttr, PTHREAD_MUTEX_DEFAULT) == 0);
  assert(pthread_mutexattr_gettype(&mxAttr, &mxType) == 0);
  assert(mxType == PTHREAD_MUTEX_DEFAULT);

  assert(pthread_mutex_init(&mutex, &mxAttr) == 0);

  assert(pthread_create(&t, NULL, locker, NULL) == 0);
 
  assert(pthread_join(t, (void **) &result) == 0);
  assert(result == 555);

  assert(lockCount == 2);

  assert(pthread_mutex_destroy(&mutex) == 0);
  assert(pthread_mutexattr_destroy(&mxAttr) == 0);

  exit(0);

  /* Never reached */
  return 0;
}
