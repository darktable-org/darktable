#ifndef CAMERA_META_DATA_H
#define CAMERA_META_DATA_H

#include "Camera.h"
/* 
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

    http://www.klauspost.com
*/

namespace RawSpeed {

using namespace pugi;

class CameraMetaData
{
public:
  CameraMetaData();
  CameraMetaData(const char *docname);
  virtual ~CameraMetaData(void);
  map<string,Camera*> cameras;
  map<uint32,Camera*> chdkCameras;
  Camera* getCamera(string make, string model, string mode);
  bool hasCamera(string make, string model, string mode);
  Camera* getChdkCamera(uint32 filesize);
  bool hasChdkCamera(uint32 filesize);
  void disableMake(string make);
  void disableCamera(string make, string model);
protected:
  bool addCamera(Camera* cam);
};

} // namespace RawSpeed

#endif
