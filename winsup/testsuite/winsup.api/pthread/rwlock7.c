/*
 * rwlock7.c
 *
 * Hammer on a bunch of rwlocks to test robustness and fairness.
 * Printed stats should be roughly even for each thread.
 */

#include "test.h"
#include <sys/time.h>
#include <sys/timeb.h>

#ifdef __GNUC__
#include <stdlib.h>
#endif

#define THREADS         5
#define DATASIZE        15
#define ITERATIONS      1000000

#define rand_r( _seed ) \
	( _seed == _seed? rand() : rand() )

/*
 * Keep statistics for each thread.
 */
typedef struct thread_tag {
  int         thread_num;
  pthread_t   thread_id;
  int         updates;
  int         reads;
  int         interval;
} thread_t;

/*
 * Read-write lock and shared data
 */
typedef struct data_tag {
  pthread_rwlock_t    lock;
  int                 data;
  int                 updates;
} data_t;

static thread_t threads[THREADS];
static data_t data[DATASIZE];

/*
 * Thread start routine that uses read-write locks
 */
void *thread_routine (void *arg)
{
  thread_t *self = (thread_t*)arg;
  int repeats = 0;
  int iteration;
  int element = 0;

  for (iteration = 0; iteration < ITERATIONS; iteration++)
    {
      if (iteration % (ITERATIONS / 10) == 0)
        {
          putchar('.');
          fflush(stdout);
        }
      /*
       * Each "self->interval" iterations, perform an
       * update operation (write lock instead of read
       * lock).
       */
      if ((iteration % self->interval) == 0)
        {
          assert(pthread_rwlock_wrlock (&data[element].lock) == 0);
          data[element].data = self->thread_num;
          data[element].updates++;
          self->updates++;
          assert(pthread_rwlock_unlock (&data[element].lock) == 0);
        } else {
          /*
           * Look at the current data element to see whether
           * the current thread last updated it. Count the
           * times, to report later.
           */
          assert(pthread_rwlock_rdlock (&data[element].lock) == 0);

          self->reads++;

          if (data[element].data == self->thread_num)
            {
              repeats++;
            }

          assert(pthread_rwlock_unlock (&data[element].lock) == 0);
        }

      element++;

      if (element >= DATASIZE)
        {
          element = 0;
        }
    }

  if (repeats > 0)
    {
      printf ("\nThread %d found unchanged elements %d times",
              self->thread_num, repeats);
      fflush(stdout);
    }

  return NULL;
}

int
main (int argc, char *argv[])
{
  int count;
  int data_count;
  int thread_updates = 0;
  int data_updates = 0;
  int seed = 1;

  struct timeb currSysTime1;
  struct timeb currSysTime2;

  /*
   * Initialize the shared data.
   */
  for (data_count = 0; data_count < DATASIZE; data_count++)
    {
      data[data_count].data = 0;
      data[data_count].updates = 0;

      assert(pthread_rwlock_init (&data[data_count].lock, NULL) == 0);
    }

  ftime(&currSysTime1);

  /*
   * Create THREADS threads to access shared data.
   */
  for (count = 0; count < THREADS; count++)
    {
      threads[count].thread_num = count;
      threads[count].updates = 0;
      threads[count].reads = 0;
      threads[count].interval = rand_r (&seed) % 71;

      assert(pthread_create (&threads[count].thread_id,
                             NULL, thread_routine, (void*)&threads[count]) == 0);
    }

  /*
   * Wait for all threads to complete, and collect
   * statistics.
   */
  for (count = 0; count < THREADS; count++)
    {
      assert(pthread_join (threads[count].thread_id, NULL) == 0);
      thread_updates += threads[count].updates;
      printf ("%02d: interval %d, updates %d, reads %d\n",
              count, threads[count].interval,
              threads[count].updates, threads[count].reads);
    }

  putchar('\n');
  fflush(stdout);

  /*
   * Collect statistics for the data.
   */
  for (data_count = 0; data_count < DATASIZE; data_count++)
    {
      data_updates += data[data_count].updates;
      printf ("data %02d: value %d, %d updates\n",
              data_count, data[data_count].data, data[data_count].updates);
      assert(pthread_rwlock_destroy (&data[data_count].lock) == 0);
    }

  printf ("%d thread updates, %d data updates\n",
          thread_updates, data_updates);

  ftime(&currSysTime2);

  printf( "\nstart: %ld/%d, stop: %ld/%d, duration:%ld\n",
          currSysTime1.time,currSysTime1.millitm,
          currSysTime2.time,currSysTime2.millitm,
          (currSysTime2.time*1000+currSysTime2.millitm) -
          (currSysTime1.time*1000+currSysTime1.millitm));

  return 0;
}
