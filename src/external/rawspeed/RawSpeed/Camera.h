#ifndef CAMERA_H
#define CAMERA_H

#include "ColorFilterArray.h"
#include <libxml/parser.h>
#include "BlackArea.h"
#include "CameraMetadataException.h"
/* 
    RawSpeed - RAW file decoder.

    Copyright (C) 2009 Klaus Post

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
  Camera(xmlDocPtr doc, xmlNodePtr cur);
  Camera(const Camera* camera, uint32 alias_num);
  void parseCameraChild(xmlDocPtr doc, xmlNodePtr cur);
  virtual ~Camera(void);
  string make;
  string model;
  string mode;
  vector<string> aliases;
  ColorFilterArray cfa;
  uint32 black;
  uint32 white;
  bool supported;
  iPoint2D cropSize;
  iPoint2D cropPos;
  vector<BlackArea> blackAreas;
  int decoderVersion;
  map<string,string> hints;
private:
  int StringToInt(const xmlChar *in, const xmlChar *tag, const char* attribute);
  int getAttributeAsInt( xmlNodePtr cur , const xmlChar *tag, const char* attribute);
protected:
  void parseCFA( xmlDocPtr doc, xmlNodePtr cur );
  void parseAlias( xmlDocPtr doc, xmlNodePtr cur );
  void parseHint( xmlDocPtr doc, xmlNodePtr cur );
  void parseBlackAreas( xmlDocPtr doc, xmlNodePtr cur );
};

} // namespace RawSpeed

#endif
