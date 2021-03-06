/* environ.cc: Cygwin-adopted functions from newlib to manipulate
   process's environment.

   Copyright 1997, 1998, 1999, 2000, 2001, 2002, 2003 Red Hat, Inc.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "winsup.h"
#include <stdlib.h>
#include <stddef.h>
#include <ctype.h>
#include <assert.h>
#include <sys/cygwin.h>
#include <cygwin/version.h>
#include "pinfo.h"
#include "perprocess.h"
#include "security.h"
#include "fhandler.h"
#include "path.h"
#include "cygerrno.h"
#include "dtable.h"
#include "cygheap.h"
#include "registry.h"
#include "environ.h"
#include "child_info.h"

extern bool allow_glob;
extern bool ignore_case_with_glob;
extern bool allow_ntea;
extern bool allow_smbntsec;
extern bool allow_winsymlinks;
extern bool strip_title_path;
extern int pcheck_case;
extern int subauth_id;
bool reset_com = false;
static bool envcache = true;
#ifdef USE_SERVER
extern bool allow_server;
#endif

static char **lastenviron;

#define ENVMALLOC \
  (CYGWIN_VERSION_DLL_MAKE_COMBINED (user_data->api_major, user_data->api_minor) \
	  <= CYGWIN_VERSION_DLL_MALLOC_ENV)

#define NL(x) x, (sizeof (x) - 1)
/* List of names which are converted from dos to unix
   on the way in and back again on the way out.

   PATH needs to be here because CreateProcess uses it and gdb uses
   CreateProcess.  HOME is here because most shells use it and would be
   confused by Windows style path names.  */
static int return_MAX_PATH (const char *) {return MAX_PATH;}
static NO_COPY win_env conv_envvars[] =
  {
    {NL ("PATH="), NULL, NULL, cygwin_win32_to_posix_path_list,
     cygwin_posix_to_win32_path_list,
     cygwin_win32_to_posix_path_list_buf_size,
     cygwin_posix_to_win32_path_list_buf_size},
    {NL ("HOME="), NULL, NULL, cygwin_conv_to_full_posix_path,
     cygwin_conv_to_full_win32_path, return_MAX_PATH, return_MAX_PATH},
    {NL ("LD_LIBRARY_PATH="), NULL, NULL, cygwin_conv_to_full_posix_path,
     cygwin_conv_to_full_win32_path, return_MAX_PATH, return_MAX_PATH},
    {NL ("TMPDIR="), NULL, NULL, cygwin_conv_to_full_posix_path,
     cygwin_conv_to_full_win32_path, return_MAX_PATH, return_MAX_PATH},
    {NL ("TMP="), NULL, NULL, cygwin_conv_to_full_posix_path,
     cygwin_conv_to_full_win32_path, return_MAX_PATH, return_MAX_PATH},
    {NL ("TEMP="), NULL, NULL, cygwin_conv_to_full_posix_path,
     cygwin_conv_to_full_win32_path, return_MAX_PATH, return_MAX_PATH},
    {NULL, 0, NULL, NULL, NULL, NULL, 0, 0}
  };

static unsigned char conv_start_chars[256] = {0};

void
win_env::add_cache (const char *in_posix, const char *in_native)
{
  MALLOC_CHECK;
  posix = (char *) realloc (posix, strlen (in_posix) + 1);
  strcpy (posix, in_posix);
  if (in_native)
    {
      native = (char *) realloc (native, namelen + 1 + strlen (in_native));
      (void) strcpy (native, name);
      (void) strcpy (native + namelen, in_native);
    }
  else
    {
      native = (char *) realloc (native, namelen + 1 + win32_len (in_posix));
      (void) strcpy (native, name);
      towin32 (in_posix, native + namelen);
    }
  MALLOC_CHECK;
  debug_printf ("posix %s", posix);
  debug_printf ("native %s", native);
}


/* Check for a "special" environment variable name.  *env is the pointer
  to the beginning of the environment variable name.  *in_posix is any
  known posix value for the environment variable. Returns a pointer to
  the appropriate conversion structure.  */
win_env * __stdcall
getwinenv (const char *env, const char *in_posix)
{
  if (!conv_start_chars[(unsigned char)*env])
    return NULL;

  for (int i = 0; conv_envvars[i].name != NULL; i++)
    if (strncmp (env, conv_envvars[i].name, conv_envvars[i].namelen) == 0)
      {
	win_env * const we = conv_envvars + i;
	const char *val;
	if (!cur_environ () || !(val = in_posix ?: getenv (we->name)))
	  debug_printf ("can't set native for %s since no environ yet",
			we->name);
	else if (!envcache || !we->posix || strcmp (val, we->posix) != 0)
	      we->add_cache (val);
	return we;
      }
  return NULL;
}

/* Convert windows path specs to POSIX, if appropriate.
 */
static void __stdcall
posify (char **here, const char *value)
{
  char *src = *here;
  win_env *conv;

  if (!(conv = getwinenv (src)))
    return;

  int len = strcspn (src, "=") + 1;

  /* Turn all the items from c:<foo>;<bar> into their
     mounted equivalents - if there is one.  */

  char *outenv = (char *) malloc (1 + len + conv->posix_len (value));
  memcpy (outenv, src, len);
  conv->toposix (value, outenv + len);
  conv->add_cache (outenv + len, *value != '/' ? value : NULL);

  debug_printf ("env var converted to %s", outenv);
  *here = outenv;
  free (src);
  MALLOC_CHECK;
}

/*
 * my_findenv --
 *	Returns pointer to value associated with name, if any, else NULL.
 *	Sets offset to be the offset of the name/value combination in the
 *	environment array, for use by setenv(3) and unsetenv(3).
 *	Explicitly removes '=' in argument name.
 */

static char * __stdcall
my_findenv (const char *name, int *offset)
{
  register int len;
  register char **p;
  const char *c;

  c = name;
  len = 0;
  while (*c && *c != '=')
    {
      c++;
      len++;
    }

  for (p = cur_environ (); *p; ++p)
    if (!strncmp (*p, name, len))
      if (*(c = *p + len) == '=')
	{
	  *offset = p - cur_environ ();
	  return (char *) (++c);
	}
  MALLOC_CHECK;
  return NULL;
}

/*
 * getenv --
 *	Returns ptr to value associated with name, if any, else NULL.
 */

extern "C" char *
getenv (const char *name)
{
  int offset;

  return my_findenv (name, &offset);
}

static int __stdcall
envsize (const char * const *in_envp)
{
  const char * const *envp;
  for (envp = in_envp; *envp; envp++)
    continue;
  return (1 + envp - in_envp) * sizeof (const char *);
}

/* Takes similar arguments to setenv except that overwrite is
   either -1, 0, or 1.  0 or 1 signify that the function should
   perform similarly to setenv.  Otherwise putenv is assumed. */
static int __stdcall
_addenv (const char *name, const char *value, int overwrite)
{
  int issetenv = overwrite >= 0;
  int offset;
  char *p;

  unsigned int valuelen = strlen (value);
  if ((p = my_findenv (name, &offset)))
    {				/* Already exists. */
      if (!overwrite)		/* Ok to overwrite? */
	return 0;		/* No.  Wanted to add new value.  FIXME: Right return value? */

      /* We've found the offset into environ.  If this is a setenv call and
	 there is room in the current environment entry then just overwrite it.
	 Otherwise handle this case below. */
      if (issetenv && strlen (p) >= valuelen)
	{
	  strcpy (p, value);
	  return 0;
	}
    }
  else
    {				/* Create new slot. */
      int sz = envsize (cur_environ ());
      int allocsz = sz + (2 * sizeof (char *));

      offset = (sz - 1) / sizeof (char *);

      /* Allocate space for additional element plus terminating NULL. */
      if (cur_environ () == lastenviron)
	lastenviron = __cygwin_environ = (char **) realloc (cur_environ (),
							    allocsz);
      else if ((lastenviron = (char **) malloc (allocsz)) != NULL)
	__cygwin_environ = (char **) memcpy ((char **) lastenviron,
					     __cygwin_environ, sz);

      if (!__cygwin_environ)
	{
#ifdef DEBUGGING
	  try_to_debug ();
#endif
	  return -1;				/* Oops.  No more memory. */
	}

      __cygwin_environ[offset + 1] = NULL;	/* NULL terminate. */
      update_envptrs ();	/* Update any local copies of 'environ'. */
    }

  char *envhere;
  if (!issetenv)
    /* Not setenv. Just overwrite existing. */
    envhere = cur_environ ()[offset] = (char *) (ENVMALLOC ? strdup (name) : name);
  else
    {				/* setenv */
      /* Look for an '=' in the name and ignore anything after that if found. */
      for (p = (char *) name; *p && *p != '='; p++)
	continue;

      int namelen = p - name;	/* Length of name. */
      /* Allocate enough space for name + '=' + value + '\0' */
      envhere = cur_environ ()[offset] = (char *) malloc (namelen + valuelen + 2);
      if (!envhere)
	return -1;		/* Oops.  No more memory. */

      /* Put name '=' value into current slot. */
      strncpy (envhere, name, namelen);
      envhere[namelen] = '=';
      strcpy (envhere + namelen + 1, value);
    }

  /* Update cygwin's cache, if appropriate */
  win_env *spenv;
  if ((spenv = getwinenv (envhere)))
    spenv->add_cache (value);

  MALLOC_CHECK;
  return 0;
}

/* putenv Sets an environment variable */
extern "C" int
putenv (const char *str)
{
  int res;
  if ((res = check_null_empty_str (str)))
    {
      if (res == ENOENT)
	return 0;
      set_errno (res);
      return  -1;
    }
  char *eq = strchr (str, '=');
  if (eq)
    return _addenv (str, eq + 1, -1);

  /* Remove str from the environment. */
  unsetenv (str);
  return 0;
}

/* setenv -- Set the value of the environment variable "name" to be
   "value".  If overwrite is set, replace any current value.  */
extern "C" int
setenv (const char *name, const char *value, int overwrite)
{
  int res;
  if ((res = check_null_empty_str (value)) == EFAULT)
    {
      set_errno (res);
      return  -1;
    }
  if ((res = check_null_empty_str (name)))
    {
      if (res == ENOENT)
	return 0;
      set_errno (res);
      return  -1;
    }
  if (*value == '=')
    value++;
  return _addenv (name, value, !!overwrite);
}

/* unsetenv(name) -- Delete environment variable "name".  */
extern "C" void
unsetenv (const char *name)
{
  register char **e;
  int offset;

  while (my_findenv (name, &offset))	/* if set multiple times */
    /* Move up the rest of the array */
    for (e = cur_environ () + offset; ; e++)
      if (!(*e = *(e + 1)))
	break;
}

/* Turn environment variable part of a=b string into uppercase. */
static __inline__ void
ucenv (char *p, char *eq)
{
  /* Amazingly, NT has a case sensitive environment name list,
     but only sometimes.
     It's normal to have NT set your "Path" to something.
     Later, you set "PATH" to something else.  This alters "Path".
     But if you try and do a naive getenv on "PATH" you'll get nothing.

     So we upper case the labels here to prevent confusion later but
     we only do it for the first process in a session group. */
  for (; p < eq; p++)
    if (islower (*p))
      *p = cyg_toupper (*p);
}

/* Parse CYGWIN options */

static NO_COPY bool export_settings = false;

enum settings
  {
    justset,
    isfunc,
    setbit,
    set_process_state,
  };

/* When BUF is:
   null or empty: disables globbing
   "ignorecase": enables case-insensitive globbing
   anything else: enables case-sensitive globbing */
static void
glob_init (const char *buf)
{
  if (!buf || !*buf)
    {
      allow_glob = false;
      ignore_case_with_glob = false;
    }
  else if (strncasematch (buf, "ignorecase", 10))
    {
      allow_glob = true;
      ignore_case_with_glob = true;
    }
  else
    {
      allow_glob = true;
      ignore_case_with_glob = false;
    }
}

static void
check_case_init (const char *buf)
{
  if (!buf || !*buf)
    return;

  if (strncasematch (buf, "relax", 5))
    {
      pcheck_case = PCHECK_RELAXED;
      debug_printf ("File case checking set to RELAXED");
    }
  else if (strcasematch (buf, "adjust"))
    {
      pcheck_case = PCHECK_ADJUST;
      debug_printf ("File case checking set to ADJUST");
    }
  else if (strcasematch (buf, "strict"))
    {
      pcheck_case = PCHECK_STRICT;
      debug_printf ("File case checking set to STRICT");
    }
  else
    {
      debug_printf ("Wrong case checking name: %s", buf);
    }
}

void
set_file_api_mode (codepage_type cp)
{
  if (cp == oem_cp)
    {
      SetFileApisToOEM ();
      debug_printf ("File APIs set to OEM");
    }
  else if (cp == ansi_cp)
    {
      SetFileApisToANSI ();
      debug_printf ("File APIs set to ANSI");
    }
}

static void
codepage_init (const char *buf)
{
  if (!buf || !*buf)
    return;

  if (strcasematch (buf, "oem"))
    {
      current_codepage = oem_cp;
      set_file_api_mode (current_codepage);
    }
  else if (strcasematch (buf, "ansi"))
    {
      current_codepage = ansi_cp;
      set_file_api_mode (current_codepage);
    }
  else
    debug_printf ("Wrong codepage name: %s", buf);
}

static void
subauth_id_init (const char *buf)
{
  if (!buf || !*buf)
    return;

  int i = strtol (buf, NULL, 0);

  /* 0..127 are reserved by Microsoft, 132 is IIS subauthentication. */
  if (i > 127 && i != 132 && i <= 255)
    subauth_id = i;
}

static void
set_chunksize (const char *buf)
{
  wincap.set_chunksize (strtol (buf, NULL, 0));
}

/* The structure below is used to set up an array which is used to
   parse the CYGWIN environment variable or, if enabled, options from
   the registry.  */
static struct parse_thing
  {
    const char *name;
    union parse_setting
      {
	bool *b;
	DWORD *x;
	int *i;
	void (*func)(const char *);
      } setting;

    enum settings disposition;
    char *remember;
    union parse_values
      {
	DWORD i;
	const char *s;
      } values[2];
  } known[] NO_COPY =
{
  {"binmode", {x: &binmode}, justset, NULL, {{O_TEXT}, {O_BINARY}}},
  {"check_case", {func: &check_case_init}, isfunc, NULL, {{0}, {0}}},
  {"codepage", {func: &codepage_init}, isfunc, NULL, {{0}, {0}}},
#ifdef USE_SERVER
  {"server", {&allow_server}, justset, NULL, {{false}, {true}}},
#endif
  {"envcache", {&envcache}, justset, NULL, {{true}, {false}}},
  {"error_start", {func: &error_start_init}, isfunc, NULL, {{0}, {0}}},
  {"export", {&export_settings}, justset, NULL, {{false}, {true}}},
  {"forkchunk", {func: set_chunksize}, isfunc, NULL, {{0}, {0}}},
  {"glob", {func: &glob_init}, isfunc, NULL, {{0}, {s: "normal"}}},
  {"ntea", {&allow_ntea}, justset, NULL, {{false}, {true}}},
  {"ntsec", {&allow_ntsec}, justset, NULL, {{false}, {true}}},
  {"smbntsec", {&allow_smbntsec}, justset, NULL, {{false}, {true}}},
  {"reset_com", {&reset_com}, justset, NULL, {{false}, {true}}},
  {"strip_title", {&strip_title_path}, justset, NULL, {{false}, {true}}},
  {"subauth_id", {func: &subauth_id_init}, isfunc, NULL, {{0}, {0}}},
  {"title", {&display_title}, justset, NULL, {{false}, {true}}},
  {"tty", {NULL}, set_process_state, NULL, {{0}, {PID_USETTY}}},
  {"winsymlinks", {&allow_winsymlinks}, justset, NULL, {{false}, {true}}},
  {NULL, {0}, justset, 0, {{0}, {0}}}
};

/* Parse a string of the form "something=stuff somethingelse=more-stuff",
   silently ignoring unknown "somethings".  */
static void __stdcall
parse_options (char *buf)
{
  int istrue;
  char *p, *lasts;
  parse_thing *k;

  if (buf == NULL)
    {
      char newbuf[MAX_PATH + 7];
      newbuf[0] = '\0';
      for (k = known; k->name != NULL; k++)
	if (k->remember)
	  {
	    strcat (strcat (newbuf, " "), k->remember);
	    free (k->remember);
	    k->remember = NULL;
	  }

      if (export_settings)
	{
	  debug_printf ("%s", newbuf + 1);
	  setenv ("CYGWIN", newbuf + 1, 1);
	}
      return;
    }

  buf = strcpy ((char *) alloca (strlen (buf) + 1), buf);
  for (p = strtok_r (buf, " \t", &lasts);
       p != NULL;
       p = strtok_r (NULL, " \t", &lasts))
    {
      char *keyword_here = p;
      if (!(istrue = !strncasematch (p, "no", 2)))
	p += 2;
      else if (!(istrue = *p != '-'))
	p++;

      char ch, *eq;
      if ((eq = strchr (p, '=')) != NULL || (eq = strchr (p, ':')) != NULL)
	ch = *eq, *eq++ = '\0';
      else
	ch = 0;

      for (parse_thing *k = known; k->name != NULL; k++)
	if (strcasematch (p, k->name))
	  {
	    switch (k->disposition)
	      {
	      case isfunc:
		k->setting.func ((!eq || !istrue) ?
		  k->values[istrue].s : eq);
		debug_printf ("%s (called func)", k->name);
		break;
	      case justset:
		if (!istrue || !eq)
		  *k->setting.x = k->values[istrue].i;
		else
		  *k->setting.x = strtol (eq, NULL, 0);
		debug_printf ("%s %d", k->name, *k->setting.x);
		break;
	      case set_process_state:
		k->setting.x = &myself->process_state;
		/* fall through */
	      case setbit:
		*k->setting.x &= ~k->values[istrue].i;
		if (istrue || (eq && strtol (eq, NULL, 0)))
		  *k->setting.x |= k->values[istrue].i;
		debug_printf ("%s %x", k->name, *k->setting.x);
		break;
	      }

	    if (eq)
	      *--eq = ch;

	    int n = eq - p;
	    p = strdup (keyword_here);
	    if (n > 0)
	      p[n] = ':';
	    k->remember = p;
	    break;
	  }
      }
  debug_printf ("returning");
  return;
}

/* Set options from the registry. */
static bool __stdcall
regopt (const char *name)
{
  bool parsed_something = false;
  /* FIXME: should not be under mount */
  reg_key r (KEY_READ, CYGWIN_INFO_PROGRAM_OPTIONS_NAME, NULL);
  char buf[MAX_PATH];
  char lname[strlen (name) + 1];
  strlwr (strcpy (lname, name));

  if (r.get_string (lname, buf, sizeof (buf) - 1, "") == ERROR_SUCCESS)
    {
      parse_options (buf);
      parsed_something = true;
    }
  else
    {
      reg_key r1 (HKEY_LOCAL_MACHINE, KEY_READ, "SOFTWARE",
		  CYGWIN_INFO_CYGNUS_REGISTRY_NAME,
		  CYGWIN_INFO_CYGWIN_REGISTRY_NAME,
		  CYGWIN_INFO_PROGRAM_OPTIONS_NAME, NULL);
      if (r1.get_string (lname, buf, sizeof (buf) - 1, "") == ERROR_SUCCESS)
	{
	  parse_options (buf);
	  parsed_something = true;
	}
    }
  MALLOC_CHECK;
  return parsed_something;
}

/* Initialize the environ array.  Look for the CYGWIN environment
   environment variable and set appropriate options from it.  */
void
environ_init (char **envp, int envc)
{
  char *rawenv;
  int i;
  char *p;
  char *newp;
  int sawTERM = 0;
  bool envp_passed_in;
  bool got_something_from_registry;
  static char NO_COPY cygterm[] = "TERM=cygwin";

  static int initted;
  if (!initted)
    {
      for (int i = 0; conv_envvars[i].name != NULL; i++)
	{
	  conv_start_chars[(int) cyg_tolower (conv_envvars[i].name[0])] = 1;
	  conv_start_chars[(int) cyg_toupper (conv_envvars[i].name[0])] = 1;
	}
      initted = 1;
    }

  got_something_from_registry = regopt ("default");
  if (myself->progname[0])
    got_something_from_registry = regopt (myself->progname) || got_something_from_registry;

  /* Set ntsec explicit as default, if NT is running */
  if (wincap.has_security ())
    allow_ntsec = true;

  if (!envp)
    envp_passed_in = 0;
  else
    {
      envc++;
      envc *= sizeof (char *);
      char **newenv = (char **) malloc (envc);
      memcpy (newenv, envp, envc);
      cfree (envp);

      /* Older applications relied on the fact that cygwin malloced elements of the
	 environment list.  */
      envp = newenv;
      if (ENVMALLOC)
	for (char **e = newenv; *e; e++)
	  {
	    char *p = *e;
	    *e = strdup (p);
	    cfree (p);
	  }
      envp_passed_in = 1;
      goto out;
    }

  /* Allocate space for environment + trailing NULL + CYGWIN env. */
  lastenviron = envp = (char **) malloc ((4 + (envc = 100)) * sizeof (char *));
  rawenv = GetEnvironmentStrings ();

  /* Current directory information is recorded as variables of the
     form "=X:=X:\foo\bar; these must be changed into something legal
     (we could just ignore them but maybe an application will
     eventually want to use them).  */
  for (i = 0, p = rawenv; *p != '\0'; p = strchr (p, '\0') + 1, i++)
    {
      newp = strdup (p);
      if (i >= envc)
	envp = (char **) realloc (envp, (4 + (envc += 100)) * sizeof (char *));
      envp[i] = newp;
      if (*newp == '=')
	*newp = '!';
      char *eq = strechr (newp, '=');
      if (!child_proc_info)
	ucenv (newp, eq);
      if (*newp == 'T' && strncmp (newp, "TERM=", 5) == 0)
	sawTERM = 1;
      if (*newp == 'C' && strncmp (newp, "CYGWIN=", sizeof ("CYGWIN=") - 1) == 0)
	parse_options (newp + sizeof ("CYGWIN=") - 1);
      if (*eq && conv_start_chars[(unsigned char)envp[i][0]])
	posify (envp + i, *++eq ? eq : --eq);
      debug_printf ("%p: %s", envp[i], envp[i]);
    }

  if (!sawTERM)
    envp[i++] = cygterm;
  envp[i] = NULL;
  FreeEnvironmentStrings (rawenv);

out:
  __cygwin_environ = envp;
  update_envptrs ();
  if (envp_passed_in)
    {
      p = getenv ("CYGWIN");
      if (p)
	parse_options (p);
    }

  if (got_something_from_registry)
    parse_options (NULL);	/* possibly export registry settings to
				   environment */
  MALLOC_CHECK;
}

/* Function called by qsort to sort environment strings.  */
static int
env_sort (const void *a, const void *b)
{
  const char **p = (const char **) a;
  const char **q = (const char **) b;

  return strcmp (*p, *q);
}

char * __stdcall
getwinenveq (const char *name, size_t namelen, int x)
{
  char dum[1];
  char name0[namelen - 1];
  memcpy (name0, name, namelen - 1);
  name0[namelen - 1] = '\0';
  int totlen = GetEnvironmentVariable (name0, dum, 0);
  if (totlen > 0)
    {
      totlen++;
      if (x == HEAP_1_STR)
	totlen += namelen;
      else
	namelen = 0;
      char *p = (char *) cmalloc ((cygheap_types) x, totlen);
      if (namelen)
	strcpy (p, name);
      if (GetEnvironmentVariable (name0, p + namelen, totlen))
	{
	  debug_printf ("using value from GetEnvironmentVariable for '%s'",
			name0);
	  return p;
	}
      else
	cfree (p);
    }

  debug_printf ("warning: %s not present in environment", name);
  return NULL;
}

struct spenv
{
  const char *name;
  size_t namelen;
  const char * (cygheap_user::*from_cygheap) (const char *, size_t);
  char *retrieve (bool, const char * const = NULL)
    __attribute__ ((regparm (3)));
};

#define env_dontadd almost_null

/* Keep this list in upper case and sorted */
static NO_COPY spenv spenvs[] =
{
  {NL ("HOMEDRIVE="), &cygheap_user::env_homedrive},
  {NL ("HOMEPATH="), &cygheap_user::env_homepath},
  {NL ("LOGONSERVER="), &cygheap_user::env_logsrv},
  {NL ("SYSTEMDRIVE="), NULL},
  {NL ("SYSTEMROOT="), NULL},
  {NL ("USERDOMAIN="), &cygheap_user::env_domain},
  {NL ("USERNAME="), &cygheap_user::env_name},
  {NL ("USERPROFILE="), &cygheap_user::env_userprofile},
};

char *
spenv::retrieve (bool no_envblock, const char *const envname)
{
  if (envname && !strncasematch (envname, name, namelen))
    return NULL;

  debug_printf ("no_envblock %d", no_envblock);

  if (from_cygheap)
    {
      const char *p;
      if (envname && !cygheap->user.issetuid ())
	{
	  debug_printf ("duping existing value for '%s'", name);
	  return cstrdup1 (envname);	/* Don't really care what it's set to
					   if we're calling a cygwin program */
	}

      /* Calculate (potentially) value for given environment variable.  */
      p = (cygheap->user.*from_cygheap) (name, namelen);
      if (!p || (no_envblock && !envname) || (p == env_dontadd))
	return env_dontadd;
      char *s = (char *) cmalloc (HEAP_1_STR, namelen + strlen (p) + 1);
      strcpy (s, name);
      (void) strcpy (s + namelen, p);
      debug_printf ("using computed value for '%s'", name);
      return s;
    }

  if (envname)
    return cstrdup1 (envname);

  return getwinenveq (name, namelen, HEAP_1_STR);
}

#define SPENVS_SIZE (sizeof (spenvs) / sizeof (spenvs[0]))

/* Create a Windows-style environment block, i.e. a typical character buffer
   filled with null terminated strings, terminated by double null characters.
   Converts environment variables noted in conv_envvars into win32 form
   prior to placing them in the string.  */
char ** __stdcall
build_env (const char * const *envp, char *&envblock, int &envc,
	   bool no_envblock)
{
  int len, n;
  const char * const *srcp;
  char **dstp;
  bool saw_spenv[SPENVS_SIZE] = {0};

  debug_printf ("envp %p", envp);

  /* How many elements? */
  for (n = 0; envp[n]; n++)
    continue;

  /* Allocate a new "argv-style" environ list with room for extra stuff. */
  char **newenv = (char **) cmalloc (HEAP_1_ARGV, sizeof (char *) *
						  (n + SPENVS_SIZE + 1));

  int tl = 0;
  /* Iterate over input list, generating a new environment list and refreshing
     "special" entries, if necessary. */
  for (srcp = envp, dstp = newenv; *srcp; srcp++)
    {
      /* Look for entries that require special attention */
      for (unsigned i = 0; i < SPENVS_SIZE; i++)
	if (!saw_spenv[i] && (*dstp = spenvs[i].retrieve (no_envblock, *srcp)))
	  {
	    saw_spenv[i] = 1;
	    if (*dstp == env_dontadd)
	      goto next1;
	    goto  next0;
	  }

      /* Add entry to new environment */
      *dstp = cstrdup1 (*srcp);

    next0:
      /* If necessary, calculate rough running total for envblock size */
      if (!no_envblock)
	tl += strlen (*dstp) + 1;
      dstp++;
    next1:
      continue;
    }

  assert ((srcp - envp) == n);
  /* Fill in any required-but-missing environment variables. */
  for (unsigned i = 0; i < SPENVS_SIZE; i++)
    if (!saw_spenv[i])
      {
	*dstp = spenvs[i].retrieve (no_envblock);
	if (*dstp && !no_envblock && *dstp != env_dontadd)
	  {
	    tl += strlen (*dstp) + 1;
	    dstp++;
	  }
      }

  envc = dstp - newenv;		/* Number of entries in newenv */
  assert ((size_t) envc <= (n + SPENVS_SIZE));
  *dstp = NULL;			/* Terminate */

  if (no_envblock)
    envblock = NULL;
  else
    {
      debug_printf ("env count %d, bytes %d", envc, tl);

      /* Windows programs expect the environment block to be sorted.  */
      qsort (newenv, envc, sizeof (char *), env_sort);

      /* Create an environment block suitable for passing to CreateProcess.  */
      char *s;
      envblock = (char *) malloc (2 + tl);
      int new_tl = 0;
      for (srcp = newenv, s = envblock; *srcp; srcp++)
	{
	  const char *p;
	  win_env *conv;
	  len = strcspn (*srcp, "=") + 1;

	  /* See if this entry requires posix->win32 conversion. */
	  conv = getwinenv (*srcp, *srcp + len);
	  if (conv)
	    p = conv->native;	/* Use win32 path */
	  else
	    p = *srcp;		/* Don't worry about it */

	  len = strlen (p);
	  new_tl += len + 1;	/* Keep running total of block length so far */

	  /* See if we need to increase the size of the block. */
	  if (new_tl > tl)
	    {
	      tl = new_tl + 100;
	      char *new_envblock =
			(char *) realloc (envblock, 2 + tl);
	      /* If realloc moves the block, move `s' with it. */
	      if (new_envblock != envblock)
		{
		  s += new_envblock - envblock;
		  envblock = new_envblock;
		}
	    }

	  memcpy (s, p, len + 1);

	  /* See if environment variable is "special" in a Windows sense.
	     Under NT, the current directories for visited drives are stored
	     as =C:=\bar.  Cygwin converts the '=' to '!' for hopefully obvious
	     reasons.  We need to convert it back when building the envblock */
	  if (s[0] == '!' && (isdrive (s + 1) || (s[1] == ':' && s[2] == ':'))
	      && s[3] == '=')
	    *s = '=';
	  s += len + 1;
	}
      *s = '\0';			/* Two null bytes at the end */
      assert ((s - envblock) <= tl);	/* Detect if we somehow ran over end
					   of buffer */
    }

  debug_printf ("envp %p, envc %d", newenv, envc);
  return newenv;
}

/* This idiocy is necessary because the early implementers of cygwin
   did not seem to know about importing data variables from the DLL.
   So, we have to synchronize cygwin's idea of the environment with the
   main program's with each reference to the environment. */
extern "C" char ** __stdcall
cur_environ ()
{
  if (*main_environ != __cygwin_environ)
    {
      __cygwin_environ = *main_environ;
      update_envptrs ();
    }

  return __cygwin_environ;
}
