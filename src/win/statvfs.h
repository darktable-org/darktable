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
 * statvfs emulation for windows
 *
 * Copyright 2012 Gerald Richter
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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long long fsblkcnt_t;
typedef unsigned long long fsfilcnt_t;

struct statvfs
{
  unsigned long f_bsize;   /* file system block size */
  unsigned long f_frsize;  /* fragment size */
  fsblkcnt_t f_blocks;     /* size of fs in f_frsize units */
  fsblkcnt_t f_bfree;      /* # free blocks */
  fsblkcnt_t f_bavail;     /* # free blocks for unprivileged users */
  fsfilcnt_t f_files;      /* # inodes */
  fsfilcnt_t f_ffree;      /* # free inodes */
  fsfilcnt_t f_favail;     /* # free inodes for unprivileged users */
  unsigned long f_fsid;    /* file system ID */
  unsigned long f_flag;    /* mount flags */
  unsigned long f_namemax; /* maximum filename length */
};

int statvfs(const char *path, struct statvfs *buf);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
