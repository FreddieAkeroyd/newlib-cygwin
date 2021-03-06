/* dcrt0.cc -- essentially the main() for the Cygwin dll

   Copyright 1996, 1997, 1998, 1999, 2000, 2001, 2002, 2003 Red Hat, Inc.

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include <unistd.h>
#include <stdlib.h>
#include "glob.h"
#include "exceptions.h"
#include <ctype.h>
#include <limits.h>
#include <wingdi.h>
#include <winuser.h>
#include "sigproc.h"
#include "pinfo.h"
#include "cygerrno.h"
#define NEED_VFORK
#include "perprocess.h"
#include "security.h"
#include "fhandler.h"
#include "path.h"
#include "dtable.h"
#include "cygheap.h"
#include "child_info_magic.h"
#include "perthread.h"
#include "shared_info.h"
#include "cygwin_version.h"
#include "dll_init.h"
#include "cygthread.h"
#include "sync.h"

#define MAX_AT_FILE_LEVEL 10

#define PREMAIN_LEN (sizeof (user_data->premain) / sizeof (user_data->premain[0]))

HANDLE NO_COPY hMainProc;
HANDLE NO_COPY hMainThread;

sigthread NO_COPY mainthread;		// ID of the main thread

per_thread_waitq NO_COPY waitq_storage;
per_thread_vfork NO_COPY vfork_storage;
per_thread_signal_dispatch NO_COPY signal_dispatch_storage;

per_thread NO_COPY *threadstuff[] = {&waitq_storage,
				     &vfork_storage,
				     &signal_dispatch_storage,
				     NULL};

bool display_title;
bool strip_title_path;
bool allow_glob = TRUE;
codepage_type current_codepage = ansi_cp;

int cygwin_finished_initializing;

/* Used in SIGTOMASK for generating a bit for insertion into a sigset_t.
   This is subtracted from the signal number prior to shifting the bit.
   In older versions of cygwin, the signal was used as-is to shift the
   bit for masking.  So, we'll temporarily detect this and set it to zero
   for programs that are linked using older cygwins.  This is just a stopgap
   measure to allow an orderly transfer to the new, correct sigmask method. */
unsigned NO_COPY int signal_shift_subtract = 1;

ResourceLocks _reslock NO_COPY;
MTinterface _mtinterf;

bool NO_COPY _cygwin_testing;
unsigned NO_COPY _cygwin_testing_magic;

char NO_COPY almost_null[1];

extern "C"
{
  /* This is an exported copy of environ which can be used by DLLs
     which use cygwin.dll.  */
  char **__cygwin_environ;
  char ***main_environ;
  /* __progname used in getopt error message */
  char *__progname;
  struct _reent reent_data = _REENT_INIT(reent_data);
  struct per_process __cygwin_user_data =
  {/* initial_sp */ 0, /* magic_biscuit */ 0,
   /* dll_major */ CYGWIN_VERSION_DLL_MAJOR,
   /* dll_major */ CYGWIN_VERSION_DLL_MINOR,
   /* impure_ptr_ptr */ NULL, /* envptr */ NULL,
   /* malloc */ malloc, /* free */ free,
   /* realloc */ realloc,
   /* fmode_ptr */ NULL, /* main */ NULL, /* ctors */ NULL,
   /* dtors */ NULL, /* data_start */ NULL, /* data_end */ NULL,
   /* bss_start */ NULL, /* bss_end */ NULL,
   /* calloc */ calloc,
   /* premain */ {NULL, NULL, NULL, NULL},
   /* run_ctors_p */ 0,
   /* unused */ {0, 0, 0, 0, 0, 0, 0},
   /* forkee */ 0,
   /* hmodule */ NULL,
   /* api_major */ CYGWIN_VERSION_API_MAJOR,
   /* api_minor */ CYGWIN_VERSION_API_MINOR,
   /* unused2 */ {0, 0, 0, 0, 0},
   /* resourcelocks */ &_reslock, /* threadinterface */ &_mtinterf,
   /* impure_ptr */ &reent_data,
  };
  bool ignore_case_with_glob;
  int __declspec (dllexport) _check_for_executable = TRUE;
#ifdef DEBUGGING
  int pinger;
#endif
};

char *old_title;
char title_buf[TITLESIZE + 1];

static void
do_global_dtors (void)
{
  if (user_data->dtors)
    {
      void (**pfunc)() = user_data->dtors;
      while (*++pfunc)
	(*pfunc) ();
    }
}

static void __stdcall
do_global_ctors (void (**in_pfunc)(), int force)
{
  if (!force)
    {
      if (user_data->forkee || user_data->run_ctors_p)
	return;		// inherit constructed stuff from parent pid
      user_data->run_ctors_p = 1;
    }

  /* Run ctors backwards, so skip the first entry and find how many
     there are, then run them. */

  void (**pfunc)() = in_pfunc;

  while (*++pfunc)
    ;
  while (--pfunc > in_pfunc)
    (*pfunc) ();

  if (user_data->magic_biscuit == SIZEOF_PER_PROCESS)
    atexit (do_global_dtors);
}

/*
 * Replaces @file in the command line with the contents of the file.
 * There may be multiple @file's in a single command line
 * A \@file is replaced with @file so that echo \@foo would print
 * @foo and not the contents of foo.
 */
static int __stdcall
insert_file (char *name, char *&cmd)
{
  HANDLE f;
  DWORD size;

  f = CreateFile (name + 1,
		  GENERIC_READ,		 /* open for reading	*/
		  FILE_SHARE_READ,       /* share for reading	*/
		  &sec_none_nih,	 /* no security		*/
		  OPEN_EXISTING,	 /* existing file only	*/
		  FILE_ATTRIBUTE_NORMAL, /* normal file		*/
		  NULL);		 /* no attr. template	*/

  if (f == INVALID_HANDLE_VALUE)
    {
      debug_printf ("couldn't open file '%s', %E", name);
      return FALSE;
    }

  /* This only supports files up to about 4 billion bytes in
     size.  I am making the bold assumption that this is big
     enough for this feature */
  size = GetFileSize (f, NULL);
  if (size == 0xFFFFFFFF)
    {
      debug_printf ("couldn't get file size for '%s', %E", name);
      return FALSE;
    }

  int new_size = strlen (cmd) + size + 2;
  char *tmp = (char *) malloc (new_size);
  if (!tmp)
    {
      debug_printf ("malloc failed, %E");
      return FALSE;
    }

  /* realloc passed as it should */
  DWORD rf_read;
  BOOL rf_result;
  rf_result = ReadFile (f, tmp, size, &rf_read, NULL);
  CloseHandle (f);
  if (!rf_result || (rf_read != size))
    {
      debug_printf ("ReadFile failed, %E");
      return FALSE;
    }

  tmp[size++] = ' ';
  strcpy (tmp + size, cmd);
  cmd = tmp;
  return TRUE;
}

static inline int
isquote (char c)
{
  char ch = c;
  return ch == '"' || ch == '\'';
}

/* Step over a run of characters delimited by quotes */
static /*__inline*/ char *
quoted (char *cmd, int winshell)
{
  char *p;
  char quote = *cmd;

  if (!winshell)
    {
      char *p;
      strcpy (cmd, cmd + 1);
      if ((p = strchr (cmd, quote)) != NULL)
	strcpy (p, p + 1);
      else
	p = strchr (cmd, '\0');
      return p;
    }

  const char *s = quote == '\'' ? "'" : "\\\"";
  /* This must have been run from a Windows shell, so preserve
     quotes for globify to play with later. */
  while (*cmd && *++cmd)
    if ((p = strpbrk (cmd, s)) == NULL)
      {
	cmd = strchr (cmd, '\0');	// no closing quote
	break;
      }
    else if (*p == '\\')
      cmd = ++p;
    else if (quote == '"' && p[1] == '"')
      {
	*p = '\\';
	cmd = ++p;			// a quoted quote
      }
    else
      {
	cmd = p + 1;		// point to after end
	break;
      }
  return cmd;
}

/* Perform a glob on word if it contains wildcard characters.
   Also quote every character between quotes to force glob to
   treat the characters literally. */
static int __stdcall
globify (char *word, char **&argv, int &argc, int &argvlen)
{
  if (*word != '~' && strpbrk (word, "?*[\"\'(){}") == NULL)
    return 0;

  int n = 0;
  char *p, *s;
  int dos_spec = isdrive (word);
  if (!dos_spec && isquote (*word) && word[1] && word[2])
    dos_spec = isdrive (word + 1);

  /* We'll need more space if there are quoting characters in
     word.  If that is the case, doubling the size of the
     string should provide more than enough space. */
  if (strpbrk (word, "'\""))
    n = strlen (word);
  char pattern[strlen (word) + ((dos_spec + 1) * n) + 1];

  /* Fill pattern with characters from word, quoting any
     characters found within quotes. */
  for (p = pattern, s = word; *s != '\000'; s++, p++)
    if (!isquote (*s))
      {
	if (dos_spec && *s == '\\')
	  *p++ = '\\';
	*p = *s;
      }
    else
      {
	char quote = *s;
	while (*++s && *s != quote)
	  {
	    if (dos_spec || *s != '\\')
	      /* nothing */;
	    else if (s[1] == quote || s[1] == '\\')
	      s++;
	    *p++ = '\\';
	    *p++ = *s;
	  }
	if (*s == quote)
	  p--;
	if (*s == '\0')
	    break;
      }

  *p = '\0';

  glob_t gl;
  gl.gl_offs = 0;

  /* Attempt to match the argument.  Return just word (minus quoting) if no match. */
  if (glob (pattern, GLOB_TILDE | GLOB_NOCHECK | GLOB_BRACE | GLOB_QUOTE, NULL, &gl) || !gl.gl_pathc)
    return 0;

  /* Allocate enough space in argv for the matched filenames. */
  n = argc;
  if ((argc += gl.gl_pathc) > argvlen)
    {
      argvlen = argc + 10;
      argv = (char **) realloc (argv, (1 + argvlen) * sizeof (argv[0]));
    }

  /* Copy the matched filenames to argv. */
  char **gv = gl.gl_pathv;
  char **av = argv + n;
  while (*gv)
    {
      debug_printf ("argv[%d] = '%s'", n++, *gv);
      *av++ = *gv++;
    }

  /* Clean up after glob. */
  free (gl.gl_pathv);
  return 1;
}

/* Build argv, argc from string passed from Windows.  */

static void __stdcall
build_argv (char *cmd, char **&argv, int &argc, int winshell)
{
  int argvlen = 0;
  int nesting = 0;		// monitor "nesting" from insert_file

  argc = 0;
  argvlen = 0;
  argv = NULL;

  /* Scan command line until there is nothing left. */
  while (*cmd)
    {
      /* Ignore spaces */
      if (issep (*cmd))
	{
	  cmd++;
	  continue;
	}

      /* Found the beginning of an argument. */
      char *word = cmd;
      char *sawquote = NULL;
      while (*cmd)
	{
	  if (*cmd != '"' && (!winshell || *cmd != '\''))
	    cmd++;		// Skip over this character
	  else
	    /* Skip over characters until the closing quote */
	    {
	      sawquote = cmd;
	      cmd = quoted (cmd, winshell && argc > 0);
	    }
	  if (issep (*cmd))	// End of argument if space
	    break;
	}
      if (*cmd)
	*cmd++ = '\0';		// Terminate `word'

      /* Possibly look for @file construction assuming that this isn't
	 the very first argument and the @ wasn't quoted */
      if (argc && sawquote != word && *word == '@')
	{
	  if (++nesting > MAX_AT_FILE_LEVEL)
	    api_fatal ("Too many levels of nesting for %s", word);
	  if (insert_file (word, cmd))
	      continue;			// There's new stuff in cmd now
	}

      /* See if we need to allocate more space for argv */
      if (argc >= argvlen)
	{
	  argvlen = argc + 10;
	  argv = (char **) realloc (argv, (1 + argvlen) * sizeof (argv[0]));
	}

      /* Add word to argv file after (optional) wildcard expansion. */
      if (!winshell || !argc || !globify (word, argv, argc, argvlen))
	{
	  debug_printf ("argv[%d] = '%s'", argc, word);
	  argv[argc++] = word;
	}
    }

  argv[argc] = NULL;

  debug_printf ("argc %d", argc);
}

/* sanity and sync check */
void __stdcall
check_sanity_and_sync (per_process *p)
{
  /* Sanity check to make sure developers didn't change the per_process    */
  /* struct without updating SIZEOF_PER_PROCESS [it makes them think twice */
  /* about changing it].						   */
  if (sizeof (per_process) != SIZEOF_PER_PROCESS)
    {
      api_fatal ("per_process sanity check failed");
    }

  /* Make sure that the app and the dll are in sync. */

  /* Complain if older than last incompatible change */
  if (p->dll_major < CYGWIN_VERSION_DLL_EPOCH)
    api_fatal ("cygwin DLL and APP are out of sync -- DLL version mismatch %d < %d",
	       p->dll_major, CYGWIN_VERSION_DLL_EPOCH);

  /* magic_biscuit != 0 if using the old style version numbering scheme.  */
  if (p->magic_biscuit != SIZEOF_PER_PROCESS)
    api_fatal ("Incompatible cygwin .dll -- incompatible per_process info %d != %d",
	       p->magic_biscuit, SIZEOF_PER_PROCESS);

  /* Complain if incompatible API changes made */
  if (p->api_major != cygwin_version.api_major)
    api_fatal ("cygwin DLL and APP are out of sync -- API version mismatch %d < %d",
	       p->api_major, cygwin_version.api_major);

  if (CYGWIN_VERSION_DLL_MAKE_COMBINED (p->dll_major, p->dll_minor) <=
      CYGWIN_VERSION_DLL_BAD_SIGNAL_MASK)
    signal_shift_subtract = 0;
}

child_info NO_COPY *child_proc_info = NULL;
static MEMORY_BASIC_INFORMATION NO_COPY sm;

#define CYGWIN_GUARD ((wincap.has_page_guard ()) ? \
		     PAGE_EXECUTE_READWRITE|PAGE_GUARD : PAGE_NOACCESS)

// __inline__ void
extern void
alloc_stack_hard_way (child_info_fork *ci, volatile char *b)
{
  void *new_stack_pointer;
  MEMORY_BASIC_INFORMATION m;
  void *newbase;
  int newlen;
  LPBYTE curbot = (LPBYTE) sm.BaseAddress + sm.RegionSize;
  bool noguard;

  if (ci->stacktop > (LPBYTE) sm.AllocationBase && ci->stacktop < curbot)
    {
      newbase = curbot;
      newlen = (LPBYTE) ci->stackbottom - (LPBYTE) curbot;
      noguard = 1;
    }
  else
    {
      newbase = ci->stacktop;
      newlen = (DWORD) ci->stackbottom - (DWORD) ci->stacktop;
      noguard = 0;
    }
  if (!VirtualAlloc (newbase, newlen, MEM_RESERVE, PAGE_NOACCESS))
    api_fatal ("fork: can't reserve memory for stack %p - %p, %E",
		ci->stacktop, ci->stackbottom);

  new_stack_pointer = (void *) ((LPBYTE) ci->stackbottom - ci->stacksize);

  if (!VirtualAlloc (new_stack_pointer, ci->stacksize, MEM_COMMIT,
		     PAGE_EXECUTE_READWRITE))
    api_fatal ("fork: can't commit memory for stack %p(%d), %E",
	       new_stack_pointer, ci->stacksize);
  if (!VirtualQuery ((LPCVOID) new_stack_pointer, &m, sizeof m))
    api_fatal ("fork: couldn't get new stack info, %E");
  if (!noguard)
    {
      m.BaseAddress = (LPVOID)((DWORD)m.BaseAddress - 1);
      if (!VirtualAlloc ((LPVOID) m.BaseAddress, 1, MEM_COMMIT,
			 CYGWIN_GUARD))
	api_fatal ("fork: couldn't allocate new stack guard page %p, %E",
		   m.BaseAddress);
    }
  if (!VirtualQuery ((LPCVOID) m.BaseAddress, &m, sizeof m))
    api_fatal ("fork: couldn't get new stack info, %E");
  ci->stacktop = m.BaseAddress;
  *b = 0;
}

/* extend the stack prior to fork longjmp */

static void
alloc_stack (child_info_fork *ci)
{
  /* FIXME: adding 16384 seems to avoid a stack copy problem during
     fork on Win95, but I don't know exactly why yet. DJ */
  volatile char b[ci->stacksize + 16384];

  if (!VirtualQuery ((LPCVOID) &b, &sm, sizeof sm))
    api_fatal ("fork: couldn't get stack info, %E");

  if (sm.AllocationBase != ci->stacktop)
    alloc_stack_hard_way (ci, b + sizeof (b) - 1);
  else
    ci->stacksize = 0;

  return;
}

static NO_COPY int mypid = 0;
int _declspec(dllexport) __argc;
char _declspec(dllexport) **__argv;
vfork_save NO_COPY *main_vfork = NULL;

void
sigthread::init (const char *s)
{
  InitializeCriticalSection (&lock);
  id = GetCurrentThreadId ();
}

/* Take over from libc's crt0.o and start the application. Note the
   various special cases when Cygwin DLL is being runtime loaded (as
   opposed to being link-time loaded by Cygwin apps) from a non
   cygwin app via LoadLibrary.  */
static void
dll_crt0_1 ()
{
  /* According to onno@stack.urc.tue.nl, the exception handler record must
     be on the stack.  */
  /* FIXME: Verify forked children get their exception handler set up ok. */
  exception_list cygwin_except_entry;

  /* Initialize SIGSEGV handling, etc. */
  init_exceptions (&cygwin_except_entry);

  /* Set the os_being_run global. */
  wincap.init ();
  check_sanity_and_sync (user_data);

  do_global_ctors (&__CTOR_LIST__, 1);

  /* Nasty static stuff needed by newlib -- point to a local copy of
     the reent stuff.
     Note: this MUST be done here (before the forkee code) as the
     fork copy code doesn't copy the data in libccrt0.cc (that's why we
     pass in the per_process struct into the .dll from libccrt0). */

  _impure_ptr = &reent_data;

  user_data->resourcelocks->Init ();
  user_data->threadinterface->Init ();

  mainthread.init ("mainthread"); // For use in determining if signals
				  //  should be blocked.

  winpids::init ();

  int envc = 0;
  char **envp = NULL;

  if (!child_proc_info)
    memory_init ();
  else
    {
      bool close_ppid_handle = false;
      bool close_hexec_proc = false;
      switch (child_proc_info->type)
	{
	  case _PROC_FORK:
	    alloc_stack (fork_info);
	    cygheap_fixup_in_child (0);
	    memory_init ();
	    set_myself (mypid);
	    close_ppid_handle = !!child_proc_info->pppid_handle;
	    break;
	  case _PROC_SPAWN:
	    /* Have to delay closes until after cygheap is setup */
	    close_hexec_proc = !!spawn_info->hexec_proc;
	    close_ppid_handle = !!child_proc_info->pppid_handle;
	    goto around;
	  case _PROC_EXEC:
	    hexec_proc = spawn_info->hexec_proc;
	  around:
	    HANDLE h;
	    cygheap_fixup_in_child (1);
	    memory_init ();
	    if (!spawn_info->moreinfo->myself_pinfo ||
		!DuplicateHandle (hMainProc, spawn_info->moreinfo->myself_pinfo,
				  hMainProc, &h, 0, 0,
				  DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE))
	      h = NULL;
	    set_myself (mypid, h);
	    __argc = spawn_info->moreinfo->argc;
	    __argv = spawn_info->moreinfo->argv;
	    envp = spawn_info->moreinfo->envp;
	    envc = spawn_info->moreinfo->envc;
	    cygheap->fdtab.fixup_after_exec (spawn_info->parent);
	    signal_fixup_after_exec ();
	    CloseHandle (spawn_info->parent);
	    if (spawn_info->moreinfo->old_title)
	      {
		old_title = strcpy (title_buf, spawn_info->moreinfo->old_title);
		cfree (spawn_info->moreinfo->old_title);
	      }
	    break;
	}
      if (close_hexec_proc)
	CloseHandle (spawn_info->hexec_proc);
      if (close_ppid_handle)
	CloseHandle (child_proc_info->pppid_handle);
    }

  ProtectHandle (hMainProc);
  ProtectHandle (hMainThread);
  cygthread::init ();

  /* Initialize pthread mainthread when not forked and it is save to call new,
     otherwise it is reinitalized in fixup_after_fork */
  if (!user_data->forkee)
    pthread::init_mainthread ();

#ifdef DEBUGGING
  strace.microseconds ();
#endif

  /* Initialize debug muto, if DLL is built with --enable-debugging.
     Need to do this before any helper threads start. */
  debug_init ();

  cygheap->fdtab.vfork_child_fixup ();

  (void) SetErrorMode (SEM_FAILCRITICALERRORS);

  /* Initialize events. */
  events_init ();

  cygheap->cwd.init ();
  main_vfork = vfork_storage.create ();

  cygbench ("pre-forkee");
  if (user_data->forkee)
    {
      /* If we've played with the stack, stacksize != 0.  That means that
	 fork() was invoked from other than the main thread.  Make sure that
	 frame pointer is referencing the new stack so that the OS knows what
	 to do when it needs to increase the size of the stack.

	 NOTE: Don't do anything that involves the stack until you've completed
	 this step. */
      if (fork_info->stacksize)
	{
	  asm ("movl %0,%%fs:4" : : "r" (fork_info->stackbottom));
	  asm ("movl %0,%%fs:8" : : "r" (fork_info->stacktop));
	}

      longjmp (fork_info->jmp, fork_info->cygpid);
    }

#ifdef DEBUGGING
  {
  extern void fork_init ();
  fork_init ();
  }
#endif

  /* Initialize our process table entry. */
  pinfo_init (envp, envc);

  if (!old_title && GetConsoleTitle (title_buf, TITLESIZE))
      old_title = title_buf;

  /* Allocate cygheap->fdtab */
  dtable_init ();

  /* Init global well known SID objects */
  cygsid::init ();

  /* Initialize user info. */
  uinfo_init ();

  /* Initialize signal/subprocess handling. */
  sigproc_init ();

  /* Connect to tty. */
  tty_init ();

  if (!__argc)
    {
      char *line = GetCommandLineA ();
      line = strcpy ((char *) alloca (strlen (line) + 1), line);

      if (current_codepage == oem_cp)
	CharToOemA (line, line);

      /* Scan the command line and build argv.  Expand wildcards if not
	 called from another cygwin process. */
      build_argv (line, __argv, __argc,
		  NOTSTATE (myself, PID_CYGPARENT) && allow_glob);

      /* Convert argv[0] to posix rules if it's currently blatantly
	 win32 style. */
      if ((strchr (__argv[0], ':')) || (strchr (__argv[0], '\\')))
	{
	  char *new_argv0 = (char *) alloca (MAX_PATH);
	  cygwin_conv_to_posix_path (__argv[0], new_argv0);
	  __argv[0] = new_argv0;
	}
    }

  if (user_data->premain[0])
    for (unsigned int i = 0; i < PREMAIN_LEN / 2; i++)
      user_data->premain[i] (__argc, __argv, user_data);

  /* Set up standard fds in file descriptor table. */
  cygheap->fdtab.stdio_init ();

  /* Set up __progname for getopt error call. */
  if (__argv[0] && (__progname = strrchr (__argv[0], '/')))
    ++__progname;
  else
    __progname = __argv[0];
  if (__progname)
    {
      char *cp = strchr (__progname, '\0') - 4;
      if (cp > __progname && strcasematch (cp, ".exe"))
	*cp = '\0';
    }

  /* Set new console title if appropriate. */

  if (display_title && !dynamically_loaded)
    {
      char *cp = __progname;
      if (strip_title_path)
	for (char *ptr = cp; *ptr && *ptr != ' '; ptr++)
	  if (isdirsep (*ptr))
	    cp = ptr + 1;
      set_console_title (cp);
    }

  cygwin_finished_initializing = 1;
  /* Call init of loaded dlls. */
  dlls.init ();

  /* Execute any specified "premain" functions */
  if (user_data->premain[PREMAIN_LEN / 2])
    for (unsigned int i = PREMAIN_LEN / 2; i < PREMAIN_LEN; i++)
      user_data->premain[i] (__argc, __argv, user_data);

  debug_printf ("user_data->main %p", user_data->main);

  if (dynamically_loaded)
    {
      set_errno (0);
      return;
    }

  /* Disable case-insensitive globbing */
  ignore_case_with_glob = FALSE;

  /* Flush signals and ensure that signal thread is up and running. Can't
     do this for noncygwin case since the signal thread is blocked due to
     LoadLibrary serialization. */
  wait_for_sigthread ();

  set_errno (0);

  MALLOC_CHECK;
  cygbench (__progname);
  if (user_data->main)
    exit (user_data->main (__argc, __argv, *user_data->envptr));
}

#ifdef DEBUGGING
void
break_here ()
{
  debug_printf ("break here");
}
#endif

void
initial_env ()
{
  DWORD len;
  char buf[MAX_PATH + 1];
#ifdef DEBUGGING
  if (GetEnvironmentVariable ("CYGWIN_SLEEP", buf, sizeof (buf) - 1))
    {
      DWORD ms = atoi (buf);
      buf[0] = '\0';
      len = GetModuleFileName (NULL, buf, MAX_PATH);
      console_printf ("Sleeping %d, pid %u %s\n", ms, GetCurrentProcessId (), buf);
      Sleep (ms);
    }
  if (GetEnvironmentVariable ("CYGWIN_DEBUG", buf, sizeof (buf) - 1))
    {
      char buf1[MAX_PATH + 1];
      len = GetModuleFileName (NULL, buf1, MAX_PATH);
      strlwr (buf1);
      strlwr (buf);
      char *p = strchr (buf, ':');
      if (!p)
	p = (char *) "gdb.exe -nw";
      else
	*p++ = '\0';
      if (strstr (buf1, buf))
	{
	  error_start_init (p);
	  try_to_debug ();
	  break_here ();
	}
    }
#endif

  if (GetEnvironmentVariable ("CYGWIN_TESTING", buf, sizeof (buf) - 1))
    {
      _cygwin_testing = 1;
      if ((len = GetModuleFileName (cygwin_hmodule, buf, MAX_PATH))
	  && len > sizeof ("new-cygwin1.dll")
	  && strcasematch (buf + len - sizeof ("new-cygwin1.dll"),
			   "\\new-cygwin1.dll"))
	_cygwin_testing_magic = 0x10;
    }
}

/* Wrap the real one, otherwise gdb gets confused about
   two symbols with the same name, but different addresses.

   UPTR is a pointer to global data that lives on the libc side of the
   line [if one distinguishes the application from the dll].  */

extern "C" void __stdcall
_dll_crt0 ()
{
  DECLARE_TLS_STORAGE;
  initial_env ();
  char zeros[sizeof (fork_info->zero)] = {0};
  static NO_COPY STARTUPINFO si;

  main_environ = user_data->envptr;
  *main_environ = NULL;

  early_stuff_init ();
  if (!DuplicateHandle (GetCurrentProcess (), GetCurrentProcess (),
		       GetCurrentProcess (), &hMainProc, 0, FALSE,
			DUPLICATE_SAME_ACCESS))
    hMainProc = GetCurrentProcess ();

  DuplicateHandle (hMainProc, GetCurrentThread (), hMainProc,
		   &hMainThread, 0, false, DUPLICATE_SAME_ACCESS);

  GetStartupInfo (&si);
  child_proc_info = (child_info *) si.lpReserved2;
  if (si.cbReserved2 < EXEC_MAGIC_SIZE || !child_proc_info
      || memcmp (child_proc_info->zero, zeros, sizeof (zeros)) != 0)
    child_proc_info = NULL;
  else
    {
      if ((child_proc_info->intro & OPROC_MAGIC_MASK) == OPROC_MAGIC_GENERIC)
	multiple_cygwin_problem ("proc", child_proc_info->intro, 0);
      else if (child_proc_info->intro == PROC_MAGIC_GENERIC
	       && child_proc_info->magic != CHILD_INFO_MAGIC)
	multiple_cygwin_problem ("proc", child_proc_info->magic,
				 CHILD_INFO_MAGIC);
      else if (child_proc_info->cygheap != (void *) &_cygheap_start)
	multiple_cygwin_problem ("cygheap", (DWORD) child_proc_info->cygheap,
				 (DWORD) &_cygheap_start);
      unsigned should_be_cb = 0;
      switch (child_proc_info->type)
	{
	  case _PROC_FORK:
	    user_data->forkee = child_proc_info->cygpid;
	    should_be_cb = sizeof (child_info_fork);
	    /* fall through */;
	  case _PROC_SPAWN:
	  case _PROC_EXEC:
	    if (!should_be_cb)
	      should_be_cb = sizeof (child_info);
	    if (should_be_cb != child_proc_info->cb)
	      multiple_cygwin_problem ("proc size", child_proc_info->cb, should_be_cb);
	    else if (sizeof (fhandler_union) != child_proc_info->fhandler_union_cb)
	      multiple_cygwin_problem ("fhandler size", child_proc_info->fhandler_union_cb, sizeof (fhandler_union));
	    else
	      {
		cygwin_mount_h = child_proc_info->mount_h;
		mypid = child_proc_info->cygpid;
		break;
	      }
	  default:
	    system_printf ("unknown exec type %d", child_proc_info->type);
	    /* intentionally fall through */
	  case _PROC_WHOOPS:
	    child_proc_info = NULL;
	    break;
	}
    }
  dll_crt0_1 ();
}

void
dll_crt0 (per_process *uptr)
{
  DECLARE_TLS_STORAGE;
  /* Set the local copy of the pointer into the user space. */
  if (uptr && uptr != user_data)
    {
      memcpy (user_data, uptr, per_process_overwrite);
      *(user_data->impure_ptr_ptr) = &reent_data;
    }
  _dll_crt0 ();
}

/* This must be called by anyone who uses LoadLibrary to load cygwin1.dll */
extern "C" void
cygwin_dll_init ()
{
  static char **envp;
  static int _fmode;

  if (!DuplicateHandle (GetCurrentProcess (), GetCurrentProcess (),
		       GetCurrentProcess (), &hMainProc, 0, FALSE,
			DUPLICATE_SAME_ACCESS))
    hMainProc = GetCurrentProcess ();

  DuplicateHandle (hMainProc, GetCurrentThread (), hMainProc,
		   &hMainThread, 0, FALSE, DUPLICATE_SAME_ACCESS);
  user_data->magic_biscuit = sizeof (per_process);

  user_data->envptr = &envp;
  user_data->fmode_ptr = &_fmode;

  dll_crt0_1 ();
}

extern "C" void
__main (void)
{
  do_global_ctors (user_data->ctors, FALSE);
}

enum exit_states
  {
    ES_NOT_EXITING = 0,
    ES_THREADTERM,
    ES_SIGNAL,
    ES_CLOSEALL,
    ES_SIGPROCTERMINATE,
    ES_TITLE,
    ES_HUP_PGRP,
    ES_HUP_SID,
    ES_TTY_TERMINATE,
    ES_EVENTS_TERMINATE
  };

exit_states NO_COPY exit_state;
extern CRITICAL_SECTION exit_lock;

extern "C" void __stdcall
do_exit (int status)
{
  EnterCriticalSection (&exit_lock);
  UINT n = (UINT) status;

  syscall_printf ("do_exit (%d)", n);

  vfork_save *vf = vfork_storage.val ();
  if (vf != NULL && vf->pid < 0)
    vf->restore_exit (status);

  if (exit_state < ES_THREADTERM)
    {
      exit_state = ES_THREADTERM;
      cygthread::terminate ();
    }

  if (exit_state < ES_SIGNAL)
    {
      exit_state = ES_SIGNAL;
      if (!(n & EXIT_REPARENTING))
	{
	  signal (SIGCHLD, SIG_IGN);
	  signal (SIGHUP, SIG_IGN);
	  signal (SIGINT, SIG_IGN);
	  signal (SIGQUIT, SIG_IGN);
	}
    }

  if (exit_state < ES_CLOSEALL)
    {
      exit_state = ES_CLOSEALL;
      close_all_files ();
    }

  if (exit_state < ES_SIGPROCTERMINATE)
    {
      exit_state = ES_SIGPROCTERMINATE;
      sigproc_terminate ();
    }

  myself->stopsig = 0;
  if (exit_state < ES_TITLE)
    {
      exit_state = ES_TITLE;
      /* restore console title */
      if (old_title && display_title)
	set_console_title (old_title);
    }

  if (exit_state < ES_HUP_PGRP)
    {
      exit_state = ES_HUP_PGRP;
      /* Kill orphaned children on group leader exit */
      if (myself->has_pgid_children && myself->pid == myself->pgid)
	{
	  sigproc_printf ("%d == pgrp %d, send SIG{HUP,CONT} to stopped children",
			  myself->pid, myself->pgid);
	  kill_pgrp (myself->pgid, -SIGHUP);
	}
    }

  if (exit_state < ES_HUP_SID)
    {
      exit_state = ES_HUP_SID;
      /* Kill the foreground process group on session leader exit */
      if (getpgrp () > 0 && myself->pid == myself->sid && real_tty_attached (myself))
	{
	  tty *tp = cygwin_shared->tty[myself->ctty];
	  sigproc_printf ("%d == sid %d, send SIGHUP to children",
			  myself->pid, myself->sid);

	/* CGF FIXME: This can't be right. */
	  if (tp->getsid () == myself->sid)
	    tp->kill_pgrp (SIGHUP);
	}

    }

  if (exit_state < ES_TTY_TERMINATE)
    {
      exit_state = ES_TTY_TERMINATE;
      tty_terminate ();
    }

  if (exit_state < ES_EVENTS_TERMINATE)
    {
      exit_state = ES_EVENTS_TERMINATE;
      events_terminate ();
    }

  minimal_printf ("winpid %d, exit %d", GetCurrentProcessId (), n);
  myself->exit (n);
}

static muto *atexit_lock;

extern "C" int
cygwin_atexit (void (*function)(void))
{
  int res;
  if (!atexit_lock)
    new_muto (atexit_lock);
  atexit_lock->acquire ();
  res = atexit (function);
  atexit_lock->release ();
  return res;
}

extern "C" void
cygwin_exit (int n)
{
  if (atexit_lock)
    atexit_lock->acquire ();
  exit (n);
}

extern "C" void
_exit (int n)
{
  do_exit ((DWORD) n & 0xffff);
}

extern "C" void
__api_fatal (const char *fmt, ...)
{
  char buf[4096];
  va_list ap;

  va_start (ap, fmt);
  __small_vsprintf (buf, fmt, ap);
  va_end (ap);
  strcat (buf, "\n");
  int len = strlen (buf);
  DWORD done;
  (void) WriteFile (GetStdHandle (STD_ERROR_HANDLE), buf, len, &done, 0);

  /* Make sure that the message shows up on the screen, too, since this is
     a serious error. */
  if (GetFileType (GetStdHandle (STD_ERROR_HANDLE)) != FILE_TYPE_CHAR)
    {
      HANDLE h = CreateFile ("CONOUT$", GENERIC_READ | GENERIC_WRITE,
			     FILE_SHARE_WRITE | FILE_SHARE_WRITE,
			     &sec_none, OPEN_EXISTING, 0, 0);
      if (h != INVALID_HANDLE_VALUE)
	(void) WriteFile (h, buf, len, &done, 0);
    }

  /* We are going down without mercy.  Make sure we reset
     our process_state. */
  sigproc_terminate ();
#ifdef DEBUGGING
  (void) try_to_debug ();
#endif
  myself->exit (1);
}

void
multiple_cygwin_problem (const char *what, unsigned magic_version, unsigned version)
{
  if (_cygwin_testing && (strstr (what, "proc") || strstr (what, "cygheap")))
    {
      child_proc_info->type = _PROC_WHOOPS;
      return;
    }

  char buf[1024];
  if (GetEnvironmentVariable ("CYGWIN_MISMATCH_OK", buf, sizeof (buf)))
    return;

  if (CYGWIN_VERSION_MAGIC_VERSION (magic_version) == version)
    system_printf ("%s magic number mismatch detected - %p/%p", what, magic_version, version);
  else
    api_fatal ("%s version mismatch detected - %p/%p.\n\
You have multiple copies of cygwin1.dll on your system.\n\
Search for cygwin1.dll using the Windows Start->Find/Search facility\n\
and delete all but the most recent version.  The most recent version *should*\n\
reside in x:\\cygwin\\bin, where 'x' is the drive on which you have\n\
installed the cygwin distribution.", what, magic_version, version);
}

#ifdef DEBUGGING
void __stdcall
cygbench (const char *s)
{
  char buf[1024];
  if (GetEnvironmentVariable ("CYGWIN_BENCH", buf, sizeof (buf)))
    small_printf ("%05d ***** %s : %10d\n", GetCurrentProcessId (), s, strace.microseconds ());
}
#endif
