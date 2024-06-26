/*
    This file is part of darktable,
    Copyright (C) 2012-2024 darktable developers.

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

#pragma once

#include <strings.h>

#ifndef __APPLE__
// Strings and compute capability versions vim-macro converted from
// http://developer.nvidia.com/cuda/cuda-gpus on 2012/09/18.

// All models with compute capability >= 2.0 were then filtered out of the table.
// The compute capabilities value from the table is not used, it is present only
// for the purpose of documenting and possible verification of correctness.

// We're not really interested in updates, this is merely used to detect which
// GPUs don't support sm_20 (and we believe newer GPUs will all do that).
static const char *nvidia_gpus[] = {
  // clang-format off

  "Tesla C1060", "1.3",
  "Tesla C870", "1.0",
  "Tesla D870", "1.0",
  "Tesla S1070", "1.3",
  "Tesla M1060", "1.3",
  "Tesla S870", "1.0",
  "Quadro FX 5800", "1.3",
  "Quadro FX 5600", "1.0",
  "Quadro FX 4800", "1.3",
  "Quadro FX 4800 for Mac", "1.3",
  "Quadro FX 4700 X2", "1.1",
  "Quadro FX 4600", "1.0",
  "Quadro FX 3800", "1.3",
  "Quadro FX 3700", "1.1",
  "Quadro FX 1800", "1.1",
  "Quadro FX 1700", "1.1",
  "Quadro FX 580", "1.1",
  "Quadro FX 570", "1.1",
  "Quadro FX 470", "1.1",
  "Quadro FX 380", "1.1",
  "Quadro FX 380 Low Profile", "1.2",
  "Quadro FX 370", "1.1",
  "Quadro FX 370 Low Profile", "1.1",
  "Quadro CX", "1.3",
  "Quadro NVS 450", "1.1",
  "Quadro NVS 420", "1.1",
  "Quadro NVS 300", "1.2",
  "Quadro NVS 295", "1.1",
  "Quadro Plex 7000", "2.0",
  "Quadro Plex 2200 D2", "1.3",
  "Quadro Plex 2100 D4", "1.1",
  "Quadro Plex 2100 S4", "1.0",
  "Quadro FX 3800M", "1.1",
  "Quadro FX 3700M", "1.1",
  "Quadro FX 3600M", "1.1",
  "Quadro FX 2800M", "1.1",
  "Quadro FX 2700M", "1.1",
  "Quadro FX 1800M", "1.2",
  "Quadro FX 1700M", "1.1",
  "Quadro FX 1600M", "1.1",
  "Quadro FX 880M", "1.2",
  "Quadro FX 770M", "1.1",
  "Quadro FX 570M", "1.1",
  "Quadro FX 380M", "1.2",
  "Quadro FX 370M", "1.1",
  "Quadro FX 360M", "1.1",
  "Quadro NVS 320M", "1.1",
  "Quadro NVS 160M", "1.1",
  "Quadro NVS 150M", "1.1",
  "Quadro NVS 140M", "1.1",
  "Quadro NVS 135M", "1.1",
  "Quadro NVS 130M", "1.1",
  "Quadro NVS 450", "1.1",
  "Quadro NVS 420", "1.1",
  "NVIDIA NVS 300", "1.2",
  "Quadro NVS 295", "1.1",
  "NVS 5100M", "1.2",
  "NVS 3100M", "1.2",
  "NVS 2100M", "1.2",
  "NVS 300", "1.2",
  "GeForce GTX 295", "1.3",
  "GeForce GTX 285", "1.3",
  "GeForce GTX 285 for Mac", "1.3",
  "GeForce GTX 280", "1.3",
  "GeForce GTX 275", "1.3",
  "GeForce GTX 260", "1.3",
  "GeForce GT 420", "1.0",
  "GeForce GT 240", "1.2",
  "GeForce GTS 240", "1.2",
  "GeForce GT 220", "1.2",
  "GeForce 210", "1.2",
  "GeForce GTS 250", "1.1",
  "GeForce GTS 150", "1.1",
  "GeForce GT 130", "1.1",
  "GeForce GT 120", "1.1",
  "GeForce G100", "1.1",
  "GeForce 9800 GX2", "1.1",
  "GeForce 9800 GTX+", "1.1",
  "GeForce 9800 GTX", "1.1",
  "GeForce 9600 GSO", "1.1",
  "GeForce 9500 GT", "1.1",
  "GeForce 9400 GT", "1.1",
  "GeForce 8800 GTS", "1.1",
  "GeForce 8800 GTS 512", "1.1",
  "GeForce 8800 GT", "1.1",
  "GeForce 8800 GS", "1.1",
  "GeForce 8600 GTS", "1.1",
  "GeForce 8600 GT", "1.1",
  "GeForce 8500 GT", "1.1",
  "GeForce 8400 GS", "1.1",
  "GeForce 9400 mGPU", "1.1",
  "GeForce 9300 mGPU", "1.1",
  "GeForce 8300 mGPU", "1.1",
  "GeForce 8200 mGPU", "1.1",
  "GeForce 8100 mGPU", "1.1",
  "GeForce 8800 Ultra", "1.0",
  "GeForce 8800 GTX", "1.0",
  "GeForce GT 340", "1.0",
  "GeForce GT 330", "1.0",
  "GeForce GT 320", "1.0",
  "GeForce 315", "1.0",
  "GeForce 310", "1.0",
  "GeForce 9800 GT", "1.0",
  "GeForce 9600 GT", "1.0",
  "GeForce 9400GT", "1.0",
  "GeForce GTS 360M", "1.2",
  "GeForce GTS 350M", "1.2",
  "GeForce GT 335M", "1.2",
  "GeForce GT 330M", "1.2",
  "GeForce GT 320M", "1.2",
  "GeForce GT 325M", "1.2",
  "GeForce GT 240M", "1.2",
  "GeForce G210M", "1.2",
  "GeForce 310M", "1.2",
  "GeForce 305M", "1.2",
  "GeForce GTX 285M", "1.1",
  "GeForce GTX 280M", "1.1",
  "GeForce GTX 260M", "1.1",
  "GeForce 9800M GTX", "1.1",
  "GeForce 8800M GTX", "1.1",
  "GeForce GTS 260M", "1.1",
  "GeForce GTS 250M", "1.1",
  "GeForce 9800M GT", "1.1",
  "GeForce 9600M GT", "1.1",
  "GeForce 8800M GTS", "1.1",
  "GeForce 9800M GTS", "1.1",
  "GeForce GT 230M", "1.1",
  "GeForce 9700M GT", "1.1",
  "GeForce 9650M GS", "1.1",
  "GeForce 9700M GT", "1.1",
  "GeForce 9650M GS", "1.1",
  "GeForce 9600M GT", "1.1",
  "GeForce 9600M GS", "1.1",
  "GeForce 9500M GS", "1.1",
  "GeForce 8700M GT", "1.1",
  "GeForce 8600M GT", "1.1",
  "GeForce 8600M GS", "1.1",
  "GeForce 9500M G", "1.1",
  "GeForce 9300M G", "1.1",
  "GeForce 8400M GS", "1.1",
  "GeForce G210M", "1.1",
  "GeForce G110M", "1.1",
  "GeForce 9300M GS", "1.1",
  "GeForce 9200M GS", "1.1",
  "GeForce 9100M G", "1.1",
  "GeForce 8400M GT", "1.1",
  "GeForce G105M", "1.1",
  "GeForce G102M", "1.1",
  "GeForce G 103M", "1.1",
  "ION", "1.0",
  NULL, NULL

  // clang-format on
};
#endif

gboolean dt_nvidia_gpu_supports_sm_20(const char *model)
{
#ifdef __APPLE__
  // On macOS the OpenCL driver does not seem to support inline asm - even with recent NVIDIA GPUs.
  return FALSE;
#else
  int i = 0;
  while(nvidia_gpus[2 * i] != NULL)
  {
    if(!strcasecmp(model, nvidia_gpus[2 * i]))
    {
      // The table contains only models with compute capabilities < 2.0, so
      // we can already return the result without additional check.
      return FALSE;
    }
    i++;
  }
  return TRUE;
#endif
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
