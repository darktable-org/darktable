/*
 Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 Portions Copyright (c) 1994, Regents of the University of California

 This file is licensed under a BSD license.
*/

#ifdef __WIN32__
#include "win/win.h"
#endif

#include <errno.h>

#include "getrusage.h"

/*
 * lengthof
 *              Number of elements in an array.
 */
#define lengthof(array) (sizeof(array) / sizeof((array)[0]))

static const struct
{
  DWORD winerr;
  int doserr;
} doserrors[] =

      { { ERROR_INVALID_FUNCTION, EINVAL },
        { ERROR_FILE_NOT_FOUND, ENOENT },
        { ERROR_PATH_NOT_FOUND, ENOENT },
        { ERROR_TOO_MANY_OPEN_FILES, EMFILE },
        { ERROR_ACCESS_DENIED, EACCES },
        { ERROR_INVALID_HANDLE, EBADF },
        { ERROR_ARENA_TRASHED, ENOMEM },
        { ERROR_NOT_ENOUGH_MEMORY, ENOMEM },
        { ERROR_INVALID_BLOCK, ENOMEM },
        { ERROR_BAD_ENVIRONMENT, E2BIG },
        { ERROR_BAD_FORMAT, ENOEXEC },
        { ERROR_INVALID_ACCESS, EINVAL },
        { ERROR_INVALID_DATA, EINVAL },
        { ERROR_INVALID_DRIVE, ENOENT },
        { ERROR_CURRENT_DIRECTORY, EACCES },
        { ERROR_NOT_SAME_DEVICE, EXDEV },
        { ERROR_NO_MORE_FILES, ENOENT },
        { ERROR_LOCK_VIOLATION, EACCES },
        { ERROR_SHARING_VIOLATION, EACCES },
        { ERROR_BAD_NETPATH, ENOENT },
        { ERROR_NETWORK_ACCESS_DENIED, EACCES },
        { ERROR_BAD_NET_NAME, ENOENT },
        { ERROR_FILE_EXISTS, EEXIST },
        { ERROR_CANNOT_MAKE, EACCES },
        { ERROR_FAIL_I24, EACCES },
        { ERROR_INVALID_PARAMETER, EINVAL },
        { ERROR_NO_PROC_SLOTS, EAGAIN },
        { ERROR_DRIVE_LOCKED, EACCES },
        { ERROR_BROKEN_PIPE, EPIPE },
        { ERROR_DISK_FULL, ENOSPC },
        { ERROR_INVALID_TARGET_HANDLE, EBADF },
        { ERROR_INVALID_HANDLE, EINVAL },
        { ERROR_WAIT_NO_CHILDREN, ECHILD },
        { ERROR_CHILD_NOT_COMPLETE, ECHILD },
        { ERROR_DIRECT_ACCESS_HANDLE, EBADF },
        { ERROR_NEGATIVE_SEEK, EINVAL },
        { ERROR_SEEK_ON_DEVICE, EACCES },
        { ERROR_DIR_NOT_EMPTY, ENOTEMPTY },
        { ERROR_NOT_LOCKED, EACCES },
        { ERROR_BAD_PATHNAME, ENOENT },
        { ERROR_MAX_THRDS_REACHED, EAGAIN },
        { ERROR_LOCK_FAILED, EACCES },
        { ERROR_ALREADY_EXISTS, EEXIST },
        { ERROR_FILENAME_EXCED_RANGE, ENOENT },
        { ERROR_NESTING_NOT_ALLOWED, EAGAIN },
        { ERROR_NOT_ENOUGH_QUOTA, ENOMEM } };

static void _dosmaperr(unsigned long e)
{
  int i;

  if(e == 0)
  {
    errno = 0;
    return;
  }

  for(i = 0; i < lengthof(doserrors); i++)
  {
    if(doserrors[i].winerr == e)
    {
      errno = doserrors[i].doserr;
      // #ifndef FRONTEND
      //       ereport(DEBUG5,
      //               (errmsg_internal("mapped win32 error code %lu to %d",
      //               e, errno)));
      // #elif FRONTEND_DEBUG
      //       fprintf(stderr, _("mapped win32 error code %lu to %d"), e, errno);
      // #endif
      return;
    }
  }

  // #ifndef FRONTEND
  //   ereport(LOG,
  //           (errmsg_internal("unrecognized win32 error code: %lu",
  //           e)));
  // #else
  //   fprintf(stderr, _("unrecognized win32 error code: %lu"), e);
  // #endif

  errno = EINVAL;
  return;
}

/* This code works on:
 *              univel
 *              solaris_i386
 *              sco
 *              solaris_sparc
 *              svr4
 *              hpux 9.*
 *              win32
 * which currently is all the supported platforms that don't have a
 * native version of getrusage().  So, if configure decides to compile
 * this file at all, we just use this version unconditionally.
 */
int getrusage(int who, struct rusage *rusage)
{
#ifdef __WIN32__

  FILETIME starttime;
  FILETIME exittime;
  FILETIME kerneltime;
  FILETIME usertime;
  ULARGE_INTEGER li;

  if(who != RUSAGE_SELF)
  {
    /* Only RUSAGE_SELF is supported in this implementation for now */
    errno = EINVAL;
    return -1;
  }

  if(rusage == (struct rusage *)NULL)
  {
    errno = EFAULT;
    return -1;
  }
  memset(rusage, 0, sizeof(struct rusage));
  if(GetProcessTimes(GetCurrentProcess(), &starttime, &exittime, &kerneltime, &usertime) == 0)
  {
    _dosmaperr(GetLastError());
    return -1;
  }

  /* Convert FILETIMEs (0.1 us) to struct timeval */
  memcpy(&li, &kerneltime, sizeof(FILETIME));
  li.QuadPart /= 10L; /* Convert to microseconds */
  rusage->ru_stime.tv_sec = li.QuadPart / 1000000L;
  rusage->ru_stime.tv_usec = li.QuadPart % 1000000L;

  memcpy(&li, &usertime, sizeof(FILETIME));
  li.QuadPart /= 10L; /* Convert to microseconds */
  rusage->ru_utime.tv_sec = li.QuadPart / 1000000L;
  rusage->ru_utime.tv_usec = li.QuadPart % 1000000L;
#else /* all but __WIN32__ */

  struct tms tms;
  int tick_rate = CLK_TCK; /* ticks per second */
  clock_t u, s;

  if(rusage == (struct rusage *)NULL)
  {
    errno = EFAULT;
    return -1;
  }
  if(times(&tms) < 0)
  {
    /* errno set by times */
    return -1;
  }
  switch(who)
  {
    case RUSAGE_SELF:
      u = tms.tms_utime;
      s = tms.tms_stime;
      break;
    case RUSAGE_CHILDREN:
      u = tms.tms_cutime;
      s = tms.tms_cstime;
      break;
    default:
      errno = EINVAL;
      return -1;
  }
#define TICK_TO_SEC(T, RATE) ((T) / (RATE))
#define TICK_TO_USEC(T, RATE) (((T) % (RATE)*1000000) / RATE)
  rusage->ru_utime.tv_sec = TICK_TO_SEC(u, tick_rate);
  rusage->ru_utime.tv_usec = TICK_TO_USEC(u, tick_rate);
  rusage->ru_stime.tv_sec = TICK_TO_SEC(s, tick_rate);
  rusage->ru_stime.tv_usec = TICK_TO_USEC(u, tick_rate);
#endif /* __WIN32__ */

  return 0;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
