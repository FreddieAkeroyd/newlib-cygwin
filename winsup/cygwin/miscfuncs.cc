/* miscfuncs.cc: misc funcs that don't belong anywhere else

   Copyright 1996, 1997, 1998, 1999, 2000, 2001, 2002, 2003 Red Hat, Inc.

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#define _WIN32_WINNT 0x400
#include "winsup.h"
#include "cygerrno.h"
#include <sys/errno.h>
#include <sys/uio.h>
#include <assert.h>
#include <limits.h>
#include <winbase.h>
#include <winnls.h>
#include "cygthread.h"

long tls_ix = -1;

const char case_folded_lower[] NO_COPY = {
   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,  15,
  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,
  32, '!', '"', '#', '$', '%', '&',  39, '(', ')', '*', '+', ',', '-', '.', '/',
 '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
 '@', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '[',  92, ']', '^', '_',
 '`', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',
 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '{', '|', '}', '~', 127,
 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255
};

const char case_folded_upper[] NO_COPY = {
   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13,  14,  15,
  16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,
  32, '!', '"', '#', '$', '%', '&',  39, '(', ')', '*', '+', ',', '-', '.', '/',
 '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ':', ';', '<', '=', '>', '?',
 '@', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '[',  92, ']', '^', '_',
 '`', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '{', '|', '}', '~', 127,
 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255
};

const char isalpha_array[] NO_COPY = {
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,   0,   0,   0,   0,   0,
   0,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,
0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x20,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

#define ch_case_eq(ch1, ch2) (cyg_tolower(ch1) == cyg_tolower(ch2))

#if 0

/* Return TRUE if two strings match up to length n */
extern "C" int __stdcall
strncasematch (const char *s1, const char *s2, size_t n)
{
  if (s1 == s2)
    return 1;

  n++;
  while (--n && *s1)
    {
      if (!ch_case_eq (*s1, *s2))
	return 0;
      s1++; s2++;
    }
  return !n || *s2 == '\0';
}

/* Return TRUE if two strings match */
extern "C" int __stdcall
strcasematch (const char *s1, const char *s2)
{
  if (s1 == s2)
    return 1;

  while (*s1)
    {
      if (!ch_case_eq (*s1, *s2))
	return 0;
      s1++; s2++;
    }
  return *s2 == '\0';
}
#endif

extern "C" char * __stdcall
strcasestr (const char *searchee, const char *lookfor)
{
  if (*searchee == 0)
    {
      if (*lookfor)
	return NULL;
      return (char *) searchee;
    }

  while (*searchee)
    {
      int i = 0;
      while (1)
	{
	  if (lookfor[i] == 0)
	    return (char *) searchee;

	  if (!ch_case_eq (lookfor[i], searchee[i]))
	    break;
	  lookfor++;
	}
      searchee++;
    }

  return NULL;
}

int __stdcall
check_null_str (const char *name)
{
  if (name && !IsBadStringPtr (name, MAX_PATH))
    return 0;

  return EFAULT;
}

int __stdcall
check_null_empty_str (const char *name)
{
  if (name && !IsBadStringPtr (name, MAX_PATH))
    return !*name ? ENOENT : 0;

  return EFAULT;
}

int __stdcall
check_null_empty_str_errno (const char *name)
{
  int __err;
  if ((__err = check_null_empty_str (name)))
    set_errno (__err);
  return __err;
}

int __stdcall
check_null_str_errno (const char *name)
{
  int __err;
  if ((__err = check_null_str (name)))
    set_errno (__err);
  return __err;
}

int __stdcall
__check_null_invalid_struct (void *s, unsigned sz)
{
  if (s && !IsBadWritePtr (s, sz))
    return 0;

  return EFAULT;
}

int __stdcall
__check_null_invalid_struct_errno (void *s, unsigned sz)
{
  int err;
  if ((err = __check_null_invalid_struct (s, sz)))
    set_errno (err);
  return err;
}

int __stdcall
__check_invalid_read_ptr_errno (const void *s, unsigned sz)
{
  if (s && !IsBadReadPtr (s, sz))
    return 0;
  return set_errno (EFAULT);
}

ssize_t
check_iovec_for_read (const struct iovec *iov, int iovcnt)
{
  if (iovcnt <= 0 || iovcnt > IOV_MAX)
    {
      set_errno (EINVAL);
      return -1;
    }

  if (__check_invalid_read_ptr_errno (iov, iovcnt * sizeof (*iov)))
    return -1;

  size_t tot = 0;

  while (iovcnt != 0)
    {
      if (iov->iov_len > SSIZE_MAX || (tot += iov->iov_len) > SSIZE_MAX)
	{
	  set_errno (EINVAL);
	  return -1;
	}

      if (iov->iov_len
	  && __check_null_invalid_struct_errno (iov->iov_base, iov->iov_len))
	return -1;

      iov += 1;
      iovcnt -= 1;
    }

  assert (tot <= SSIZE_MAX);

  return (ssize_t) tot;
}

ssize_t
check_iovec_for_write (const struct iovec *iov, int iovcnt)
{
  if (iovcnt <= 0 || iovcnt > IOV_MAX)
    {
      set_errno (EINVAL);
      return -1;
    }

  if (__check_invalid_read_ptr_errno (iov, iovcnt * sizeof (*iov)))
    return -1;

  size_t tot = 0;

  while (iovcnt != 0)
    {
      if (iov->iov_len > SSIZE_MAX || (tot += iov->iov_len) > SSIZE_MAX)
	{
	  set_errno (EINVAL);
	  return -1;
	}

      if (iov->iov_len
	  && __check_invalid_read_ptr_errno (iov->iov_base, iov->iov_len))
	return -1;

      iov += 1;
      iovcnt -= 1;
    }

  assert (tot <= SSIZE_MAX);

  return (ssize_t) tot;
}

UINT
get_cp ()
{
  return current_codepage == ansi_cp ? GetACP() : GetOEMCP();
}

int __stdcall
sys_wcstombs (char *tgt, const WCHAR *src, int len)
{
  return WideCharToMultiByte (get_cp (), 0, src, -1, tgt, len, NULL, NULL);
}

int __stdcall
sys_mbstowcs (WCHAR *tgt, const char *src, int len)
{
  return MultiByteToWideChar (get_cp (), 0, src, -1, tgt, len);
}

int __stdcall
low_priority_sleep (DWORD secs)
{
  HANDLE thisthread = GetCurrentThread ();
  int curr_prio = GetThreadPriority (thisthread);
  bool staylow;
  if (secs != INFINITE)
    staylow = false;
  else
    {
      secs = 0;
      staylow = true;
    }

  if (!secs && wincap.has_switch_to_thread ())
    {
      for (int i = 0; i < 10; i++)
	SwitchToThread ();
    }
  else
    {
      int new_prio;
      if (GetCurrentThreadId () == cygthread::main_thread_id)
	new_prio = THREAD_PRIORITY_LOWEST;
      else
	new_prio = GetThreadPriority (hMainThread);

      if (curr_prio != new_prio)
	/* Force any threads in normal priority to be scheduled */
	SetThreadPriority (thisthread, new_prio);
      Sleep (secs);

      if (!staylow || curr_prio == new_prio)
	SetThreadPriority (thisthread, curr_prio);
    }

  return curr_prio;
}
