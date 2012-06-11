#ifndef CAMERA_META_DATA_H
#define CAMERA_META_DATA_H

#include <libxml/xmlmemory.h>
#include <libxml/parser.h>
#include <libxml/xmlschemas.h>
#include "Camera.h"
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

class CameraMetaData
{
public:
  CameraMetaData();
  CameraMetaData(char *docname);
  virtual ~CameraMetaData(void);
  xmlDocPtr doc;
  xmlParserCtxtPtr ctxt; /* the parser context */
  map<string,Camera*> cameras;
  Camera* getCamera(string make, string model, string mode);
protected:
  void addCamera(Camera* cam);
};

} // namespace RawSpeed

#endif
