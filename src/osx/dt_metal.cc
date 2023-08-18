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
  MTL::Device *_pDevice = MTL::CreateSystemDefaultDevice();

  dt_print(DT_DEBUG_METAL,
           "[dt_metal_create_library] Create library for device: %s\n",
           _pDevice->name()->utf8String());

  NS::URL *url = NS::URL::fileURLWithPath(NS::String::string("/Users/mario/src/darktable/build/macosx/share/darktable/metal/darktable.metallib",
                                          NS::StringEncoding::UTF8StringEncoding));

  NS::Error *libraryError = NULL;

  MTL::Library *library = _pDevice->newLibrary(url, &libraryError);
  if (!library)
  {
    dt_print(DT_DEBUG_METAL,
             "[dt_metal_create_library] Could not create library: %s",
             libraryError->localizedDescription()->utf8String());

    return;
  }

  dt_print(DT_DEBUG_METAL, "[dt_metal_create_library] Library created\n");

  for(int i = 0; i < library->functionNames()->count(); i++) 
  {
    const char *functionName = ((NS::String*) library->functionNames()->object(i))->utf8String();
    dt_print(DT_DEBUG_METAL, 
             "[dt_metal_create_library] Function: %s\n",
             functionName);
  }  
}

void dt_metal_init()
{
  dt_print(DT_DEBUG_METAL, "[dt_metal_init] Initializing Metal devices\n");

  _dt_metal_get_devices();
  _dt_metal_create_library();
}
