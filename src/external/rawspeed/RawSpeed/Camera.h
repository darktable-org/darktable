#ifndef CAMERA_H
#define CAMERA_H

#include "ColorFilterArray.h"
#include "CameraSensorInfo.h"
#include "BlackArea.h"
#include "CameraMetadataException.h"
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


class Camera
{
public:
  Camera(pugi::xml_node &camera);
  Camera(const Camera* camera, uint32 alias_num);
  void parseCameraChild( pugi::xml_node &node );
  const CameraSensorInfo* getSensorInfo(int iso);
  virtual ~Camera(void);
  string make;
  string model;
  string mode;
  string canonical_make;
  string canonical_model;
  string canonical_alias;
  string canonical_id;
  vector<string> aliases;
  vector<string> canonical_aliases;
  ColorFilterArray cfa;
  bool supported;
  iPoint2D cropSize;
  iPoint2D cropPos;
  vector<BlackArea> blackAreas;
  vector<CameraSensorInfo> sensorInfo;
  int decoderVersion;
  map<string,string> hints;
protected:
  void parseCFA( pugi::xml_node &node );
  void parseAlias( pugi::xml_node &node );
  void parseHint( pugi::xml_node &node );
  void parseID( pugi::xml_node &node );
  void parseBlackAreas( pugi::xml_node &node );
  void parseSensorInfo( pugi::xml_node &node );
  vector<int> MultipleStringToInt(const char *in, const char *tag, const char* attribute);
};

} // namespace RawSpeed

#endif
