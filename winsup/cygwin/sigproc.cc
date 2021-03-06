/* sigproc.cc: inter/intra signal and sub process handler

   Copyright 1997, 1998, 1999, 2000, 2001, 2002 Red Hat, Inc.

   Written by Christopher Faylor <cgf@cygnus.com>

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include <stdlib.h>
#include <time.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <sys/cygwin.h>
#include <assert.h>
#include "cygerrno.h"
#include "sync.h"
#include "sigproc.h"
#include "pinfo.h"
#include "security.h"
#include "fhandler.h"
#include "path.h"
#include "dtable.h"
#include "cygheap.h"
#include "child_info_magic.h"
#define NEED_VFORK
#include "perthread.h"
#include "shared_info.h"
#include "cygthread.h"

/*
 * Convenience defines
 */
#define WSSC		   60000 // Wait for signal completion
#define WPSP		   40000 // Wait for proc_subproc mutex
#define WSPX		   20000 // Wait for wait_sig to terminate
#define WWSP		   20000 // Wait for wait_subproc to terminate

#define TOTSIGS	(NSIG + __SIGOFFSET)

#define wake_wait_subproc() SetEvent (events[0])

#define no_signals_available() (!hwait_sig || !sig_loop_wait)

#define NZOMBIES	256

LONG local_sigtodo[TOTSIGS];
inline LONG* getlocal_sigtodo (int sig)
{
  return local_sigtodo + __SIGOFFSET + sig;
}

/*
 * Global variables
 */
const char *__sp_fn ;
int __sp_ln;

char NO_COPY myself_nowait_dummy[1] = {'0'};// Flag to sig_send that signal goes to
					//  current process but no wait is required
char NO_COPY myself_nowait_nonmain_dummy[1] = {'1'};// Flag to sig_send that signal goes to
					//  current process but no wait is required
					//  if this is not the main thread.

HANDLE NO_COPY signal_arrived;		// Event signaled when a signal has
					//  resulted in a user-specified
					//  function call
/*
 * Common variables
 */


/* How long to wait for message/signals.  Normally this is infinite.
 * On termination, however, these are set to zero as a flag to exit.
 */

#define Static static NO_COPY

Static DWORD proc_loop_wait = 1000;	// Wait for subprocesses to exit
Static DWORD sig_loop_wait = INFINITE;	// Wait for signals to arrive

Static HANDLE sigcatch_nonmain;		// The semaphore signaled when
					//  signals are available for
					//  processing from non-main thread
Static HANDLE sigcatch_main;		// Signalled when main thread sends a
					//  signal
Static HANDLE sigcatch_nosync;		// Signal wait_sig to scan sigtodo
					//  but not to bother with any
					//  synchronization
Static HANDLE sigcomplete_main;		// Event signaled when a signal has
					//  finished processing for the main
					//  thread
Static HANDLE sigcomplete_nonmain;	// Semaphore raised for non-main
					//  threads when a signal has finished
					//  processing
HANDLE NO_COPY sigCONT;			// Used to "STOP" a process
Static cygthread *hwait_sig;		// Handle of wait_sig thread
Static cygthread *hwait_subproc;	// Handle of sig_subproc thread

Static HANDLE wait_sig_inited;		// Control synchronization of
					//  message queue startup

/* Used by WaitForMultipleObjects.  These are handles to child processes.
 */
Static HANDLE events[PSIZE + 1];	  // All my children's handles++
#define hchildren (events + 1)		// Where the children handles begin
Static char cpchildren[PSIZE * sizeof (pinfo)];		// All my children info
Static int nchildren;			// Number of active children
Static char czombies[(NZOMBIES + 1) * sizeof (pinfo)];		// All my deceased children info
Static int nzombies;			// Number of deceased children

#define pchildren ((pinfo *) cpchildren)
#define zombies ((pinfo *) czombies)

Static waitq waitq_head = {0, 0, 0, 0, 0, 0, 0};// Start of queue for wait'ing threads
Static waitq waitq_main;		// Storage for main thread

muto NO_COPY *sync_proc_subproc = NULL;	// Control access to subproc stuff

DWORD NO_COPY sigtid = 0;		// ID of the signal thread

static bool NO_COPY pending_signals = false;	// true if signals pending

/* Functions
 */
static int __stdcall checkstate (waitq *) __attribute__ ((regparm (1)));
static __inline__ BOOL get_proc_lock (DWORD, DWORD);
static HANDLE __stdcall getevent (_pinfo *, const char *) __attribute__ ((regparm (2)));
static void __stdcall remove_zombie (int);
static DWORD WINAPI wait_sig (VOID *arg);
static int __stdcall stopped_or_terminated (waitq *, _pinfo *);
static DWORD WINAPI wait_subproc (VOID *);

/* Determine if the parent process is alive.
 */

BOOL __stdcall
my_parent_is_alive ()
{
  DWORD res;
  if (!myself->ppid_handle)
    {
      debug_printf ("No myself->ppid_handle");
      res = FALSE;
    }
  else
    for (int i = 0; i < 2; i++)
      switch (res = WaitForSingleObject (myself->ppid_handle, 0))
	{
	  case WAIT_OBJECT_0:
	    debug_printf ("parent dead.");
	    res = FALSE;
	    goto out;
	  case WAIT_TIMEOUT:
	    debug_printf ("parent still alive");
	    res = TRUE;
	    goto out;
	  case WAIT_FAILED:
	    DWORD werr = GetLastError ();
	    if (werr == ERROR_INVALID_HANDLE && i == 0)
	      continue;
	    system_printf ("WFSO for myself->ppid_handle(%p) failed, error %d",
			   myself->ppid_handle, werr);
	    res = FALSE;
	    goto out;
	}
out:
  return res;
}

void __stdcall
wait_for_sigthread ()
{
  sigproc_printf ("wait_sig_inited %p", wait_sig_inited);
  HANDLE hsig_inited = wait_sig_inited;
  assert (hsig_inited);
  (void) WaitForSingleObject (hsig_inited, INFINITE);
  wait_sig_inited = NULL;
  (void) ForceCloseHandle1 (hsig_inited, wait_sig_inited);
}

/* Get the sync_proc_subproc muto to control access to
 * children, zombie arrays.
 * Attempt to handle case where process is exiting as we try to grab
 * the mutex.
 */
static BOOL
get_proc_lock (DWORD what, DWORD val)
{
  Static int lastwhat = -1;
  if (!sync_proc_subproc)
    return FALSE;
  if (sync_proc_subproc->acquire (WPSP))
    {
      lastwhat = what;
      return TRUE;
    }
  if (!sync_proc_subproc)
    return FALSE;
  system_printf ("Couldn't aquire sync_proc_subproc for(%d,%d), %E, last %d",
		  what, val, lastwhat);
  return TRUE;
}

static BOOL __stdcall
proc_can_be_signalled (_pinfo *p)
{
  if (p == myself_nowait || p == myself_nowait_nonmain || p == myself)
    {
      assert (!wait_sig_inited);
      return 1;
    }

  return ISSTATE (p, PID_INITIALIZING) ||
	 (((p)->process_state & (PID_ACTIVE | PID_IN_USE)) ==
	  (PID_ACTIVE | PID_IN_USE));
}

BOOL __stdcall
pid_exists (pid_t pid)
{
  pinfo p (pid);
  return proc_exists (p);
}

/* Test to determine if a process really exists and is processing signals.
 */
BOOL __stdcall
proc_exists (_pinfo *p)
{
  return p && !(p->process_state & PID_EXITED);
}

/* Return 1 if this is one of our children, zero otherwise.
   FIXME: This really should be integrated with the rest of the proc_subproc
   testing.  Scanning these lists twice is inefficient. */
int __stdcall
mychild (int pid)
{
  for (int i = 0; i < nchildren; i++)
    if (pchildren[i]->pid == pid)
      return 1;
  for (int i = 0; i < nzombies; i++)
    if (zombies[i]->pid == pid)
      return 1;
  return 0;
}

/* Handle all subprocess requests
 */
#define vchild (*((pinfo *) val))
int __stdcall
proc_subproc (DWORD what, DWORD val)
{
  int rc = 1;
  int potential_match;
  _pinfo *child;
  int clearing;
  waitq *w;

#define wval	 ((waitq *) val)

  sigproc_printf ("args: %x, %d", what, val);

  if (!get_proc_lock (what, val))	// Serialize access to this function
    {
      system_printf ("couldn't get proc lock.  Something is wrong.");
      goto out1;
    }

  switch (what)
    {
    /* Add a new subprocess to the children arrays.
     * (usually called from the main thread)
     */
    case PROC_ADDCHILD:
      if (nchildren >= PSIZE - 1)
	{
	  rc = 0;
	  break;
	}
      pchildren[nchildren] = vchild;
      hchildren[nchildren] = vchild->hProcess;
      if (!DuplicateHandle (hMainProc, vchild->hProcess, hMainProc, &vchild->pid_handle,
			    0, 0, DUPLICATE_SAME_ACCESS))
	system_printf ("Couldn't duplicate child handle for pid %d, %E", vchild->pid);
      ProtectHandle1 (vchild->pid_handle, pid_handle);

      if (!DuplicateHandle (hMainProc, hMainProc, vchild->hProcess, &vchild->ppid_handle,
			    0, TRUE, DUPLICATE_SAME_ACCESS))
	system_printf ("Couldn't duplicate my handle<%p> for pid %d, %E", hMainProc, vchild->pid);
      vchild->ppid = myself->pid;
      vchild->uid = myself->uid;
      vchild->gid = myself->gid;
      vchild->pgid = myself->pgid;
      vchild->sid = myself->sid;
      vchild->ctty = myself->ctty;
      vchild->process_state |= PID_INITIALIZING | (myself->process_state & PID_USETTY);
      vchild->copysigs (myself);

      sigproc_printf ("added pid %d to wait list, slot %d, winpid %p, handle %p",
		  vchild->pid, nchildren, vchild->dwProcessId,
		  vchild->hProcess);
      nchildren++;

      wake_wait_subproc ();
      break;

    /* A child process had terminated.
       Possibly this is just due to an exec().  Cygwin implements an exec()
       as a "handoff" from one windows process to another.  If child->hProcess
       is different from what is recorded in hchildren, then this is an exec().
       Otherwise this is a normal child termination event.
       (called from wait_subproc thread) */
    case PROC_CHILDTERMINATED:
      if (hchildren[val] != pchildren[val]->hProcess)
	{
	  sigproc_printf ("pid %d[%d], reparented old hProcess %p, new %p",
			  pchildren[val]->pid, val, hchildren[val], pchildren[val]->hProcess);
	  HANDLE h = hchildren[val];
	  hchildren[val] = pchildren[val]->hProcess; /* Filled out by child */
	  sync_proc_subproc->release ();	// Release the lock ASAP
	  ForceCloseHandle1 (h, childhProc);
	  ProtectHandle1 (pchildren[val]->hProcess, childhProc);
	  rc = 0;
	  goto out;			// This was an exec()
	}

      sigproc_printf ("pid %d[%d] terminated, handle %p, nchildren %d, nzombies %d",
		  pchildren[val]->pid, val, hchildren[val], nchildren, nzombies);

      int thiszombie;
      thiszombie = nzombies;
      zombies[nzombies] = pchildren[val];	// Add to zombie array
      zombies[nzombies++]->process_state = PID_ZOMBIE;// Walking dead

      sigproc_printf ("zombifying [%d], pid %d, handle %p, nchildren %d",
		      val, pchildren[val]->pid, hchildren[val], nchildren);
      if ((int) val < --nchildren)
	{
	  hchildren[val] = hchildren[nchildren];
	  pchildren[val] = pchildren[nchildren];
	}

      /* See if we should care about the this terminated process.  If we've
	 filled up our table or if we're ignoring SIGCHLD, then we immediately
	 remove the process and move on. Otherwise, this process becomes a zombie
	 which must be reaped by a wait() call. */
      if (nzombies >= NZOMBIES
	  || myself->getsig (SIGCHLD).sa_handler == (void *) SIG_IGN)
	{
	  sigproc_printf ("automatically removing zombie %d", thiszombie);
	  remove_zombie (thiszombie);
	}

      /* Don't scan the wait queue yet.  Caller will send SIGCHLD to this process.
	 This will cause an eventual scan of waiters. */
      break;

    /* Handle a wait4() operation.  Allocates an event for the calling
     * thread which is signaled when the appropriate pid exits or stops.
     * (usually called from the main thread)
     */
    case PROC_WAIT:
      wval->ev = NULL;		// Don't know event flag yet

      if (wval->pid <= 0)
	child = NULL;		// Not looking for a specific pid
      else if (!mychild (wval->pid))
	goto out;		// invalid pid.  flag no such child

      wval->status = 0;		// Don't know status yet
      sigproc_printf ("wval->pid %d, wval->options %d", wval->pid, wval->options);

      /* If the first time for this thread, create a new event, otherwise
       * reset the event.
       */
      if ((wval->ev = wval->thread_ev) == NULL)
	{
	  wval->ev = wval->thread_ev = CreateEvent (&sec_none_nih, TRUE,
						    FALSE, NULL);
	  ProtectHandle (wval->ev);
	}

      ResetEvent (wval->ev);
      w = waitq_head.next;
      waitq_head.next = wval;	/* Add at the beginning. */
      wval->next = w;		/* Link in rest of the list. */
      clearing = 0;
      goto scan_wait;

    /* Clear all waiting threads.  Called from exceptions.cc prior to
     * the main thread's dispatch to a signal handler function.
     * (called from wait_sig thread)
     */
    case PROC_CLEARWAIT:
      /* Clear all "wait"ing threads. */
      if (val)
	sigproc_printf ("clear waiting threads");
      else
	sigproc_printf ("looking for processes to reap");
      clearing = val;

    scan_wait:
      /* Scan the linked list of wait()ing threads.  If a wait's parameters
       * match this pid, then activate it.
       */
      for (w = &waitq_head; w->next != NULL; w = w->next)
	{
	  if ((potential_match = checkstate (w)) > 0)
	    sigproc_printf ("released waiting thread");
	  else if (!clearing && !(w->next->options & WNOHANG) && potential_match < 0)
	    sigproc_printf ("only found non-terminated children");
	  else if (potential_match <= 0)		// nothing matched
	    {
	      sigproc_printf ("waiting thread found no children");
	      HANDLE oldw = w->next->ev;
	      w->next->pid = 0;
	      if (clearing)
		w->next->status = -1;		/* flag that a signal was received */
	      else if (!potential_match || !(w->next->options & WNOHANG))
		w->next->ev = NULL;
	      if (!SetEvent (oldw))
		system_printf ("couldn't wake up wait event %p, %E", oldw);
	      w->next = w->next->next;
	    }
	  if (w->next == NULL)
	    break;
	}

      if (!clearing)
	sigproc_printf ("finished processing terminated/stopped child");
      else
	{
	  waitq_head.next = NULL;
	  sigproc_printf ("finished clearing");
	}
      break;
  }

out:
  sync_proc_subproc->release ();	// Release the lock
out1:
  sigproc_printf ("returning %d", rc);
  return rc;
}

/* Terminate the wait_subproc thread.
 * Called on process exit.
 * Also called by spawn_guts to disassociate any subprocesses from this
 * process.  Subprocesses will then know to clean up after themselves and
 * will not become zombies.
 */
void __stdcall
proc_terminate (void)
{
  sigproc_printf ("nchildren %d, nzombies %d", nchildren, nzombies);
  /* Signal processing is assumed to be blocked in this routine. */
  if (hwait_subproc)
    {
      proc_loop_wait = 0;	// Tell wait_subproc thread to exit
      sync_proc_subproc->acquire (WPSP);
      wake_wait_subproc ();	// Wake wait_subproc loop
      hwait_subproc = NULL;

      (void) proc_subproc (PROC_CLEARWAIT, 1);

      /* Clean out zombie processes from the pid list. */
      int i;
      for (i = 0; i < nzombies; i++)
	{
	  if (zombies[i]->hProcess)
	    {
	      ForceCloseHandle1 (zombies[i]->hProcess, childhProc);
	      ForceCloseHandle1 (zombies[i]->pid_handle, pid_handle);
	    }
	  zombies[i]->ppid = 1;
	  zombies[i]->process_state = PID_EXITED;	/* CGF FIXME - still needed? */
	  zombies[i].release ();	// FIXME: this breaks older gccs for some reason
	}

      /* Disassociate my subprocesses */
      for (i = 0; i < nchildren; i++)
	{
	  if (!pchildren[i]->hProcess)
	    sigproc_printf ("%d(%d) hProcess cleared already?", pchildren[i]->pid,
			pchildren[i]->dwProcessId);
	  else
	    {
	      ForceCloseHandle1 (pchildren[i]->hProcess, childhProc);
	      sigproc_printf ("%d(%d) closed child handle", pchildren[i]->pid,
			      pchildren[i]->dwProcessId);
	      pchildren[i]->ppid = 1;
	      if (pchildren[i]->pgid == myself->pid)
		pchildren[i]->process_state |= PID_ORPHANED;
	    }
	  pchildren[i].release ();
	}
      nchildren = nzombies = 0;
      /* Just zero sync_proc_subproc as the delete below seems to cause
	 problems for older gccs. */
	sync_proc_subproc = NULL;
    }
  sigproc_printf ("leaving");
}

/* Clear pending signal from the sigtodo array
 */
void __stdcall
sig_clear (int sig)
{
  (void) InterlockedExchange (myself->getsigtodo (sig), 0L);
  (void) InterlockedExchange (getlocal_sigtodo (sig), 0L);
  return;
}

extern "C" int
sigpending (sigset_t *set)
{
  unsigned bit;
  *set = 0;
  for (int sig = 1; sig < NSIG; sig++)
    if ((*getlocal_sigtodo (sig) || *myself->getsigtodo (sig))
        && (myself->getsigmask () & (bit = SIGTOMASK (sig))))
      *set |= bit;
  return 0;
}

/* Force the wait_sig thread to wake up and scan the sigtodo array.
 */
extern "C" int __stdcall
sig_dispatch_pending ()
{
  if (!hwait_sig || GetCurrentThreadId () == sigtid)
    return 0;

  sigframe thisframe (mainthread);

  int was_pending = pending_signals;
#ifdef DEBUGGING
  sigproc_printf ("pending_signals %d", was_pending);
#endif

  if (!was_pending)
#ifdef DEBUGGING
    sigproc_printf ("no need to wake anything up");
#else
    ;
#endif
  else
    {
      (void) sig_send (myself, __SIGFLUSH);
#ifdef DEBUGGING
      sigproc_printf ("woke up wait_sig");
#endif
    }

  if (was_pending)
    thisframe.call_signal_handler ();

  return was_pending;
}

/* Message initialization.  Called from dll_crt0_1
 *
 * This routine starts the signal handling thread.  The wait_sig_inited
 * event is used to signal that the thread is ready to handle signals.
 * We don't wait for this during initialization but instead detect it
 * in sig_send to gain a little concurrency.
 */
void __stdcall
sigproc_init ()
{
  wait_sig_inited = CreateEvent (&sec_none_nih, TRUE, FALSE, NULL);
  ProtectHandle (wait_sig_inited);

  /* sync_proc_subproc is used by proc_subproc.  It serialises
   * access to the children and zombie arrays.
   */
  new_muto (sync_proc_subproc);

  /* local event signaled when main thread has been dispatched
     to a signal handler function. */
  signal_arrived = CreateEvent (&sec_none_nih, TRUE, FALSE, NULL);
  ProtectHandle (signal_arrived);

  hwait_sig = new cygthread (wait_sig, cygself, "sig");
  hwait_sig->zap_h ();

  /* Initialize waitq structure for main thread.  A waitq structure is
   * allocated for each thread that executes a wait to allow multiple threads
   * to perform waits.  Pre-allocate a waitq structure for the main thread.
   */
  waitq *w;
  if ((w = (waitq *)waitq_storage.get ()) == NULL)
    {
      w = &waitq_main;
      waitq_storage.set (w);
    }
  memset (w, 0, sizeof *w);	// Just to be safe

  myself->getsig (SIGSTOP).sa_flags = SA_RESTART | SA_NODEFER;
  sigproc_printf ("process/signal handling enabled(%x)", myself->process_state);
  return;
}

/* Called on process termination to terminate signal and process threads.
 */
void __stdcall
sigproc_terminate (void)
{
  hwait_sig = NULL;

  if (!sig_loop_wait)
    sigproc_printf ("sigproc handling not active");
  else
    {
      sigproc_printf ("entering");
      sig_loop_wait = 0;	// Tell wait_sig to exit when it is
				//  finished with anything it is doing
      ForceCloseHandle (sigcomplete_main);
      for (int i = 0; i < 20; i++)
	(void) ReleaseSemaphore (sigcomplete_nonmain, 1, NULL);
      // ForceCloseHandle (sigcomplete_nonmain);
      // ForceCloseHandle (sigcatch_main);
      // ForceCloseHandle (sigcatch_nonmain);
      // ForceCloseHandle (sigcatch_nosync);
    }
  proc_terminate ();		// Terminate process handling thread

  return;
}

/* Send a signal to another process by raising its signal semaphore.
 * If pinfo *p == NULL, send to the current process.
 * If sending to this process, wait for notification that a signal has
 * completed before returning.
 */
int __stdcall
sig_send (_pinfo *p, int sig, DWORD ebp, bool exception)
{
  int rc = 1;
  DWORD tid = GetCurrentThreadId ();
  BOOL its_me;
  HANDLE thiscatch = NULL;
  HANDLE thiscomplete = NULL;
  BOOL wait_for_completion;
  sigframe thisframe;

  if (p == myself_nowait_nonmain)
    p = (tid == mainthread.id) ? (_pinfo *) myself : myself_nowait;
  if (!(its_me = (p == NULL || p == myself || p == myself_nowait)))
    wait_for_completion = FALSE;
  else
    {
      if (no_signals_available ())
	goto out;		// Either exiting or not yet initializing
      if (wait_sig_inited)
	wait_for_sigthread ();
      wait_for_completion = p != myself_nowait;
      p = myself;
    }

  /* It is possible that the process is not yet ready to receive messages
   * or that it has exited.  Detect this.
   */
  if (!proc_can_be_signalled (p))	/* Is the process accepting messages? */
    {
      sigproc_printf ("invalid pid %d(%x), signal %d",
		  p->pid, p->process_state, sig);
      set_errno (ESRCH);
      goto out;
    }

  sigproc_printf ("pid %d, signal %d, its_me %d", p->pid, sig, its_me);

  LONG *todo;
  bool issem;
  if (its_me)
    {
      if (!wait_for_completion)
	{
	  thiscatch = sigcatch_nosync;
	  todo = myself->getsigtodo (sig);
	  issem = false;
	}
      else if (tid != mainthread.id)
	{
	  thiscatch = sigcatch_nonmain;
	  thiscomplete = sigcomplete_nonmain;
	  todo = getlocal_sigtodo (sig);
	  issem = true;
	}
      else
	{
	  thiscatch = sigcatch_main;
	  thiscomplete = sigcomplete_main;
	  thisframe.set (mainthread, ebp, exception);
	  todo = getlocal_sigtodo (sig);
	  issem = true;
	}
    }
  else if ((thiscatch = getevent (p, "sigcatch")))
    {
      todo = p->getsigtodo (sig);
      issem = false;
    }
  else
    goto out;		  // Couldn't get the semaphore.  getevent issued
			  //  an error, if appropriate.

#if WHEN_MULTI_THREAD_SIGNALS_WORK
  signal_dispatch *sd;
  sd = signal_dispatch_storage.get ();
  if (sd == NULL)
    sd = signal_dispatch_storage.create ();
#endif

  /* Increment the sigtodo array to signify which signal to assert.
   */
  (void) InterlockedIncrement (todo);

  /* Notify the process that a signal has arrived.
   */
  if (issem ? !ReleaseSemaphore (thiscatch, 1, NULL) : !SetEvent (thiscatch))
    {
      /* Couldn't signal the semaphore.  This probably means that the
       * process is exiting.
       */
      if (!its_me)
	ForceCloseHandle (thiscatch);
      else
	{
	  if (no_signals_available ())
	    sigproc_printf ("I'm going away now");
	  else if ((int) GetLastError () == -1)
	    rc = WaitForSingleObject (thiscomplete, 500);
	  else
	    system_printf ("error sending signal %d to pid %d, semaphore %p, %E",
			  sig, p->pid, thiscatch);
	}
      goto out;
    }

  /* No need to wait for signal completion unless this was a signal to
   * this process.
   *
   * If it was a signal to this process, wait for a dispatched signal.
   * Otherwise just wait for the wait_sig to signal that it has finished
   * processing the signal.
   */
  if (!wait_for_completion)
    {
      rc = WAIT_OBJECT_0;
      sigproc_printf ("Not waiting for sigcomplete.  its_me %d signal %d", its_me, sig);
      if (!its_me)
	ForceCloseHandle (thiscatch);
    }
  else
    {
      sigproc_printf ("Waiting for thiscomplete %p", thiscomplete);
      rc = WaitForSingleObject (thiscomplete, WSSC);
    }

  if (rc == WAIT_OBJECT_0)
    rc = 0;		// Successful exit
  else
    {
      /* It's an error unless sig_loop_wait == 0 (the process is exiting). */
      if (!no_signals_available ())
	system_printf ("wait for sig_complete event failed, signal %d, rc %d, %E",
		      sig, rc);
      set_errno (ENOSYS);
      rc = -1;
    }

out:
  sigproc_printf ("returning %d from sending signal %d", rc, sig);
  return rc;
}

/* Set pending signal from the sigtodo array
 */
void __stdcall
sig_set_pending (int sig)
{
  (void) InterlockedIncrement (getlocal_sigtodo (sig));
  return;
}

/* Initialize the wait_subproc thread.
 * Called from fork() or spawn() to initialize the handling of subprocesses.
 */
void __stdcall
subproc_init (void)
{
  if (hwait_subproc)
    return;

  /* A "wakeup" handle which can be toggled to make wait_subproc reexamine
   * the hchildren array.
   */
  events[0] = CreateEvent (&sec_none_nih, FALSE, FALSE, NULL);
  hwait_subproc = new cygthread (wait_subproc, NULL, "proc");
  hwait_subproc->zap_h ();
  ProtectHandle (events[0]);
  sigproc_printf ("started wait_subproc thread");
}

/* Initialize some of the memory block passed to child processes
   by fork/spawn/exec. */

void __stdcall
init_child_info (DWORD chtype, child_info *ch, pid_t pid, HANDLE subproc_ready)
{
  memset (ch, 0, sizeof *ch);
  ch->cb = chtype == PROC_FORK ? sizeof (child_info_fork) : sizeof (child_info);
  ch->intro = PROC_MAGIC_GENERIC;
  ch->magic = CHILD_INFO_MAGIC;
  ch->type = chtype;
  ch->cygpid = pid;
  ch->subproc_ready = subproc_ready;
  ch->pppid_handle = myself->ppid_handle;
  ch->fhandler_union_cb = sizeof (fhandler_union);
  ch->mount_h = cygwin_mount_h;
}

/* Check the state of all of our children to see if any are stopped or
 * terminated.
 */
static int __stdcall
checkstate (waitq *parent_w)
{
  int potential_match = 0;

  sigproc_printf ("nchildren %d, nzombies %d", nchildren, nzombies);

  /* Check already dead processes first to see if they match the criteria
   * given in w->next.
   */
  for (int i = 0; i < nzombies; i++)
    switch (stopped_or_terminated (parent_w, zombies[i]))
      {
      case -1:
	potential_match = -1;
	break;
      case 1:
	remove_zombie (i);
	potential_match = 1;
	goto out;
      }

  sigproc_printf ("checking alive children");

  /* No dead terminated children matched.  Check for stopped children. */
  for (int i = 0; i < nchildren; i++)
    switch (stopped_or_terminated (parent_w, pchildren[i]))
      {
      case -1:
	potential_match = -1;
	break;
      case 1:
	potential_match = 1;
	goto out;
      }

out:
  sigproc_printf ("returning %d", potential_match);
  return potential_match;
}

/* Get or create a process specific semaphore used in message passing.
 */
static HANDLE __stdcall
getevent (_pinfo *p, const char *str)
{
  HANDLE h;
  char sem_name[MAX_PATH];

  if (p != NULL)
    {
      if (!proc_can_be_signalled (p))
	{
	  set_errno (ESRCH);
	  return NULL;
	}
      int wait = 1000;
      /* Wait for new process to generate its semaphores. */
      sigproc_printf ("pid %d, ppid %d, wait %d, initializing %x", p->pid, p->ppid, wait,
		  ISSTATE (p, PID_INITIALIZING));
      for (int i = 0; ISSTATE (p, PID_INITIALIZING) && i < wait; i++)
	low_priority_sleep (1);
    }

  if (p == NULL)
    {
      char sa_buf[1024];

      DWORD winpid = GetCurrentProcessId ();
#if 0
      h = CreateSemaphore (sec_user_nih (sa_buf), init, max,
			   str = shared_name (sem_name, str, winpid));
#else
      h = CreateEvent (sec_user_nih (sa_buf), FALSE, FALSE,
		       str = shared_name (sem_name, str, winpid));
#endif
      p = myself;
      if (!h)
	{
	  system_printf ("can't create semaphore %s, %E", str);
	  __seterrno ();
	}
    }
  else
    {
#if 0
      h = OpenSemaphore (SEMAPHORE_ALL_ACCESS, FALSE,
			 shared_name (sem_name, str, p->dwProcessId));
#else
      h = OpenEvent (EVENT_ALL_ACCESS, FALSE,
		     shared_name (sem_name, str, p->dwProcessId));
#endif

      if (!h)
	{
	  if (GetLastError () == ERROR_FILE_NOT_FOUND && !proc_exists (p))
	    set_errno (ESRCH);	/* No such process */
	  else
	    set_errno (EPERM);	/* Couldn't access the semaphore --
				   different cygwin DLL maybe? */
	}
    }

  return h;
}

/* Remove a zombie from zombies by swapping it with the last child in the list.
 */
static void __stdcall
remove_zombie (int ci)
{
  sigproc_printf ("removing %d, pid %d, nzombies %d", ci, zombies[ci]->pid,
		  nzombies);

  if (zombies[ci])
    {
      ForceCloseHandle1 (zombies[ci]->hProcess, childhProc);
      ForceCloseHandle1 (zombies[ci]->pid_handle, pid_handle);
      zombies[ci].release ();
    }

  if (ci < --nzombies)
    zombies[ci] = zombies[nzombies];

  return;
}

/* Check status of child process vs. waitq member.
 *
 * parent_w is the pointer to the parent of the waitq member in question.
 * child is the subprocess being considered.
 *
 * Returns
 *   1 if stopped or terminated child matches parent_w->next criteria
 *  -1 if a non-stopped/terminated child matches parent_w->next criteria
 *   0 if child does not match parent_w->next criteria
 */
static int __stdcall
stopped_or_terminated (waitq *parent_w, _pinfo *child)
{
  int potential_match;
  waitq *w = parent_w->next;

  sigproc_printf ("considering pid %d", child->pid);
  if (w->pid == -1)
    potential_match = 1;
  else if (w->pid == 0)
    potential_match = child->pgid == myself->pgid;
  else if (w->pid < 0)
    potential_match = child->pgid == -w->pid;
  else
    potential_match = (w->pid == child->pid);

  if (!potential_match)
    return 0;

  BOOL terminated;

  if ((terminated = child->process_state == PID_ZOMBIE) ||
      ((w->options & WUNTRACED) && child->stopsig))
    {
      parent_w->next = w->next;	/* successful wait.  remove from wait queue */
      w->pid = child->pid;

      if (!terminated)
	{
	  sigproc_printf ("stopped child");
	  w->status = (child->stopsig << 8) | 0x7f;
	  child->stopsig = 0;
	}
      else /* Should only get here when child has been moved to the zombies array */
	{
	  DWORD status;
	  if (!GetExitCodeProcess (child->hProcess, &status))
	    status = 0xffff;
	  if (status & EXIT_SIGNAL)
	    w->status = (status >> 8) & 0xff;	/* exited due to signal */
	  else
	    w->status = (status & 0xff) << 8;	/* exited via "exit ()" */

	  add_rusage (&myself->rusage_children, &child->rusage_children);
	  add_rusage (&myself->rusage_children, &child->rusage_self);

	  if (w->rusage)
	    {
	      add_rusage ((struct rusage *) w->rusage, &child->rusage_children);
	      add_rusage ((struct rusage *) w->rusage, &child->rusage_self);
	    }
	}

      if (!SetEvent (w->ev))	/* wake up wait4 () immediately */
	system_printf ("couldn't wake up wait event %p, %E", w->ev);
      return 1;
    }

  return -potential_match;
}

static void
talktome ()
{
  winpids pids;
  for (unsigned i = 0; i < pids.npids; i++)
    if (pids[i]->hello_pid == myself->pid)
      pids[i]->commune_recv ();
}

#define RC_MAIN 0
#define RC_NONMAIN 1
#define RC_NOSYNC 2
/* Process signals by waiting for a semaphore to become signaled.
 * Then scan an in-memory array representing queued signals.
 * Executes in a separate thread.
 *
 * Signals sent from this process are sent a completion signal so
 * that returns from kill/raise do not occur until the signal has
 * has been handled, as per POSIX.
 */
static DWORD WINAPI
wait_sig (VOID *self)
{
  LONG *todos[] = {getlocal_sigtodo (0), myself->getsigtodo (0)};
  /* Initialization */
  (void) SetThreadPriority (GetCurrentThread (), WAIT_SIG_PRIORITY);

  /* sigcatch_nosync       - semaphore incremented by sig_dispatch_pending and
   *			     by foreign processes to force an examination of
   *			     the sigtodo array.
   * sigcatch_main	   - ditto for local main thread.
   * sigcatch_nonmain      - ditto for local non-main threads.
   *
   * sigcomplete_main	   - event used to signal main thread on signal
   *			     completion
   * sigcomplete_nonmain   - semaphore signaled for non-main thread on signal
   *			     completion
   */
  sigcatch_nosync = getevent (NULL, "sigcatch");
  sigcatch_nonmain = CreateSemaphore (&sec_none_nih, 0, MAXLONG, NULL);
  sigcatch_main = CreateSemaphore (&sec_none_nih, 0, MAXLONG, NULL);
  sigcomplete_nonmain = CreateSemaphore (&sec_none_nih, 0, MAXLONG, NULL);
  sigcomplete_main = CreateEvent (&sec_none_nih, FALSE, FALSE, NULL);
  sigproc_printf ("sigcatch_nonmain %p, sigcatch_main %p", sigcatch_nonmain, sigcatch_main);
  sigCONT = CreateEvent (&sec_none_nih, FALSE, FALSE, NULL);

  /* Setting dwProcessId flags that this process is now capable of receiving
   * signals.  Prior to this, dwProcessId was set to the windows pid of
   * of the original windows process which spawned us unless this was a
   * "toplevel" process.
   */
  myself->dwProcessId = GetCurrentProcessId ();
  myself->process_state |= PID_ACTIVE;
  myself->process_state &= ~PID_INITIALIZING;

  ProtectHandle (sigcatch_nosync);
  ProtectHandle (sigcatch_nonmain);
  ProtectHandle (sigcatch_main);
  ProtectHandle (sigcomplete_nonmain);
  ProtectHandle (sigcomplete_main);

  /* If we've been execed, then there is still a stub left in the previous
   * windows process waiting to see if it's started a cygwin process or not.
   * Signalling subproc_ready indicates that we are a cygwin process.
   */
  if (child_proc_info && child_proc_info->type == PROC_EXEC)
    {
      debug_printf ("subproc_ready %p", child_proc_info->subproc_ready);
      if (!SetEvent (child_proc_info->subproc_ready))
	system_printf ("SetEvent (subproc_ready) failed, %E");
      ForceCloseHandle1 (child_proc_info->subproc_ready, subproc_ready);
      /* Initialize an "indirect" pid block so that if someone looks up this
	 process via its Windows PID it will be redirected to the appropriate
	 Cygwin PID shared memory block. */
      static pinfo NO_COPY myself_identity;
      myself_identity.init (cygwin_pid (myself->dwProcessId), PID_EXECED);
    }

  SetEvent (wait_sig_inited);
  sigtid = GetCurrentThreadId ();

  HANDLE catchem[] = {sigcatch_main, sigcatch_nonmain, sigcatch_nosync};
  sigproc_printf ("Ready.  dwProcessid %d", myself->dwProcessId);
  DWORD rc = RC_NOSYNC;
  bool flush = false;
  for (;;)
    {
      DWORD i;
      if (rc == RC_MAIN || rc == RC_NONMAIN)
	i = RC_NOSYNC;
      else
	i = RC_MAIN;
      rc = WaitForSingleObject (catchem[i], 0);
      if (rc != WAIT_OBJECT_0)
	rc = WaitForMultipleObjects (3, catchem, FALSE, sig_loop_wait);
      else
	rc = i + WAIT_OBJECT_0;
      (void) SetThreadPriority (GetCurrentThread (), WAIT_SIG_PRIORITY);

      /* sigproc_terminate sets sig_loop_wait to zero to indicate that
         this thread should terminate.  */
      if (rc == WAIT_TIMEOUT)
	{
	  if (!sig_loop_wait)
	    break;			// Exiting
	  else
	    continue;
	}

      if (rc == WAIT_FAILED)
	{
	  if (sig_loop_wait != 0)
	    system_printf ("WFMO failed, %E");
	  break;
	}

      rc -= WAIT_OBJECT_0;
      sigproc_printf ("awake, rc %d", rc);
      LONG *todo;
      if (rc != RC_NOSYNC)
	todo = todos[0];
      else
	todo = todos[1];

      /* A sigcatch semaphore has been signaled.  Scan the sigtodo
         array looking for any unprocessed signals.  */
      pending_signals = false;
      unsigned more_signals = 0;
      bool saw_failed_interrupt = false;
      do
	{
	  more_signals = 0;
	  for (int sig = -__SIGOFFSET; sig < NSIG; sig++)
	    {
	      LONG x = InterlockedDecrement (todo + sig);
	      if (x < 0)
		InterlockedIncrement (todo + sig);
	      else if (x >= 0)
		{
		  /* If x > 0, we have to deal with a signal at some later point */
		  if (rc != RC_NOSYNC && x > 0)
		    pending_signals = true;	// There should be an armed semaphore, in this case

		  if (sig > 0 && sig != SIGKILL && sig != SIGSTOP &&
		      (sigismember (&myself->getsigmask (), sig) ||
		       main_vfork->pid ||
		       (sig != SIGCONT && ISSTATE (myself, PID_STOPPED))))
		    {
		      sigproc_printf ("signal %d blocked", sig);
		      x = InterlockedIncrement (myself->getsigtodo (sig));
		      pending_signals = true;
		    }
		  else
		    {
		      sigproc_printf ("processing signal %d", sig);
		      switch (sig)
			{
			case __SIGFLUSH:
			  if (rc == RC_MAIN)
			    {
			      flush = true;
			      SetEvent (sigcatch_nosync);
			      goto out1;
			    }
			  break;

			/* Internal signal to turn on stracing. */
			case __SIGSTRACE:
			  strace.hello ();
			  break;

			case __SIGCOMMUNE:
			  talktome ();
			  break;

			/* A normal UNIX signal */
			default:
			  sigproc_printf ("Got signal %d", sig);
			  if (!sig_handle (sig))
			    {
			      saw_failed_interrupt = true;
			      x = InterlockedIncrement (myself->getsigtodo (sig));
			      pending_signals = true;
			    }
			}
		      if (rc == RC_NOSYNC && x > 0)
			more_signals++;
		    }

		  if (sig == SIGCHLD)
		    proc_subproc (PROC_CLEARWAIT, 0);

		  /* Need to take special action if an interrupt failed due to main thread not
		     getting around to calling handler yet.  */
		  if (saw_failed_interrupt || rc != RC_NOSYNC)
		    goto out;
		}
	    }
#ifdef DEBUGGING
	  if (more_signals > 100)
	    system_printf ("hmm.  infinite loop? more_signals %u\n", more_signals);
#endif
	}
      while (more_signals && sig_loop_wait);

    out:
      /* Signal completion of signal handling depending on which semaphore
	 woke up the WaitForMultipleObjects above.  */
      if (rc == RC_NONMAIN)	// FIXME: This is broken
	ReleaseSemaphore (sigcomplete_nonmain, 1, NULL);
      else if (rc == RC_MAIN || flush)
	{
	  SetEvent (sigcomplete_main);
	  sigproc_printf ("set main thread completion event");
	  flush = false;
	}

    out1:
      if (saw_failed_interrupt)
	{
	  SetEvent (sigcatch_nosync);
	  low_priority_sleep (0);	/* Hopefully, other thread will be waking up soon. */
	}
      sigproc_printf ("looping");
    }

  sigproc_printf ("done");
  ExitThread (0);
}

/* Wait for subprocesses to terminate. Executes in a separate thread. */
static DWORD WINAPI
wait_subproc (VOID *)
{
  sigproc_printf ("starting");
  int errloop = 0;

  for (;;)
    {
      DWORD rc = WaitForMultipleObjects (nchildren + 1, events, FALSE,
					 proc_loop_wait);
      if (rc == WAIT_TIMEOUT)
	if (!proc_loop_wait)
	  break;			// Exiting
	else
	  continue;

      if (rc == WAIT_FAILED)
	{
	  if (!proc_loop_wait)
	    break;

	  /* It's ok to get an ERROR_INVALID_HANDLE since another thread may have
	     closed a handle in the children[] array.  So, we try looping a couple
	     of times to stabilize. FIXME - this is not foolproof.  Probably, this
	     thread should be responsible for closing the children. */
	  if (!errloop++)
	    proc_subproc (PROC_NOTHING, 0);	// Just synchronize and continue
	  if (errloop < 10)
	    continue;

	  system_printf ("wait failed. nchildren %d, wait %d, %E",
			nchildren, proc_loop_wait);

	  for (int i = 0; i <= nchildren; i++)
	    if ((rc = WaitForSingleObject (events[i], 0)) == WAIT_OBJECT_0 ||
		rc == WAIT_TIMEOUT)
	      continue;
	    else if (i == 0)
		system_printf ("nchildren %d, event[%d] %p, %E", nchildren, i, events[i]);
	    else
	      {
		system_printf ("nchildren %d, event[%d] %p, pchildren[%d] %p, events[0] %p, %E",
			       nchildren, i, events[i], i - 1, (_pinfo *) pchildren[i - 1], events[0]);
		system_printf ("pid %d, dwProcessId %u, hProcess %p, progname '%s'",
			       pchildren[i - 1]->pid, pchildren[i - 1]->dwProcessId,
			       pchildren[i - 1]->hProcess, pchildren[i - 1]->progname);
	      }
	  break;
	}

      errloop = 0;
      rc -= WAIT_OBJECT_0;
      if (rc-- != 0)
	{
	  rc = proc_subproc (PROC_CHILDTERMINATED, rc);
	  if (!proc_loop_wait)		// Don't bother if wait_subproc is
	    break;			//  exiting

	  /* Send a SIGCHLD to myself.   We do this here, rather than in proc_subproc
	     to avoid the proc_subproc lock since the signal thread will eventually
	     be calling proc_subproc and could unnecessarily block. */
	  if (rc)
	    sig_send (myself_nowait, SIGCHLD);
	}
      sigproc_printf ("looping");
    }

  ForceCloseHandle (events[0]);
  events[0] = NULL;
  sigproc_printf ("done");
  ExitThread (0);
}

extern "C" {
/* Provide a stack frame when calling WaitFor* functions */

#undef WaitForSingleObject

DWORD __stdcall
WFSO (HANDLE hHandle, DWORD dwMilliseconds)
{
  DWORD ret;
  sigframe thisframe (mainthread);
  ret = WaitForSingleObject (hHandle, dwMilliseconds);
  return ret;
}

#undef WaitForMultipleObjects

DWORD __stdcall
WFMO (DWORD nCount, CONST HANDLE *lpHandles, BOOL fWaitAll, DWORD dwMilliseconds)
{
  DWORD ret;
  sigframe thisframe (mainthread);
  ret = WaitForMultipleObjects (nCount, lpHandles, fWaitAll, dwMilliseconds);
  return ret;
}
}
