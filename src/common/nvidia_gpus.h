/*
    This file is part of darktable,
    Copyright (C) 2012-2020 darktable developers.

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
// strings and compute model versions vim-macro converted from http://developer.nvidia.com/cuda/cuda-gpus
// 2012/09/18.
// we're not really interested in updates, this is merely used to detect which gpus don't support sm_20
// (and we hope newer gpus will all do that)
static const char *nvidia_gpus[] = {
  // clang-format off

  "Tesla C2075", "2.0",
  "Tesla C2050/C2070", "2.0",
  "Tesla C1060", "1.3",
  "Tesla C870", "1.0",
  "Tesla D870", "1.0",
  "Tesla K20", "3.5",
  "Tesla K10", "3.0",
  "Tesla M2050/M2070/M2075/M2090", "2.0",
  "Tesla S1070", "1.3",
  "Tesla M1060", "1.3",
  "Tesla S870", "1.0",
  "Quadro K5000", "3.0",
  "Quadro 6000", "2.0",
  "Quadro 5000", "2.0",
  "Quadro 4000", "2.0",
  "Quadro 4000 for Mac", "2.0",
  "Quadro 2000", "2.1",
  "Quadro 2000D", "2.1",
  "Quadro 600", "2.1",
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
  "Quadro K500M", "3.0",
  "Quadro 5010M", "2.0",
  "Quadro 5000M", "2.0",
  "Quadro 4000M", "2.1",
  "Quadro 3000M", "2.1",
  "Quadro 2000M", "2.1",
  "Quadro 1000M", "2.1",
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
  "NVS 4200M", "2.1",
  "NVS 5100M", "1.2",
  "NVS 3100M", "1.2",
  "NVS 2100M", "1.2",
  "NVS 300", "1.2",
  "GeForce GTX 690", "3.0",
  "GeForce GTX 680", "3.0",
  "GeForce GTX 670", "3.0",
  "GeForce GTX 660 Ti", "3.0",
  "GeForce GTX 660", "3.0",
  "GeForce GTX 650", "3.0",
  "GeForce GTX 560 Ti", "2.1",
  "GeForce GTX 550 Ti", "2.1",
  "GeForce GTX 460", "2.1",
  "GeForce GTS 450", "2.1",
  "GeForce GTX 590", "2.0",
  "GeForce GTX 580", "2.0",
  "GeForce GTX 570", "2.0",
  "GeForce GTX 480", "2.0",
  "GeForce GTX 470", "2.0",
  "GeForce GTX 465", "2.0",
  "GeForce GTX 295", "1.3",
  "GeForce GTX 285", "1.3",
  "GeForce GTX 285 for Mac", "1.3",
  "GeForce GTX 280", "1.3",
  "GeForce GTX 275", "1.3",
  "GeForce GTX 260", "1.3",
  "GeForce GT 640", "2.1",
  "GeForce GT 630", "2.1",
  "GeForce GT 620", "2.1",
  "GeForce GT 610", "2.1",
  "GeForce GT 520", "2.1",
  "GeForce GT 440", "2.1",
  "GeForce GT 430", "2.1",
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
  "GeForce GTX 680M", "3.0",
  "GeForce GTX 675M", "2.1",
  "GeForce GTX 670M", "2.1",
  "GeForce GTX 660M", "3.0",
  "GeForce GT 650M", "3.0",
  "GeForce GT 640M", "3.0",
  "GeForce GT 640M LE", "3.0",
  "GeForce GT 635M", "2.1",
  "GeForce GT 630M", "2.1",
  "GeForce GT 620M", "2.1",
  "GeForce 610M", "2.1",
  "GeForce GTX 580M", "2.1",
  "GeForce GTX 570M", "2.1",
  "GeForce GTX 560M", "2.1",
  "GeForce GT 555M", "2.1",
  "GeForce GT 550M", "2.1",
  "GeForce GT 540M", "2.1",
  "GeForce GT 525M", "2.1",
  "GeForce GT 520MX", "2.1",
  "GeForce GT 520M", "2.1",
  "GeForce GTX 485M", "2.1",
  "GeForce GTX 470M", "2.1",
  "GeForce GTX 460M", "2.1",
  "GeForce GT 445M", "2.1",
  "GeForce GT 435M", "2.1",
  "GeForce GT 420M", "2.1",
  "GeForce GT 415M", "2.1",
  "GeForce GTX 480M", "2.0",
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

int dt_nvidia_gpu_supports_sm_20(const char *model)
{
#ifdef __APPLE__
  // on Mac OSX the OpenCL driver does not seem to support inline asm - even with recent NVIDIA GPUs
  return 0;
#else
  int i = 0;
  while(nvidia_gpus[2 * i] != NULL)
  {
    if(!strcasecmp(model, nvidia_gpus[2 * i]))
    {
      if(nvidia_gpus[2 * i + 1][0] >= '2') return 1;
      return 0;
    }
    i++;
  }
  // if we don't know the device, it's probably too new and all good.
  return 1;
#endif
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

