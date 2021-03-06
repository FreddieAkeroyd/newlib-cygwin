/* threaded_queue.cc

   Copyright 2001, 2002 Red Hat Inc.

   Written by Robert Collins <rbtcollins@hotmail.com>

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "woutsup.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <stdlib.h>
#include "threaded_queue.h"

/*****************************************************************************/

/* queue_request */

queue_request::~queue_request ()
{}

/*****************************************************************************/

/* threaded_queue */

threaded_queue::threaded_queue (const size_t initial_workers)
  : _workers_count (0),
    _running (false),
    _submitters_head (NULL),
    _requests_count (0),
    _requests_head (NULL),
    _requests_sem (NULL)
{
  InitializeCriticalSection (&_queue_lock);

  // This semaphore's count is the number of requests on the queue.
  // The maximum count (129792) is calculated as MAXIMUM_WAIT_OBJECTS
  // multiplied by max. threads per process (2028?), which is (a few)
  // more requests than could ever be pending with the current design.

  _requests_sem = CreateSemaphore (NULL,   // SECURITY_ATTRIBUTES
				   0,	   // Initial count
				   129792, // Maximum count
				   NULL);  // Anonymous

  if (!_requests_sem)
    {
      system_printf (("failed to create the request queue semaphore, "
		      "error = %lu"),
		     GetLastError ());
      abort ();
    }

  create_workers (initial_workers);
}

threaded_queue::~threaded_queue ()
{
  if (_running)
    stop ();

  debug_printf ("deleting all pending queue requests");
  queue_request *reqptr = _requests_head;
  while (reqptr)
    {
      queue_request *const ptr = reqptr;
      reqptr = reqptr->_next;
      safe_delete (ptr);
    }

  DeleteCriticalSection (&_queue_lock);
  if (_requests_sem)
    (void) CloseHandle (_requests_sem);
}

/* FIXME: return success or failure rather than quitting */
void
threaded_queue::add_submission_loop (queue_submission_loop *const submitter)
{
  assert (this);
  assert (submitter);
  assert (submitter->_queue == this);
  assert (!submitter->_next);

  submitter->_next =
    TInterlockedExchangePointer (&_submitters_head, submitter);

  if (_running)
    submitter->start ();
}

bool
threaded_queue::start ()
{
  EnterCriticalSection (&_queue_lock);
  const bool was_running = _running;
  _running = true;
  queue_submission_loop *loopptr = _submitters_head;
  LeaveCriticalSection (&_queue_lock);

  if (!was_running)
    {
      debug_printf ("starting all queue submission loops");

      while (loopptr)
	{
	  queue_submission_loop *const ptr = loopptr;
	  loopptr = loopptr->_next;
	  ptr->start ();
	}
    }

  return was_running;
}

bool
threaded_queue::stop ()
{
  EnterCriticalSection (&_queue_lock);
  const bool was_running = _running;
  _running = false;
  queue_submission_loop *loopptr = _submitters_head;
  LeaveCriticalSection (&_queue_lock);

  if (was_running)
    {
      debug_printf ("stopping all queue submission loops");
      while (loopptr)
	{
	  queue_submission_loop *const ptr = loopptr;
	  loopptr = loopptr->_next;
	  ptr->stop ();
	}

      ReleaseSemaphore (_requests_sem, _workers_count, NULL);
      while (_workers_count)
	{
	  debug_printf (("waiting for worker threads to terminate: "
			 "%lu still running"),
			_workers_count);
	  Sleep (1000);
	}
      debug_printf ("all worker threads have terminated");
    }

  return was_running;
}

/* FIXME: return success or failure */
void
threaded_queue::add (queue_request *const therequest)
{
  assert (this);
  assert (therequest);
  assert (!therequest->_next);

  if (!_workers_count)
    {
      system_printf ("warning: no worker threads to handle request!");
      // FIXME: And then what?
    }

  EnterCriticalSection (&_queue_lock);
  if (!_requests_head)
    _requests_head = therequest;
  else
    {
      /* Add to the queue end. */
      queue_request *reqptr = _requests_head;
      for (; reqptr->_next; reqptr = reqptr->_next)
	{}
      assert (reqptr);
      assert (!reqptr->_next);
      reqptr->_next = therequest;
    }

  _requests_count += 1;
  assert (_requests_count > 0);
  LeaveCriticalSection (&_queue_lock);

  (void) ReleaseSemaphore (_requests_sem, 1, NULL);
}

/*static*/ DWORD WINAPI
threaded_queue::start_routine (const LPVOID lpParam)
{
  class threaded_queue *const queue = (class threaded_queue *) lpParam;
  assert (queue);

  queue->worker_loop ();

  const long count = InterlockedDecrement (&queue->_workers_count);
  assert (count >= 0);

  if (queue->_running)
    debug_printf ("worker loop has exited; thread about to terminate");

  return 0;
}

/* Called from the constructor: so no need to be thread-safe until the
 * worker threads start to be created; thus the interlocked increment
 * of the `_workers_count' field.
 */

void
threaded_queue::create_workers (const size_t initial_workers)
{
  assert (initial_workers > 0);

  for (unsigned int i = 0; i != initial_workers; i++)
    {
      const long count = InterlockedIncrement (&_workers_count);
      assert (count > 0);

      DWORD tid;
      const HANDLE hThread =
	CreateThread (NULL, 0, start_routine, this, 0, &tid);

      if (!hThread)
	{
	  system_printf ("failed to create thread, error = %lu",
			 GetLastError ());
	  abort ();
	}

      (void) CloseHandle (hThread);
    }
}

void
threaded_queue::worker_loop ()
{
  while (true)
    {
      const DWORD rc = WaitForSingleObject (_requests_sem, INFINITE);
      if (rc == WAIT_FAILED)
	{
	  system_printf ("wait for request semaphore failed, error = %lu",
			 GetLastError ());
	  return;
	}
      assert (rc == WAIT_OBJECT_0);

      EnterCriticalSection (&_queue_lock);
      if (!_running)
	{
	  LeaveCriticalSection (&_queue_lock);
	  return;
	}

      assert (_requests_head);
      queue_request *const reqptr = _requests_head;
      _requests_head = reqptr->_next;

      _requests_count -= 1;
      assert (_requests_count >= 0);
      LeaveCriticalSection (&_queue_lock);

      assert (reqptr);
      reqptr->process ();
      safe_delete (reqptr);
    }
}

/*****************************************************************************/

/* queue_submission_loop */

queue_submission_loop::queue_submission_loop (threaded_queue *const queue,
					      const bool ninterruptible)
  : _running (false),
    _interrupt_event (NULL),
    _queue (queue),
    _interruptible (ninterruptible),
    _hThread (NULL),
    _tid (0),
    _next (NULL)
{
  if (_interruptible)
    {
      // verbose: debug_printf ("creating an interruptible processing thread");

      _interrupt_event = CreateEvent (NULL,  // SECURITY_ATTRIBUTES
				      FALSE, // Auto-reset
				      FALSE, // Initially non-signalled
				      NULL); // Anonymous

      if (!_interrupt_event)
	{
	  system_printf ("failed to create interrupt event, error = %lu",
			 GetLastError ());
	  abort ();
	}
    }
}

queue_submission_loop::~queue_submission_loop ()
{
  if (_running)
    stop ();
  if (_interrupt_event)
    (void) CloseHandle (_interrupt_event);
  if (_hThread)
    (void) CloseHandle (_hThread);
}

bool
queue_submission_loop::start ()
{
  assert (this);
  assert (!_hThread);

  const bool was_running = _running;

  if (!was_running)
    {
      _running = true;

      _hThread = CreateThread (NULL, 0, start_routine, this, 0, &_tid);
      if (!_hThread)
	{
	  system_printf ("failed to create thread, error = %lu",
			 GetLastError ());
	  abort ();
	}
    }

  return was_running;
}

bool
queue_submission_loop::stop ()
{
  assert (this);
  assert (_hThread && _hThread != INVALID_HANDLE_VALUE);

  const bool was_running = _running;

  if (_running)
    {
      _running = false;

      if (_interruptible)
	{
	  assert (_interrupt_event
		  && _interrupt_event != INVALID_HANDLE_VALUE);

	  SetEvent (_interrupt_event);

	  if (WaitForSingleObject (_hThread, 1000) == WAIT_TIMEOUT)
	    {
	      system_printf (("request loop thread %lu failed to shutdown "
			      "when asked politely: about to get heavy"),
			     _tid);

	      if (!TerminateThread (_hThread, 0))
		{
		  system_printf (("failed to kill request loop thread %lu"
				  ", error = %lu"),
				 _tid, GetLastError ());
		  abort ();
		}
	    }
	}
      else
	{
	  // FIXME: could wait to see if the request loop notices that
	  // the submission loop is no longer running and shuts down
	  // voluntarily.

	  debug_printf ("killing request loop thread %lu", _tid);

	  if (!TerminateThread (_hThread, 0))
	    system_printf (("failed to kill request loop thread %lu"
			    ", error = %lu"),
			   _tid, GetLastError ());
	}
    }

  return was_running;
}

/*static*/ DWORD WINAPI
queue_submission_loop::start_routine (const LPVOID lpParam)
{
  class queue_submission_loop *const submission_loop =
    (class queue_submission_loop *) lpParam;
  assert (submission_loop);

  submission_loop->request_loop ();

  debug_printf ("submission loop has exited; thread about to terminate");

  submission_loop->stop ();

  return 0;
}

/*****************************************************************************/
