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

#include <Metal/Metal.h>


void dt_metal_get_devices() {
   printf("hugo: Testing Metal!\n");
   id<MTLDevice> device = MTLCreateSystemDefaultDevice();
   NSString* deviceName = [device name];
   printf("hugo: System default device: %s\n", [deviceName UTF8String]);

   printf("hugo: ---------------------------\n");
   printf("hugo: All devices:\n");
   NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
   for (int i = 0; i < [devices count]; i++) {
      deviceName = [[devices objectAtIndex:i] name];
      printf("hugo: %s\n", [deviceName UTF8String]);
   }  
}
