/*
    This file is part of darktable,
    Copyright (C) 2017-2024 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

// Implementation of getrlimit() and setrlimit() for Win32.
//
// Includes wrappers around fwrite() and _write() where the wrappers
// are resource limit aware.

#include <windows.h>
#include "rlimit.h"
#include <io.h>
#include <errno.h>
#include <inttypes.h>

// Indicates if the rlimit structure has been initialized
static BOOL rInitialized = FALSE;

// Resource limits array on element for each limit we keep track of
static rlimit_t rlimits[RLIM_NLIMITS];

// Sets the initial values in the rlimits array for the process
void InitializeRlimits()
{
  // Initialize the rlimits structure with 0 for the current value,
  // and 2^32-1 for the max.  This function could be modified
  // to read the initial values from...
  //     ...the registry...
  //     ...an environment variable...
  //     ...a disk file...
  //     ...other...
  // which would then be used to populate the rlimits structure.
  //
  for(int i = 0; i < RLIM_NLIMITS; i++)
  {
    rlimits[i].rlim_cur = RLIM_INFINITY;
    rlimits[i].rlim_max = 0xffffffff;
  }
  rInitialized = TRUE;
}

// NOTE: Posix spec states getrlimit() returns 0 on success and -1
//       when an error occurs and sets errno to the error code.
//       Currently, if an error occurs, the errno value is returned
//       rather than -1.  errno is not set.
//
int getrlimit(int resource, struct rlimit *rlp)
{
  // If we have not initialized the limits yet, do so now
  if(!rInitialized) InitializeRlimits();

  // Check to make sure the resource value is within range
  if(resource < 0 || resource >= RLIM_NLIMITS)
    return EINVAL;

  // Return both rlim_cur and rlim_max
  *rlp = rlimits[resource];

  return 0; // success
}

// NOTE: Posix spec states setrlimit() returns 0 on success and -1
//       when an error occurs and sets errno to the error code.
//       Currently, if an error occurs, the errno value is returned
//       rather than -1.  errno is not set.
//
int setrlimit(int resource, const struct rlimit *rlp)
{
  if(!rInitialized) InitializeRlimits();

  // Check to make sure the resource value is within range
  if(resource < 0 || resource >= RLIM_NLIMITS)
    return EINVAL;

  // Only change the current limit - do not change the max limit.
  // We could pick some NT privilege, which if the user held, we
  // would allow the changing of rlim_max.
  if(rlp->rlim_cur < rlimits[resource].rlim_max)
    rlimits[resource].rlim_cur = rlp->rlim_cur;
  else
    return EINVAL;

  // We should not let the user set the max value.  However,
  // since currently there is no defined source for initial
  // values, we will let the user change the max value.
  rlimits[resource].rlim_max = rlp->rlim_max;

  return 0;  // success
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
