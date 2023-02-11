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

/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * statvfs emulation for Windows
 *
 * Copyright 2012 Gerald Richter
 * Copyright 2016 Inuvika Inc.
 * Copyright 2016 David PHAM-VAN <d.phamvan@inuvika.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <glib.h>

#include <string.h>
#include <malloc.h>

//#include <winpr/crt.h>
#include "win/win.h"

#include "statvfs.h"

int statvfs(const char *path, struct statvfs *buf)
{
  BOOL res;

  DWORD lpSectorsPerCluster = 0;
  DWORD lpBytesPerSector = 0;
  DWORD lpNumberOfFreeClusters = 0;
  DWORD lpTotalNumberOfClusters = 0;
  char szDrive[4];

  szDrive[0] = path[0];
  szDrive[1] = ':';
  szDrive[2] = '\\';
  szDrive[3] = '\0';

  wchar_t *wszDrive = g_utf8_to_utf16(szDrive, -1, NULL, NULL, NULL);

  res = GetDiskFreeSpaceW(wszDrive, &lpSectorsPerCluster, &lpBytesPerSector, &lpNumberOfFreeClusters,
                         &lpTotalNumberOfClusters);

  g_free(wszDrive);

  buf->f_bsize = lpBytesPerSector;                        /* file system block size */
  buf->f_frsize = lpBytesPerSector * lpSectorsPerCluster; /* fragment size */
  buf->f_blocks = lpTotalNumberOfClusters;                /* size of fs in f_frsize units */
  buf->f_bfree = lpNumberOfFreeClusters;                  /* # free blocks */
  buf->f_bavail = lpNumberOfFreeClusters;                 /* # free blocks for unprivileged users */
  buf->f_files = 0;                                       /* # inodes */
  buf->f_ffree = 0;                                       /* # free inodes */
  buf->f_favail = 0;                                      /* # free inodes for unprivileged users */
  buf->f_fsid = lpNumberOfFreeClusters & 0xffff;          /* file system ID */
  buf->f_flag = 0;                                        /* mount flags */
  buf->f_namemax = 250;                                   /* maximum filename length */

  if(res != 0)
    return 0;
  else
    return 1;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
