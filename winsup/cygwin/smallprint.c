/* smallprint.c: small print routines for WIN32

   Copyright 1996, 1998, 2000, 2001, 2002 Red Hat, Inc.

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include <stdarg.h>
#include <stdlib.h>
#include <ctype.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int __small_sprintf (char *dst, const char *fmt,...);
int __small_vsprintf (char *dst, const char *fmt, va_list ap);

static char *
rn (char *dst, int base, int dosign, long long val, int len, int pad)
{
  /* longest number is ULLONG_MAX, 18446744073709551615, 20 digits */
  unsigned uval;
  char res[20];
  static const char str[16] = "0123456789ABCDEF";
  int l = 0;

  if (dosign && val < 0)
    {
      *dst++ = '-';
      uval = -val;
    }
  else if (dosign > 0 && val > 0)
    {
      *dst++ = '+';
      uval = val;
    }
  else
    {
      uval = val;
    }

  do
    {
      res[l++] = str[uval % base];
      uval /= base;
    }
  while (uval);

  while (len -- > l)
    *dst++ = pad;

  while (l > 0)
    {
      *dst++ = res[--l];
    }

  return dst;
}

int
__small_vsprintf (char *dst, const char *fmt, va_list ap)
{
  char tmp[MAX_PATH + 1];
  char *orig = dst;
  const char *s;

  while (*fmt)
    {
      int i, n = 0x7fff;
      if (*fmt != '%')
	*dst++ = *fmt++;
      else
	{
	  int len = 0;
	  char pad = ' ';
	  int addsign = -1;

	  switch (*++fmt)
	  {
	    case '+':
	      addsign = 1;
	      fmt++;
	      break;
	    case '%':
	      *dst++ = *fmt++;
	      continue;
	  }

	  for (;;)
	    {
	      char c = *fmt++;
	      switch (c)
		{
		case '0':
		  if (len == 0)
		    {
		      pad = '0';
		      continue;
		    }
		case '1': case '2': case '3': case '4':
		case '5': case '6': case '7': case '8': case '9':
		  len = len * 10 + (c - '0');
		  continue;
		case 'l':
		  continue;
		case 'c':
		  {
		    int c = va_arg (ap,int);
		    if (c > ' ' && c <= 127)
		      *dst++ = c;
		    else
		      {
			*dst++ = '0';
			*dst++ = 'x';
			dst = rn (dst, 16, 0, c, len, pad);
		      }
		  }
		  break;
		case 'E':
		  strcpy (dst, "Win32 error ");
		  dst = rn (dst + sizeof ("Win32 error"), 10, 0, GetLastError (), len, pad);
		  break;
		case 'd':
		  dst = rn (dst, 10, addsign, va_arg (ap, int), len, pad);
		  break;
		case 'D':
		  dst = rn (dst, 10, addsign, va_arg (ap, long long), len, pad);
		  break;
		case 'u':
		  dst = rn (dst, 10, 0, va_arg (ap, int), len, pad);
		  break;
		case 'U':
		  dst = rn (dst, 10, 0, va_arg (ap, long long), len, pad);
		  break;
		case 'o':
		  dst = rn (dst, 8, 0, va_arg (ap, unsigned), len, pad);
		  break;
		case 'p':
		  *dst++ = '0';
		  *dst++ = 'x';
		  /* fall through */
		case 'x':
		  dst = rn (dst, 16, 0, va_arg (ap, int), len, pad);
		  break;
		case 'X':
		  dst = rn (dst, 16, 0, va_arg (ap, long long), len, pad);
		  break;
		case 'P':
		  if (!GetModuleFileName (NULL, tmp, MAX_PATH))
		    s = "cygwin program";
		  else
		    s = tmp;
		  goto fillin;
		case '.':
		  n = strtol (fmt, (char **)&fmt, 10);
		  if (*fmt++ != 's')
		    goto endfor;
		case 's':
		  s = va_arg (ap, char *);
		  if (s == NULL)
		    s = "(null)";
		fillin:
		  for (i = 0; *s && i < n; i++)
		    *dst++ = *s++;
		  break;
		default:
		  *dst++ = '?';
		  *dst++ = fmt[-1];
		}
	    endfor:
	      break;
	    }
	}
    }
  *dst = 0;
  return dst - orig;
}

int
__small_sprintf (char *dst, const char *fmt,...)
{
  int r;
  va_list ap;
  va_start (ap, fmt);
  r = __small_vsprintf (dst, fmt, ap);
  va_end (ap);
  return r;
}

void
small_printf (const char *fmt,...)
{
  char buf[16384];
  va_list ap;
  DWORD done;
  int count;

#if 0	/* Turn on to force console errors */
  extern SECURITY_ATTRIBUTES sec_none;
  HANDLE h = CreateFileA ("CONOUT$", GENERIC_READ|GENERIC_WRITE,
		   FILE_SHARE_WRITE | FILE_SHARE_WRITE, &sec_none,
		   OPEN_EXISTING, 0, 0);
  if (h)
    SetStdHandle (STD_ERROR_HANDLE, h);
#endif

  va_start (ap, fmt);
  count = __small_vsprintf (buf, fmt, ap);
  va_end (ap);

  WriteFile (GetStdHandle (STD_ERROR_HANDLE), buf, count, &done, NULL);
  FlushFileBuffers (GetStdHandle (STD_ERROR_HANDLE));
}

#ifdef DEBUGGING
static HANDLE NO_COPY console_handle = NULL;
void
console_printf (const char *fmt,...)
{
  char buf[16384];
  va_list ap;
  DWORD done;
  int count;

  if (!console_handle)
    console_handle = CreateFileA ("CON", GENERIC_WRITE,
				  FILE_SHARE_READ | FILE_SHARE_WRITE,
				  NULL, OPEN_EXISTING, 0, 0);

  if (console_handle == INVALID_HANDLE_VALUE)
    console_handle = GetStdHandle (STD_ERROR_HANDLE);

  va_start (ap, fmt);
  count = __small_vsprintf (buf, fmt, ap);
  va_end (ap);

  WriteFile (console_handle, buf, count, &done, NULL);
  FlushFileBuffers (console_handle);
}
#endif
