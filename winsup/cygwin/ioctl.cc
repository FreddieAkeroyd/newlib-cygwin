/* ioctl.cc: ioctl routines.

   Copyright 1996, 1998, 1999, 2000, 2001, 2002 Red Hat, Inc.

   Written by Doug Evans of Cygnus Support
   dje@cygnus.com

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include <sys/ioctl.h>
#include "cygerrno.h"
#include "security.h"
#include "fhandler.h"
#include "path.h"
#include "dtable.h"
#include "cygheap.h"
#include "sigproc.h"
#include <sys/termios.h>

extern "C" int
ioctl (int fd, int cmd, ...)
{
  sigframe thisframe (mainthread);

  cygheap_fdget cfd (fd);
  if (cfd < 0)
    return -1;

  /* check for optional mode argument */
  va_list ap;
  va_start (ap, cmd);
  char *argp = va_arg (ap, char *);
  va_end (ap);

  debug_printf ("fd %d, cmd %x", fd, cmd);
  int res;
  if (cfd->is_tty () && cfd->get_device () != FH_PTYM)
    switch (cmd)
      {
	case TCGETA:
	  res = tcgetattr (fd, (struct termios *) argp);
	  goto out;
	case TCSETA:
	  res = tcsetattr (fd, TCSANOW, (struct termios *) argp);
	  goto out;
	case TCSETAW:
	  res = tcsetattr (fd, TCSADRAIN, (struct termios *) argp);
	  goto out;
	case TCSETAF:
	  res = tcsetattr (fd, TCSAFLUSH, (struct termios *) argp);
	  goto out;
      }

  res = cfd->ioctl (cmd, argp);

out:
  debug_printf ("returning %d", res);
  return res;
}
