/*
 Portions Copyright (c) 1996-2010, PostgreSQL Global Development Group
 Portions Copyright (c) 1994, Regents of the University of California

 This file is licensed under a BSD license.
*/

#pragma once

#include <sys/time.h> /* for struct timeval */

#ifndef __WIN32__
#include <sys/times.h> /* for struct tms */
#endif

#include <limits.h> /* for CLK_TCK */

#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN (-1)

struct rusage
{
  struct timeval ru_utime; /* user time used */
  struct timeval ru_stime; /* system time used */
};

extern int getrusage(int who, struct rusage *rusage);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
