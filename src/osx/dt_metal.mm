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

/* workaround to fix issue #12720 */
#define _DARWIN_C_SOURCE

#include <Metal/Metal.h>
#include "dt_metal.h"
#include "common/darktable.h"
#include "osx.h"


void dt_metal_get_devices()
{

  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  NSString* deviceName = [device name];
  dt_print(DT_DEBUG_ALL, 
           "[dt_metal_get_devices] System default device: %s\n",
           [deviceName UTF8String]);

  dt_print(DT_DEBUG_METAL,
           "[dt_metal_get_devices] All devices:\n");

  NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
  for (int i = 0; i < [devices count]; i++)
  {
    deviceName = [[devices objectAtIndex:i] name];
    dt_print(DT_DEBUG_METAL,
             "[dt_metal_get_devices] Device: %s\n",
             [deviceName UTF8String]);
  }  
}

void dt_metal_create_library()
{
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();

  dt_print(DT_DEBUG_METAL,
           "[dt_metal_create_library] Create library for device: %s\n",
           [[device name] UTF8String]);

  NSURL *url = [NSURL fileURLWithPath:@"/Users/mario/src/darktable/build/bin/metal/darktable.metallib"];

  NSError *libraryError = NULL;

  id<MTLLibrary> myLibrary = [device newLibraryWithURL:url error:&libraryError];
  
  dt_print(DT_DEBUG_METAL,
           "[dt_metal_create_library] Library created\n");

  for (int i = 0; i < [myLibrary.functionNames count]; i++)
  {
    NSString *functionName = [myLibrary.functionNames objectAtIndex:i];

    dt_print(DT_DEBUG_METAL,
            "[dt_metal_create_library] Function: %s\n",
            [functionName UTF8String]);
  }


}

void dt_metal_init()
{
  // dt_metal_get_devices();
  dt_metal_create_library();
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
