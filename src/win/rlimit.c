/*
    This file is part of darktable,
    Copyright (C) 2017-2020 darktable developers.

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

//////////////////////////////////////////////////////////////
//
// Sample implementation of getrlimit() and setrlimit() for Win32.
//
// Includes wrappers around fwrite() and _write() where the wrappers
// are resource limit aware.
//
///////////////////////////////////////////////////////////////

#include <windows.h>
#include "rlimit.h"
#include <io.h>
#include <errno.h>
#include <inttypes.h>

static BOOL rInitialized = FALSE; // Indicates if the rlimit structure has been initialized

static rlimit_t rlimits[RLIM_NLIMITS]; // Resource limits array on element for each limit we
                                       // keep track of.

///////////////////////////////////////////////////////////////
//
// InitializeRlimits()
//
// Sets the initial values in the rlimits arrar for the process.
//
///////////////////////////////////////////////////////////////
void InitializeRlimits()
{
  //
  // Initialize the rlimits structure with 0 for the current value,
  // and 2^32 - 1 for the max.  This function could be modified
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

/////////////////////////////////////////////////////////////////
// getrlimit()
//
// NOTE: Posix spec states function returns 0 on success and -1
//       when an error occurs and sets errno to the error code.
//       Currently, if an error occurs, the errno value is returned
//       rather than -1.  errno is not set.
//
///////////////////////////////////////////////////////////////int
int getrlimit(int resource, struct rlimit *rlp)
{
  //
  // If we have not initialized the limits yet, do so now
  //
  if(!rInitialized) InitializeRlimits();

  //
  // Check to make sure the resource value is within range
  //
  if(resource < 0 || resource >= RLIM_NLIMITS)
    return EINVAL;

  //
  // Return both rlim_cur and rlim_max
  //
  *rlp = rlimits[resource];

  return 0; // success
}

/////////////////////////////////////////////////////////////////
// setrlimit()
//
// NOTE: Posix spec states function returns 0 on success and -1
//       when an error occurs and sets errno to the error code.
//       Currently, if an error occurs, the errno value is returned
//       rather than -1.  errno is not set.
//
///////////////////////////////////////////////////////////////int
int setrlimit(int resource, const struct rlimit *rlp)
{
  if(!rInitialized) InitializeRlimits();
  //
  // Check to make sure the resource value is within range
  //
  if(resource < 0 || resource >= RLIM_NLIMITS)
    return EINVAL;
  //
  // Only change the current limit - do not change the max limit.
  // We could pick some NT privilege, which if the user held, we
  // would allow the changing of rlim_max.
  //
  if(rlp->rlim_cur < rlimits[resource].rlim_max)
    rlimits[resource].rlim_cur = rlp->rlim_cur;
  else
    return EINVAL;
  //
  // We should not let the user set the max value.  However,
  // since currently there is no defined source for initial
  // values, we will let the user change the max value.
  //
  rlimits[resource].rlim_max = rlp->rlim_max;

  return 0;  // success
}

/////////////////////////////////////////////////////////////////
// Wrap the real fwrite() with this rfwrite() function, which is
// resource limit aware.
//
///////////////////////////////////////////////////////////////size_t
size_t rfwrite(const void *buffer, size_t size, size_t count, FILE *stream)
{
  //
  // Convert the count to a large integer (64 bit integer)
  //
  const __int64 liByteCount = (__int64)count;

  //
  // Get the current file position
  //
  const __int64 liPosition = (__int64)ftell(stream);

  //
  // Check to make sure the write will not exceed the RLIMIT_FSIZE limit.
  //
  if(liPosition + liByteCount > rlimits[RLIMIT_FSIZE].rlim_cur)
  {
    //
    // report an error
    //
    return 0;
  }
  //
  // Do the actual write the user requested
  //
  return fwrite(buffer, size, count, stream);
}

/////////////////////////////////////////////////////////////////
// Wrap the real _write() function with the _rwrite() function
// which is resource aware.
//
///////////////////////////////////////////////////////////////int
int _rwrite(int handle, const void *buffer, unsigned int count)
{
  //
  // Convert the count to a large integer
  //
  const int64_t liByteCount = (__int64)count;

  //
  // Get the Current file position
  //
  const int64_t liPosition = (__int64)_tell(handle);

  //
  // Check to make sure the write will not exceed the RLIMIT_FSIZE limit.
  //
  if(liPosition + liByteCount > rlimits[RLIMIT_FSIZE].rlim_cur)
  {
    //
    // report an error
    //
    return 0;
  }

  //
  // Do the actual write the user requested
  //
  return _write(handle, buffer, count);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

