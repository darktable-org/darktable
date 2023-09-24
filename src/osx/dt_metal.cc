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
#include "common/file_location.h"

#define NS_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#include "Metal.hpp"


static gboolean _dt_metal_create_library(NS::URL *url, MTL::Device *pDevice)
{
  dt_print(DT_DEBUG_METAL,
          "[dt_metal_create_library] Create Metal library for device: %s\n",
          pDevice->name()->utf8String());

  NS::Error *libraryError = NULL;
  MTL::Library *library = pDevice->newLibrary(url, &libraryError);
  if (!library)
  {
    dt_print(DT_DEBUG_METAL,
            "[dt_metal_create_library] Could not create library: %s",
            libraryError->localizedDescription()->utf8String());

    return FALSE;
  }

  dt_print(DT_DEBUG_METAL, "[dt_metal_create_library] Library created\n");

  /*
  for(int fi = 0; fi < library->functionNames()->count(); fi++) 
  {
    const char *functionName = ((NS::String*) library->functionNames()->object(fi))->utf8String();
    dt_print(DT_DEBUG_METAL, 
            "[dt_metal_create_library] Function: %s\n",
            functionName);
  }
  */

  return TRUE;
}

void dt_metal_init(dt_metal_t *metal)
{
  dt_print(DT_DEBUG_METAL,
           "[dt_metal_init] Initializing Metal devices\n");

  // get the path to the metal library
  char metallibpath[PATH_MAX] = { 0 };
  dt_loc_get_sharedir(metallibpath, sizeof(metallibpath));
  g_strlcat(metallibpath, "/darktable/metal/darktable.metallib", sizeof(metallibpath));
  
  dt_print(DT_DEBUG_METAL,
           "[dt_metal_init] metallib path: %s\n", 
           metallibpath);

  NS::URL *url = NS::URL::fileURLWithPath(NS::String::string(metallibpath,
                                          NS::StringEncoding::UTF8StringEncoding));

  // get all Metal devices
  NS::Array *devices = MTL::CopyAllDevices();
  const int num_devices = devices->count();

  if (num_devices)
  {
    metal->num_devs = num_devices;
    metal->dev = (dt_metal_device_t *)calloc(num_devices, sizeof(dt_metal_device_t));

    for(int i = 0; i < num_devices; i++)
    {
      MTL::Device *pDevice = (MTL::Device *) devices->object(i);
      const char* deviceName = pDevice->name()->utf8String();

      dt_print(DT_DEBUG_METAL,
              "[dt_metal_init] Device: %s\n",
              deviceName);

      if (_dt_metal_create_library(url, pDevice))
      {
        metal->dev[i].devid = pDevice->registryID();
        metal->dev[i].device = (void *) pDevice;
      }      
    }
  }

}

void dt_metal_list_devices(dt_metal_t *metal)
{
  for(int i = 0; i < metal->num_devs; i++)
  {
    MTL::Device *pDevice = (MTL::Device *) metal->dev[i].device;
    const char* deviceName = pDevice->name()->utf8String();

    dt_print(DT_DEBUG_METAL,
            "[dt_metal_list_devices] Device: %s\n",
            deviceName);
  }
}
