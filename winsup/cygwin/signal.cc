/* signal.cc

   Copyright 1996, 1997, 1998, 1999, 2000, 2001, 2002, 2003 Red Hat, Inc.

   Written by Steve Chamberlain of Cygnus Support, sac@cygnus.com
   Significant changes by Sergey Okhapkin <sos@prospect.com.ru>

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include <stdlib.h>
#include "cygerrno.h"
#include <sys/cygwin.h>
#include "sigproc.h"
#include "pinfo.h"

int sigcatchers;	/* FIXME: Not thread safe. */

#define sigtrapped(func) ((func) != SIG_IGN && (func) != SIG_DFL)

static inline void
set_sigcatchers (void (*oldsig) (int), void (*cursig) (int))
{
#ifdef DEBUGGING
  int last_sigcatchers = sigcatchers;
#endif
  if (!sigtrapped (oldsig) && sigtrapped (cursig))
    sigcatchers++;
  else if (sigtrapped (oldsig) && !sigtrapped (cursig))
    sigcatchers--;
#ifdef DEBUGGING
  if (last_sigcatchers != sigcatchers)
    sigproc_printf ("last %d, old %d, cur %p, cur %p", last_sigcatchers,
		    sigcatchers, oldsig, cursig);
#endif
}

extern "C" _sig_func_ptr
signal (int sig, _sig_func_ptr func)
{
  sig_dispatch_pending ();
  _sig_func_ptr prev;

  /* check that sig is in right range */
  if (sig < 0 || sig >= NSIG || sig == SIGKILL || sig == SIGSTOP)
    {
      set_errno (EINVAL);
      syscall_printf ("SIG_ERR = signal (%d, %p)", sig, func);
      return (_sig_func_ptr) SIG_ERR;
    }

  prev = myself->getsig (sig).sa_handler;
  myself->getsig (sig).sa_handler = func;
  myself->getsig (sig).sa_mask = 0;
  /* SA_RESTART is set to maintain BSD compatible signal behaviour by default.
     This is also compatible with the behaviour of signal(2) in Linux. */
  myself->getsig (sig).sa_flags |= SA_RESTART;
  set_sigcatchers (prev, func);

  syscall_printf ("%p = signal (%d, %p)", prev, sig, func);
  return prev;
}

extern "C" int
nanosleep (const struct timespec *rqtp, struct timespec *rmtp)
{
  int res = 0;
  sig_dispatch_pending ();
  sigframe thisframe (mainthread);
  pthread_testcancel ();

  if (rqtp->tv_sec < 0 || rqtp->tv_nsec < 0 || rqtp->tv_nsec > 999999999)
    {
      set_errno (EINVAL);
      return -1;
    }

  DWORD req = rqtp->tv_sec * 1000 + (rqtp->tv_nsec + 500000) / 1000000;
  DWORD start_time = GetTickCount ();
  DWORD end_time = start_time + req;
  syscall_printf ("nanosleep (%ld)", req);

  int rc = pthread::cancelable_wait (signal_arrived, req);
  DWORD now = GetTickCount ();
  DWORD rem = (rc == WAIT_TIMEOUT || now >= end_time) ? 0 : end_time - now;
  if (rc == WAIT_OBJECT_0)
    {
      (void) thisframe.call_signal_handler ();
      set_errno (EINTR);
      res = -1;
    }

  if (rmtp)
    {
      rmtp->tv_sec = rem / 1000;
      rmtp->tv_nsec = (rem % 1000) * 1000000;
    }

  syscall_printf ("%d = nanosleep (%ld, %ld)", res, req, rem);
  return res;
}

extern "C" unsigned int
sleep (unsigned int seconds)
{
  struct timespec req, rem;
  req.tv_sec = seconds;
  req.tv_nsec = 0;
  nanosleep (&req, &rem);
  return rem.tv_sec + (rem.tv_nsec + 500000000) / 1000000000;
}

extern "C" unsigned int
usleep (unsigned int useconds)
{
  struct timespec req;
  req.tv_sec = useconds / 1000000;
  req.tv_nsec = (useconds % 1000000) * 1000;
  int res = nanosleep (&req, 0);
  return res;
}

extern "C" int
sigprocmask (int sig, const sigset_t *set, sigset_t *oldset)
{
  sig_dispatch_pending ();
  /* check that sig is in right range */
  if (sig < 0 || sig >= NSIG)
    {
      set_errno (EINVAL);
      syscall_printf ("SIG_ERR = sigprocmask signal %d out of range", sig);
      return -1;
    }

  if (oldset)
    *oldset = myself->getsigmask ();
  if (set)
    {
      sigset_t newmask = myself->getsigmask ();
      switch (sig)
	{
	case SIG_BLOCK:
	  /* add set to current mask */
	  newmask |= *set;
	  break;
	case SIG_UNBLOCK:
	  /* remove set from current mask */
	  newmask &= ~*set;
	  break;
	case SIG_SETMASK:
	  /* just set it */
	  newmask = *set;
	  break;
	default:
	  set_errno (EINVAL);
	  return -1;
	}
      (void) set_process_mask (newmask);
    }
  return 0;
}

static int
kill_worker (pid_t pid, int sig)
{
  sig_dispatch_pending ();

  int res = 0;
  pinfo dest (pid);
  BOOL sendSIGCONT;

  if (!dest)
    {
      set_errno (ESRCH);
      return -1;
    }

  dest->setthread2signal (NULL);

  if ((sendSIGCONT = (sig < 0)))
    sig = -sig;

#if 0
  if (dest == myself && !sendSIGCONT)
    dest = myself_nowait_nonmain;
#endif
  if (sig == 0)
    {
      res = proc_exists (dest) ? 0 : -1;
      if (res < 0)
	set_errno (ESRCH);
    }
  else if ((res = sig_send (dest, sig)))
    {
      sigproc_printf ("%d = sig_send, %E ", res);
      res = -1;
    }
  else if (sendSIGCONT)
    (void) sig_send (dest, SIGCONT);

  syscall_printf ("%d = kill_worker (%d, %d)", res, pid, sig);
  return res;
}

int
raise (int sig)
{
  return kill (myself->pid, sig);
}

int
kill (pid_t pid, int sig)
{
  sigframe thisframe (mainthread);
  syscall_printf ("kill (%d, %d)", pid, sig);
  /* check that sig is in right range */
  if (sig < 0 || sig >= NSIG)
    {
      set_errno (EINVAL);
      syscall_printf ("signal %d out of range", sig);
      return -1;
    }

  /* Silently ignore stop signals from a member of orphaned process group.
     FIXME: Why??? */
  if (ISSTATE (myself, PID_ORPHANED) &&
      (sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU))
    sig = 0;

  return (pid > 0) ? kill_worker (pid, sig) : kill_pgrp (-pid, sig);
}

int
kill_pgrp (pid_t pid, int sig)
{
  int res = 0;
  int found = 0;
  int killself = 0;
  sigframe thisframe (mainthread);

  sigproc_printf ("pid %d, signal %d", pid, sig);

  winpids pids;
  for (unsigned i = 0; i < pids.npids; i++)
    {
      _pinfo *p = pids[i];

      if (!proc_exists (p))
	continue;

      /* Is it a process we want to kill?  */
      if ((pid == 0 && (p->pgid != myself->pgid || p->ctty != myself->ctty)) ||
	  (pid > 1 && p->pgid != pid) ||
	  (sig < 0 && NOTSTATE (p, PID_STOPPED)))
	continue;
      sigproc_printf ("killing pid %d, pgrp %d, p->ctty %d, myself->ctty %d",
		      p->pid, p->pgid, p->ctty, myself->ctty);
      if (p == myself)
	killself++;
      else if (kill_worker (p->pid, sig))
	res = -1;
      found++;
    }

  if (killself && kill_worker (myself->pid, sig))
    res = -1;

  if (!found)
    {
      set_errno (ESRCH);
      res = -1;
    }
  syscall_printf ("%d = kill (%d, %d)", res, pid, sig);
  return res;
}

extern "C" int
killpg (pid_t pgrp, int sig)
{
  return kill (-pgrp, sig);
}

extern "C" void
abort (void)
{
  sig_dispatch_pending ();
  sigframe thisframe (mainthread);
  /* Flush all streams as per SUSv2.
     From my reading of this document, this isn't strictly correct.
     The streams are supposed to be flushed prior to exit.  However,
     if there is I/O in any signal handler that will not necessarily
     be flushed.
     However this is the way FreeBSD does it, and it is much easier to
     do things this way, so... */
  if (_REENT->__cleanup)
    _REENT->__cleanup (_REENT);

  /* Ensure that SIGABRT can be caught regardless of blockage. */
  sigset_t sig_mask;
  sigfillset (&sig_mask);
  sigdelset (&sig_mask, SIGABRT);
  set_process_mask (sig_mask);

  raise (SIGABRT);
  (void) thisframe.call_signal_handler (); /* Call any signal handler */
  do_exit (1);	/* signal handler didn't exit.  Goodbye. */
}

extern "C" int
sigaction (int sig, const struct sigaction *newact, struct sigaction *oldact)
{
  sig_dispatch_pending ();
  sigproc_printf ("signal %d, newact %p, oldact %p", sig, newact, oldact);
  /* check that sig is in right range */
  if (sig < 0 || sig >= NSIG)
    {
      set_errno (EINVAL);
      syscall_printf ("SIG_ERR = sigaction signal %d out of range", sig);
      return -1;
    }

  struct sigaction oa = myself->getsig (sig);

  if (newact)
    {
      if (sig == SIGKILL || sig == SIGSTOP)
	{
	  set_errno (EINVAL);
	  return -1;
	}
      myself->getsig (sig) = *newact;
      if (newact->sa_handler == SIG_IGN)
	sig_clear (sig);
      if (newact->sa_handler == SIG_DFL && sig == SIGCHLD)
	sig_clear (sig);
      set_sigcatchers (oa.sa_handler, newact->sa_handler);
    }

  if (oldact)
    *oldact = oa;

  return 0;
}

extern "C" int
sigaddset (sigset_t *set, const int sig)
{
  /* check that sig is in right range */
  if (sig <= 0 || sig >= NSIG)
    {
      set_errno (EINVAL);
      syscall_printf ("SIG_ERR = sigaddset signal %d out of range", sig);
      return -1;
    }

  *set |= SIGTOMASK (sig);
  return 0;
}

extern "C" int
sigdelset (sigset_t *set, const int sig)
{
  /* check that sig is in right range */
  if (sig <= 0 || sig >= NSIG)
    {
      set_errno (EINVAL);
      syscall_printf ("SIG_ERR = sigdelset signal %d out of range", sig);
      return -1;
    }

  *set &= ~SIGTOMASK (sig);
  return 0;
}

extern "C" int
sigismember (const sigset_t *set, int sig)
{
  /* check that sig is in right range */
  if (sig <= 0 || sig >= NSIG)
    {
      set_errno (EINVAL);
      syscall_printf ("SIG_ERR = sigdelset signal %d out of range", sig);
      return -1;
    }

  if (*set & SIGTOMASK (sig))
    return 1;
  else
    return 0;
}

extern "C" int
sigemptyset (sigset_t *set)
{
  *set = (sigset_t) 0;
  return 0;
}

extern "C" int
sigfillset (sigset_t *set)
{
  *set = ~((sigset_t) 0);
  return 0;
}

extern "C" int
sigsuspend (const sigset_t *set)
{
  return handle_sigsuspend (*set);
}

extern "C" int
sigpause (int signal_mask)
{
  return handle_sigsuspend ((sigset_t) signal_mask);
}

extern "C" int
pause (void)
{
  return handle_sigsuspend (myself->getsigmask ());
}

extern "C" int
siginterrupt (int sig, int flag)
{
  struct sigaction act;
  (void)sigaction(sig, NULL, &act);
  if (flag)
    act.sa_flags &= ~SA_RESTART;
  else
    act.sa_flags |= SA_RESTART;
  return sigaction(sig, &act, NULL);
}

