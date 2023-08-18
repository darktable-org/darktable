/*
   This file is part of darktable,
   Copyright (C) 2009-2023 darktable developers.

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

#include "dt_metal.h"
#include "common/darktable.h"
#include "common/file_location.h"

#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include "Metal.hpp"


void _dt_metal_get_devices()
{
  NS::Array *devices = MTL::CopyAllDevices();
  for(int i = 0; i < devices->count(); i++)
  {
    const char* deviceName = ((MTL::Device*) devices->object(i))->name()->utf8String();

    dt_print(DT_DEBUG_METAL,
             "[dt_metal_get_devices] Device: %s\n",
             deviceName);
  }
}

void _dt_metal_create_library()
{
  // MTL::Device *_pDevice = MTL::CreateSystemDefaultDevice();

  char metallibpath[PATH_MAX] = { 0 };
  dt_loc_get_sharedir(metallibpath, sizeof(metallibpath));
  g_strlcat(metallibpath, "/darktable/metal/darktable.metallib", sizeof(metallibpath));
  dt_print(DT_DEBUG_METAL, "[dt_metal_init] metallib path: %s\n", metallibpath);

  NS::URL *url = NS::URL::fileURLWithPath(NS::String::string(metallibpath,
                                          NS::StringEncoding::UTF8StringEncoding));

  NS::Array *devices = MTL::CopyAllDevices();
  for(int i = 0; i < devices->count(); i++)
  {
    MTL::Device *pDevice = (MTL::Device *) devices->object(i);

    dt_print(DT_DEBUG_METAL,
            "[dt_metal_create_library] Create library for device: %s\n",
            pDevice->name()->utf8String());

    NS::Error *libraryError = NULL;
    MTL::Library *library = pDevice->newLibrary(url, &libraryError);
    if (!library)
    {
      dt_print(DT_DEBUG_METAL,
              "[dt_metal_create_library] Could not create library: %s",
              libraryError->localizedDescription()->utf8String());

      return;
    }

    dt_print(DT_DEBUG_METAL, "[dt_metal_create_library] Library created\n");

    for(int fi = 0; fi < library->functionNames()->count(); fi++) 
    {
      const char *functionName = ((NS::String*) library->functionNames()->object(fi))->utf8String();
      dt_print(DT_DEBUG_METAL, 
              "[dt_metal_create_library] Function: %s\n",
              functionName);
    }

  }
}

void dt_metal_init()
{
  dt_print(DT_DEBUG_METAL, "[dt_metal_init] Initializing Metal devices\n");

  // _dt_metal_get_devices();
  _dt_metal_create_library();
}
