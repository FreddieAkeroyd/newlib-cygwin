/* cygserver_transport_pipes.cc

   Copyright 2001, 2002 Red Hat Inc.

   Written by Robert Collins <rbtcollins@hotmail.com>

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

/* to allow this to link into cygwin and the .dll, a little magic is needed. */
#ifdef __OUTSIDE_CYGWIN__
#include "woutsup.h"
#else
#include "winsup.h"
#endif

#include <sys/types.h>

#include <assert.h>
#include <netdb.h>
#include <pthread.h>
#include <unistd.h>

#include "cygerrno.h"
#include "cygwin/cygserver_transport.h"
#include "cygwin/cygserver_transport_pipes.h"

#ifndef __INSIDE_CYGWIN__
#include "cygwin/cygserver.h"
#endif

enum
  {
    MAX_WAIT_NAMED_PIPE_RETRY = 64,
    WAIT_NAMED_PIPE_TIMEOUT = 10 // milliseconds
  };

#ifndef __INSIDE_CYGWIN__

static pthread_once_t pipe_instance_lock_once = PTHREAD_ONCE_INIT;
static CRITICAL_SECTION pipe_instance_lock;
static long pipe_instance = 0;

static void
initialise_pipe_instance_lock ()
{
  assert (pipe_instance == 0);
  InitializeCriticalSection (&pipe_instance_lock);
}

#endif /* !__INSIDE_CYGWIN__ */

#ifndef __INSIDE_CYGWIN__

transport_layer_pipes::transport_layer_pipes (const HANDLE hPipe)
  : _pipe_name (""),
    _hPipe (hPipe),
    _is_accepted_endpoint (true),
    _is_listening_endpoint (false)
{
  assert (_hPipe);
  assert (_hPipe != INVALID_HANDLE_VALUE);

  init_security ();
}

#endif /* !__INSIDE_CYGWIN__ */

transport_layer_pipes::transport_layer_pipes ()
  : _pipe_name ("\\\\.\\pipe\\cygwin_lpc"),
    _hPipe (NULL),
    _is_accepted_endpoint (false),
    _is_listening_endpoint (false)
{
  init_security ();
}

void
transport_layer_pipes::init_security ()
{
  assert (wincap.has_security ());

  /* FIXME: pthread_once or equivalent needed */

  InitializeSecurityDescriptor (&_sd, SECURITY_DESCRIPTOR_REVISION);
  SetSecurityDescriptorDacl (&_sd, TRUE, NULL, FALSE);

  _sec_all_nih.nLength = sizeof (SECURITY_ATTRIBUTES);
  _sec_all_nih.lpSecurityDescriptor = &_sd;
  _sec_all_nih.bInheritHandle = FALSE;
}

transport_layer_pipes::~transport_layer_pipes ()
{
  close ();
}

#ifndef __INSIDE_CYGWIN__

int
transport_layer_pipes::listen ()
{
  assert (!_hPipe);
  assert (!_is_accepted_endpoint);
  assert (!_is_listening_endpoint);

  _is_listening_endpoint = true;

  /* no-op */
  return 0;
}

class transport_layer_pipes *
transport_layer_pipes::accept (bool *const recoverable)
{
  assert (!_hPipe);
  assert (!_is_accepted_endpoint);
  assert (_is_listening_endpoint);

  pthread_once (&pipe_instance_lock_once, &initialise_pipe_instance_lock);

  EnterCriticalSection (&pipe_instance_lock);

  // Read: http://www.securityinternals.com/research/papers/namedpipe.php
  // See also the Microsoft security bulletins MS00-053 and MS01-031.

  // FIXME: Remove FILE_CREATE_PIPE_INSTANCE.

  const bool first_instance = (pipe_instance == 0);

  const HANDLE accept_pipe =
    CreateNamedPipe (_pipe_name,
		     (PIPE_ACCESS_DUPLEX
		      | (first_instance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0)),
		     (PIPE_TYPE_BYTE | PIPE_WAIT),
		     PIPE_UNLIMITED_INSTANCES,
		     0, 0, 1000,
		     &_sec_all_nih);

  const bool duplicate = (accept_pipe == INVALID_HANDLE_VALUE
			  && pipe_instance == 0
			  && GetLastError () == ERROR_ACCESS_DENIED);

  if (accept_pipe != INVALID_HANDLE_VALUE)
    InterlockedIncrement (&pipe_instance);

  LeaveCriticalSection (&pipe_instance_lock);

  if (duplicate)
    {
      *recoverable = false;
      system_printf ("failed to create named pipe: "
		     "is the daemon already running?");
      return NULL;
    }

  if (accept_pipe == INVALID_HANDLE_VALUE)
    {
      debug_printf ("error creating pipe (%lu).", GetLastError ());
      *recoverable = true;	// FIXME: case analysis?
      return NULL;
    }

  assert (accept_pipe);

  if (!ConnectNamedPipe (accept_pipe, NULL)
      && GetLastError () != ERROR_PIPE_CONNECTED)
    {
      debug_printf ("error connecting to pipe (%lu)", GetLastError ());
      (void) CloseHandle (accept_pipe);
      *recoverable = true;	// FIXME: case analysis?
      return NULL;
    }

  return safe_new (transport_layer_pipes, accept_pipe);
}

#endif /* !__INSIDE_CYGWIN__ */

void
transport_layer_pipes::close ()
{
  // verbose: debug_printf ("closing pipe %p", _hPipe);

  if (_hPipe)
    {
      assert (_hPipe != INVALID_HANDLE_VALUE);

#ifndef __INSIDE_CYGWIN__

      if (_is_accepted_endpoint)
	{
	  (void) FlushFileBuffers (_hPipe); // Blocks until client reads.
	  (void) DisconnectNamedPipe (_hPipe);
	  EnterCriticalSection (&pipe_instance_lock);
	  (void) CloseHandle (_hPipe);
	  assert (pipe_instance > 0);
	  InterlockedDecrement (&pipe_instance);
	  LeaveCriticalSection (&pipe_instance_lock);
	}
      else
	(void) CloseHandle (_hPipe);

#else /* __INSIDE_CYGWIN__ */

      assert (!_is_accepted_endpoint);
      (void) ForceCloseHandle (_hPipe);

#endif /* __INSIDE_CYGWIN__ */

      _hPipe = NULL;
    }
}

ssize_t
transport_layer_pipes::read (void *const buf, const size_t len)
{
  // verbose: debug_printf ("reading from pipe %p", _hPipe);

  assert (_hPipe);
  assert (_hPipe != INVALID_HANDLE_VALUE);
  assert (!_is_listening_endpoint);

  DWORD count;
  if (!ReadFile (_hPipe, buf, len, &count, NULL))
    {
      debug_printf ("error reading from pipe (%lu)", GetLastError ());
      set_errno (EINVAL);	// FIXME?
      return -1;
    }

  return count;
}

ssize_t
transport_layer_pipes::write (void *const buf, const size_t len)
{
  // verbose: debug_printf ("writing to pipe %p", _hPipe);

  assert (_hPipe);
  assert (_hPipe != INVALID_HANDLE_VALUE);
  assert (!_is_listening_endpoint);

  DWORD count;
  if (!WriteFile (_hPipe, buf, len, &count, NULL))
    {
      debug_printf ("error writing to pipe, error = %lu", GetLastError ());
      set_errno (EINVAL);	// FIXME?
      return -1;
    }

  return count;
}

/*
 * This routine holds a static variable, assume_cygserver, that is set
 * if the transport has good reason to think that cygserver is
 * running, i.e. if if successfully connected to it with the previous
 * attempt.  If this is set, the code tries a lot harder to get a
 * connection, making the assumption that any failures are just
 * congestion and overloading problems.
 */

int
transport_layer_pipes::connect ()
{
  assert (!_hPipe);
  assert (!_is_accepted_endpoint);
  assert (!_is_listening_endpoint);

  static bool assume_cygserver = false;

  BOOL rc = TRUE;
  int retries = 0;

  while (rc)
    {
      _hPipe = CreateFile (_pipe_name,
			   GENERIC_READ | GENERIC_WRITE,
			   FILE_SHARE_READ | FILE_SHARE_WRITE,
			   &_sec_all_nih,
			   OPEN_EXISTING,
			   SECURITY_IMPERSONATION,
			   NULL);

      if (_hPipe != INVALID_HANDLE_VALUE)
	{
	  assert (_hPipe);
#ifdef __INSIDE_CYGWIN__
	  ProtectHandle (_hPipe);
#endif
	  assume_cygserver = true;
	  return 0;
	}

      _hPipe = NULL;

      if (!assume_cygserver && GetLastError () != ERROR_PIPE_BUSY)
	{
	  debug_printf ("Error opening the pipe (%lu)", GetLastError ());
	  return -1;
	}

      /* Note: `If no instances of the specified named pipe exist, the
       * WaitNamedPipe function returns immediately, regardless of the
       * time-out value.'  Thus the explicit Sleep if the call fails
       * with ERROR_FILE_NOT_FOUND.
       */
      while (retries != MAX_WAIT_NAMED_PIPE_RETRY
	     && !(rc = WaitNamedPipe (_pipe_name, WAIT_NAMED_PIPE_TIMEOUT)))
	{
	  if (GetLastError () == ERROR_FILE_NOT_FOUND)
	    Sleep (0);		// Give the server a chance.

	  retries += 1;
	}
    }

  assert (retries == MAX_WAIT_NAMED_PIPE_RETRY);

  system_printf ("lost connection to cygserver, error = %lu",
		 GetLastError ());

  assume_cygserver = false;

  return -1;
}

#ifndef __INSIDE_CYGWIN__

void
transport_layer_pipes::impersonate_client ()
{
  assert (_hPipe);
  assert (_hPipe != INVALID_HANDLE_VALUE);
  assert (_is_accepted_endpoint);

  // verbose: debug_printf ("impersonating pipe %p", _hPipe);
  if (_hPipe)
    {
      assert (_hPipe != INVALID_HANDLE_VALUE);

      if (!ImpersonateNamedPipeClient (_hPipe))
	debug_printf ("Failed to Impersonate the client, (%lu)",
		      GetLastError ());
    }
  // verbose: debug_printf ("I am who you are");
}

void
transport_layer_pipes::revert_to_self ()
{
  assert (_is_accepted_endpoint);

  RevertToSelf ();
  // verbose: debug_printf ("I am who I yam");
}

#endif /* !__INSIDE_CYGWIN__ */
