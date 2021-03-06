/* thread.h: Locking and threading module definitions

   Copyright 1998, 1999, 2000, 2001, 2002, 2003 Red Hat, Inc.

   Written by Marco Fuykschot <marco@ddi.nl>
   Major update 2001 Robert Collins <rbtcollins@hotmail.com>

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#ifndef _CYGNUS_THREADS_
#define _CYGNUS_THREADS_

#define LOCK_FD_LIST     1
#define LOCK_MEMORY_LIST 2
#define LOCK_MMAP_LIST   3
#define LOCK_DLL_LIST    4

#define WRITE_LOCK 1
#define READ_LOCK  2

extern "C"
{
#if defined (_CYG_THREAD_FAILSAFE) && defined (_MT_SAFE)
  void AssertResourceOwner (int, int);
#else
#define AssertResourceOwner(i,ii)
#endif
}

#ifndef _MT_SAFE

#define SetResourceLock(i,n,c)
#define ReleaseResourceLock(i,n,c)

#else

#include <pthread.h>
#include <limits.h>
#include <errno.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>
#define _NOMNTENT_FUNCS
#include <mntent.h>

extern "C"
{

struct _winsup_t
{
  /*
     Needed for the group functions
   */
  struct __group16 _grp;
  char *_namearray[2];
  int _grp_pos;

  /* console.cc */
  unsigned _rarg;

  /* dlfcn.cc */
  int _dl_error;
  char _dl_buffer[256];

  /* passwd.cc */
  struct passwd _res;
  char _pass[_PASSWORD_LEN];
  int _pw_pos;

  /* path.cc */
  struct mntent mntbuf;
  int _iteration;
  DWORD available_drives;
  char mnt_type[80];
  char mnt_opts[80];
  char mnt_fsname[MAX_PATH];
  char mnt_dir[MAX_PATH];

  /* strerror */
  char _strerror_buf[20];

  /* sysloc.cc */
  char *_process_ident;
  int _process_logopt;
  int _process_facility;
  int _process_logmask;

  /* times.cc */
  char timezone_buf[20];
  struct tm _localtime_buf;

  /* uinfo.cc */
  char _username[UNLEN + 1];

  /* net.cc */
  char *_ntoa_buf;
  struct protoent *_protoent_buf;
  struct servent *_servent_buf;
  struct hostent *_hostent_buf;
};


struct __reent_t
{
  struct _reent *_clib;
  struct _winsup_t *_winsup;
};

_winsup_t *_reent_winsup ();
void SetResourceLock (int, int, const char *) __attribute__ ((regparm (3)));
void ReleaseResourceLock (int, int, const char *)
  __attribute__ ((regparm (3)));

#ifdef _CYG_THREAD_FAILSAFE
void AssertResourceOwner (int, int);
#else
#define AssertResourceOwner(i,ii)
#endif
}

class native_mutex
{
public:
  bool init ();
  bool lock ();
  void unlock ();
private:
  HANDLE theHandle;
};

class per_process;
class pinfo;

class ResourceLocks
{
public:
  ResourceLocks ()
  {
  }
  LPCRITICAL_SECTION Lock (int);
  void Init ();
  void Delete ();
#ifdef _CYG_THREAD_FAILSAFE
  DWORD owner;
  DWORD count;
#endif
private:
  CRITICAL_SECTION lock;
  bool inited;
};

#define PTHREAD_MAGIC 0xdf0df045
#define PTHREAD_MUTEX_MAGIC PTHREAD_MAGIC+1
#define PTHREAD_KEY_MAGIC PTHREAD_MAGIC+2
#define PTHREAD_ATTR_MAGIC PTHREAD_MAGIC+3
#define PTHREAD_MUTEXATTR_MAGIC PTHREAD_MAGIC+4
#define PTHREAD_COND_MAGIC PTHREAD_MAGIC+5
#define PTHREAD_CONDATTR_MAGIC PTHREAD_MAGIC+6
#define SEM_MAGIC PTHREAD_MAGIC+7
#define PTHREAD_ONCE_MAGIC PTHREAD_MAGIC+8
#define PTHREAD_RWLOCK_MAGIC PTHREAD_MAGIC+9
#define PTHREAD_RWLOCKATTR_MAGIC PTHREAD_MAGIC+10

#define MUTEX_OWNER_ANONYMOUS        ((pthread_t) -1)

/* verifyable_object should not be defined here - it's a general purpose class */

class verifyable_object
{
public:
  long magic;

  verifyable_object (long);
  virtual ~verifyable_object ();
};

typedef enum
{
  VALID_OBJECT,
  INVALID_OBJECT,
  VALID_STATIC_OBJECT
} verifyable_object_state;

verifyable_object_state verifyable_object_isvalid (void const *, long);
verifyable_object_state verifyable_object_isvalid (void const *, long, void *);

template <class list_node> class List {
public:
  List() : head(NULL)
  {
  }

  void insert (list_node *node)
  {
    if (!node)
      return;
    node->next = (list_node *) InterlockedExchangePointer (&head, node);
  }

  list_node *remove ( list_node *node)
  {
    if (!node || !head)
      return NULL;
    if (node == head)
      return pop ();

    list_node *result_prev = head;
    while (result_prev && result_prev->next && !(node == result_prev->next))
      result_prev = result_prev->next;
    if (result_prev)
      return (list_node *)InterlockedExchangePointer (&result_prev->next, result_prev->next->next);
    return NULL;
  }

  list_node *pop ()
  {
    return (list_node *) InterlockedExchangePointer (&head, head->next);
  }

  /* poor mans generic programming. */
  void for_each (void (list_node::*callback) ())
  {
    list_node *node = head;
    while (node)
      {
        (node->*callback) ();
        node = node->next;
      }
  }

protected:
  list_node *head;
};

class pthread_key:public verifyable_object
{
public:
  static bool is_good_object (pthread_key_t const *);
  DWORD tls_index;

  int set (const void *);
  void *get () const;

  pthread_key (void (*)(void *));
  ~pthread_key ();
  static void fixup_before_fork()
  {
    keys.for_each (&pthread_key::save_key_to_buffer);
  }

  static void fixup_after_fork()
  {
    keys.for_each (&pthread_key::recreate_key_from_buffer);
  }

  static void run_all_destructors ()
  {
    keys.for_each (&pthread_key::run_destructor);
  }

  /* List support calls */
  class pthread_key *next;
private:
  static List<pthread_key> keys;
  void save_key_to_buffer ();
  void recreate_key_from_buffer ();
  void (*destructor) (void *);
  void run_destructor ();
  void *fork_buf;
};

class pthread_attr:public verifyable_object
{
public:
  static bool is_good_object(pthread_attr_t const *);
  int joinable;
  int contentionscope;
  int inheritsched;
  struct sched_param schedparam;
  size_t stacksize;

  pthread_attr ();
  ~pthread_attr ();
};

class pthread_mutexattr:public verifyable_object
{
public:
  static bool is_good_object(pthread_mutexattr_t const *);
  int pshared;
  int mutextype;
  pthread_mutexattr ();
  ~pthread_mutexattr ();
};

class pthread_mutex:public verifyable_object
{
public:
  static bool is_good_object (pthread_mutex_t const *);
  static bool is_good_initializer (pthread_mutex_t const *);
  static bool is_good_initializer_or_object (pthread_mutex_t const *);
  static bool is_good_initializer_or_bad_object (pthread_mutex_t const *mutex);
  static bool can_be_unlocked (pthread_mutex_t const *mutex);
  static void init_mutex ();
  static int init (pthread_mutex_t *, const pthread_mutexattr_t *);

  unsigned long lock_counter;
  HANDLE win32_obj_id;
  unsigned int recursion_counter;
  LONG condwaits;
  pthread_t owner;
  int type;
  int pshared;

  pthread_t get_pthread_self () const
  {
    return PTHREAD_MUTEX_NORMAL == type ? MUTEX_OWNER_ANONYMOUS :
      ::pthread_self ();
  }

  int lock ()
  {
    return _lock (get_pthread_self ());
  }
  int trylock ()
  {
    return _trylock (get_pthread_self ());
  }
  int unlock ()
  {
    return _unlock (get_pthread_self ());
  }
  int destroy ()
  {
    return _destroy (get_pthread_self ());
  }

  void set_owner (pthread_t self)
  {
    recursion_counter = 1;
    owner = self;
  }

  int lock_recursive ()
  {
    if (UINT_MAX == recursion_counter)
      return EAGAIN;
    ++recursion_counter;
    return 0;
  }

  pthread_mutex (pthread_mutexattr * = NULL);
  pthread_mutex (pthread_mutex_t *, pthread_mutexattr *);
  ~pthread_mutex ();

  class pthread_mutex * next;
  static void fixup_after_fork ()
  {
    mutexes.for_each (&pthread_mutex::_fixup_after_fork);
  }

private:
  int _lock (pthread_t self);
  int _trylock (pthread_t self);
  int _unlock (pthread_t self);
  int _destroy (pthread_t self);

  void _fixup_after_fork ();

  static List<pthread_mutex> mutexes;
  static native_mutex mutex_initialization_lock;
};

#define WAIT_CANCELED   (WAIT_OBJECT_0 + 1)

class pthread:public verifyable_object
{
public:
  HANDLE win32_obj_id;
  class pthread_attr attr;
  void *(*function) (void *);
  void *arg;
  void *return_ptr;
  bool running;
  bool suspended;
  int cancelstate, canceltype;
  HANDLE cancel_event;
  pthread_t joiner;

  /* signal handling */
  struct sigaction *sigs;
  sigset_t *sigmask;
  LONG *sigtodo;
  virtual void create (void *(*)(void *), pthread_attr *, void *);

  pthread ();
  virtual ~pthread ();

  static void init_mainthread ();
  static bool is_good_object(pthread_t const *);
  static void atforkprepare();
  static void atforkparent();
  static void atforkchild();

  /* API calls */
  static int cancel (pthread_t);
  static int join (pthread_t * thread, void **return_val);
  static int detach (pthread_t * thread);
  static int create (pthread_t * thread, const pthread_attr_t * attr,
			      void *(*start_routine) (void *), void *arg);
  static int once (pthread_once_t *, void (*)(void));
  static int atfork(void (*)(void), void (*)(void), void (*)(void));
  static int suspend (pthread_t * thread);
  static int resume (pthread_t * thread);

  virtual void exit (void *value_ptr);

  virtual int cancel ();

  virtual void testcancel ();
  static void static_cancel_self ();

  static DWORD cancelable_wait (HANDLE object, DWORD timeout, const bool do_cancel = true);

  virtual int setcancelstate (int state, int *oldstate);
  virtual int setcanceltype (int type, int *oldtype);

  virtual void push_cleanup_handler (__pthread_cleanup_handler *handler);
  virtual void pop_cleanup_handler (int const execute);

  static pthread* self ();
  static void *thread_init_wrapper (void *);

  virtual unsigned long getsequence_np();

  static int equal (pthread_t t1, pthread_t t2)
  {
    return t1 == t2;
  }

  /* List support calls */
  class pthread *next;
  static void fixup_after_fork ()
  {
    threads.for_each (&pthread::_fixup_after_fork);
  }

private:
  static List<pthread> threads;
  DWORD thread_id;
  __pthread_cleanup_handler *cleanup_stack;
  pthread_mutex mutex;

  void _fixup_after_fork ();

  void pop_all_cleanup_handlers (void);
  void precreate (pthread_attr *);
  void postcreate ();
  void set_thread_id_to_current ();
  static void set_tls_self_pointer (pthread *);
  static pthread *get_tls_self_pointer ();
  void cancel_self ();
  DWORD get_thread_id ();
  void init_current_thread ();
};

class pthread_null : public pthread
{
  public:
  static pthread *get_null_pthread();
  ~pthread_null();

  /* From pthread These should never get called
  * as the ojbect is not verifyable
  */
  void create (void *(*)(void *), pthread_attr *, void *);
  void exit (void *value_ptr);
  int cancel ();
  void testcancel ();
  int setcancelstate (int state, int *oldstate);
  int setcanceltype (int type, int *oldtype);
  void push_cleanup_handler (__pthread_cleanup_handler *handler);
  void pop_cleanup_handler (int const execute);
  unsigned long getsequence_np();

  private:
  pthread_null ();
  static pthread_null _instance;
};

class pthread_condattr:public verifyable_object
{
public:
  static bool is_good_object(pthread_condattr_t const *);
  int shared;

  pthread_condattr ();
  ~pthread_condattr ();
};

class pthread_cond:public verifyable_object
{
public:
  static bool is_good_object (pthread_cond_t const *);
  static bool is_good_initializer (pthread_cond_t const *);
  static bool is_good_initializer_or_object (pthread_cond_t const *);
  static bool is_good_initializer_or_bad_object (pthread_cond_t const *);
  static void init_mutex ();
  static int init (pthread_cond_t *, const pthread_condattr_t *);

  int shared;

  unsigned long waiting;
  unsigned long pending;
  HANDLE sem_wait;

  pthread_mutex mtx_in;
  pthread_mutex mtx_out;

  pthread_mutex_t mtx_cond;

  void unblock (const bool all);
  int wait (pthread_mutex_t mutex, DWORD dwMilliseconds = INFINITE);

  pthread_cond (pthread_condattr *);
  ~pthread_cond ();

  class pthread_cond * next;
  static void fixup_after_fork ()
  {
    conds.for_each (&pthread_cond::_fixup_after_fork);
  }

private:
  void _fixup_after_fork ();

  static List<pthread_cond> conds;
  static native_mutex cond_initialization_lock;
};

class pthread_rwlockattr:public verifyable_object
{
public:
  static bool is_good_object(pthread_rwlockattr_t const *);
  int shared;

  pthread_rwlockattr ();
  ~pthread_rwlockattr ();
};

class pthread_rwlock:public verifyable_object
{
public:
  static bool is_good_object (pthread_rwlock_t const *);
  static bool is_good_initializer (pthread_rwlock_t const *);
  static bool is_good_initializer_or_object (pthread_rwlock_t const *);
  static bool is_good_initializer_or_bad_object (pthread_rwlock_t const *);
  static void init_mutex ();
  static int init (pthread_rwlock_t *, const pthread_rwlockattr_t *);

  int shared;

  unsigned long waiting_readers;
  unsigned long waiting_writers;
  pthread_t writer;
  struct RWLOCK_READER
  {
    struct RWLOCK_READER *next;
    pthread_t thread;
  } *readers;

  int rdlock ();
  int tryrdlock ();

  int wrlock ();
  int trywrlock ();

  int unlock ();

  pthread_mutex mtx;
  pthread_cond cond_readers;
  pthread_cond cond_writers;

  pthread_rwlock (pthread_rwlockattr *);
  ~pthread_rwlock ();

  class pthread_rwlock * next;
  static void fixup_after_fork ()
  {
    rwlocks.for_each (&pthread_rwlock::_fixup_after_fork);
  }

private:
  static List<pthread_rwlock> rwlocks;

  void add_reader (struct RWLOCK_READER *rd);
  void remove_reader (struct RWLOCK_READER *rd);
  struct RWLOCK_READER *lookup_reader (pthread_t thread);

  void release ()
  {
    if (waiting_writers)
      {
        if (!readers)
          cond_writers.unblock (false);
      }
    else if (waiting_readers)
      cond_readers.unblock (true);
  }


  static void rdlock_cleanup (void *arg);
  static void wrlock_cleanup (void *arg);

  void _fixup_after_fork ();

  static native_mutex rwlock_initialization_lock;
};

class pthread_once
{
public:
  pthread_mutex_t mutex;
  int state;
};

/* shouldn't be here */
class semaphore:public verifyable_object
{
public:
  static bool is_good_object(sem_t const *);
  /* API calls */
  static int init (sem_t * sem, int pshared, unsigned int value);
  static int destroy (sem_t * sem);
  static int wait (sem_t * sem);
  static int trywait (sem_t * sem);
  static int post (sem_t * sem);

  HANDLE win32_obj_id;
  int shared;
  long currentvalue;

  semaphore (int, unsigned int);
  ~semaphore ();

  class semaphore * next;
  static void fixup_after_fork ()
  {
    semaphores.for_each (&semaphore::_fixup_after_fork);
  }

private:
  void _wait ();
  void _post ();
  int _trywait ();

  void _fixup_after_fork ();

  static List<semaphore> semaphores;
};

class callback
{
public:
  void (*cb)(void);
  class callback * next;
};

class MTinterface
{
public:
  // General
  int concurrency;
  long int threadcount;

  // Used for main thread data, and sigproc thread
  struct __reent_t reents;
  struct _winsup_t winsup_reent;

  callback *pthread_prepare;
  callback *pthread_child;
  callback *pthread_parent;

  pthread_key reent_key;
  pthread_key thread_self_key;

  void Init ();
  void fixup_before_fork (void);
  void fixup_after_fork (void);

  MTinterface () :
    concurrency (0), threadcount (1),
    pthread_prepare (NULL), pthread_child (NULL), pthread_parent (NULL),
    reent_key (NULL), thread_self_key (NULL)
  {
  }
};

#define MT_INTERFACE user_data->threadinterface

#endif // MT_SAFE

#endif // _CYGNUS_THREADS_
