/* shared_info.h: shared info for cygwin

   Copyright 2000, 2001 Red Hat, Inc.

This file is part of Cygwin.

This software is a copyrighted work licensed under the terms of the
Cygwin license.  Please consult the file "CYGWIN_LICENSE" for
details. */

#include "tty.h"

/* Mount table entry */

class mount_item
{
 public:
  /* FIXME: Nasty static allocation.  Need to have a heap in the shared
     area [with the user being able to configure at runtime the max size].  */
  /* Win32-style mounted partition source ("C:\foo\bar").
     native_path[0] == 0 for unused entries.  */
  char native_path[MAX_PATH];
  int native_pathlen;

  /* POSIX-style mount point ("/foo/bar") */
  char posix_path[MAX_PATH];
  int posix_pathlen;

  unsigned flags;

  void init (const char *dev, const char *path, unsigned flags);

  struct mntent *getmntent ();
  void fnmunge (char *, const char *);
  void build_win32 (char *, const char *, unsigned *, unsigned);
};

/* Warning: Decreasing this value will cause cygwin.dll to ignore existing
   higher numbered registry entries.  Don't change this number willy-nilly.
   What we need is to have a more dynamic allocation scheme, but the current
   scheme should be satisfactory for a long while yet.  */
#define MAX_MOUNTS 30

#define MOUNT_VERSION	27	// increment when mount table changes and
#define MOUNT_VERSION_MAGIC CYGWIN_VERSION_MAGIC (MOUNT_MAGIC, MOUNT_VERSION)
#define CURR_MOUNT_MAGIC 0x4fe431cdU
#define MOUNT_INFO_CB 16488

class reg_key;

/* NOTE: Do not make gratuitous changes to the names or organization of the
   below class.  The layout is checksummed to determine compatibility between
   different cygwin versions. */
class mount_info
{
 public:
  DWORD version;
 unsigned cb;
  DWORD sys_mount_table_counter;
  int nmounts;
  mount_item mount[MAX_MOUNTS];

  /* cygdrive_prefix is used as the root of the path automatically
     prepended to a path when the path has no associated mount.
     cygdrive_flags are the default flags for the cygdrives. */
  char cygdrive[MAX_PATH];
  size_t cygdrive_len;
  unsigned cygdrive_flags;
 private:
  int posix_sorted[MAX_MOUNTS];
  int native_sorted[MAX_MOUNTS];

 public:
  /* Increment when setting up a reg_key if mounts area had to be
     created so we know when we need to import old mount tables. */
  int had_to_create_mount_areas;

  void init ();
  int add_item (const char *dev, const char *path, unsigned flags, int reg_p);
  int del_item (const char *path, unsigned flags, int reg_p);

  void from_registry ();
  int add_reg_mount (const char * native_path, const char * posix_path,
		      unsigned mountflags);
  int del_reg_mount (const char * posix_path, unsigned mountflags);

  unsigned set_flags_from_win32_path (const char *path);
  int conv_to_win32_path (const char *src_path, char *dst, DWORD &devn,
			  int &unit, unsigned *flags = NULL, bool no_normalize = 0);
  int conv_to_posix_path (const char *src_path, char *posix_path,
			  int keep_rel_p);
  struct mntent *getmntent (int x);

  int write_cygdrive_info_to_registry (const char *cygdrive_prefix, unsigned flags);
  int remove_cygdrive_info_from_registry (const char *cygdrive_prefix, unsigned flags);
  int get_cygdrive_info (char *user, char *system, char* user_flags,
			 char* system_flags);

 private:

  void sort ();
  void read_mounts (reg_key& r);
  void mount_slash ();
  void to_registry ();

  int cygdrive_win32_path (const char *src, char *dst, int& unit);
  void cygdrive_posix_path (const char *src, char *dst, int trailing_slash_p);
  void read_cygdrive_info_from_registry ();
};

/******** Close-on-delete queue ********/

/* First pass at a file deletion queue structure.

   We can't keep this list in the per-process info, since
   one process may open a file, and outlive a process which
   wanted to unlink the file - and the data would go away.
*/

#define MAX_DELQUEUES_PENDING 100

class delqueue_list
{
  char name[MAX_DELQUEUES_PENDING][MAX_PATH];
  char inuse[MAX_DELQUEUES_PENDING];
  int empty;

public:
  void init ();
  void queue_file (const char *dosname);
  void process_queue ();
};

/******** Shared Info ********/
/* Data accessible to all tasks */

#define SHARED_VERSION (unsigned)(cygwin_version.api_major << 8 | \
				  cygwin_version.api_minor)
#define SHARED_VERSION_MAGIC CYGWIN_VERSION_MAGIC (SHARED_MAGIC, SHARED_VERSION)

#define SHARED_INFO_CB 47112

#define CURR_SHARED_MAGIC 0x359218a2U

/* NOTE: Do not make gratuitous changes to the names or organization of the
   below class.  The layout is checksummed to determine compatibility between
   different cygwin versions. */
class shared_info
{
  DWORD version;
  DWORD cb;
 public:
  unsigned heap_chunk;
  DWORD sys_mount_table_counter;

  tty_list tty;
  delqueue_list delqueue;
  void initialize ();
  unsigned heap_chunk_size ();
};

extern shared_info *cygwin_shared;
extern mount_info *mount_table;
extern HANDLE cygwin_mount_h;

enum shared_locations
{
  SH_CYGWIN_SHARED,
  SH_MOUNT_TABLE,
  SH_SHARED_CONSOLE,
  SH_MYSELF,
  SH_TOTAL_SIZE
};
void __stdcall memory_init ();

#define shared_align_past(p) \
  ((char *) (system_info.dwAllocationGranularity * \
	     (((DWORD) ((p) + 1) + system_info.dwAllocationGranularity - 1) / \
	      system_info.dwAllocationGranularity)))

#define cygwin_shared_address	((void *) 0xa000000)

#ifdef FHDEVN
struct console_state
{
  tty_min tty_min_state;
  dev_console dev_state;
};
#endif

char *__stdcall shared_name (char *, const char *, int);
void *__stdcall open_shared (const char *name, int n, HANDLE &shared_h, DWORD size, shared_locations);
