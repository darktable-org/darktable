#include "StdAfx.h"
#include "CameraMetaData.h"
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

CameraMetaData::CameraMetaData() {
}

CameraMetaData::CameraMetaData(char *docname) {
  ctxt = xmlNewParserCtxt();
  if (ctxt == NULL) {
    ThrowCME("CameraMetaData:Could not initialize context.");
  }

  xmlResetLastError();
  doc = xmlCtxtReadFile(ctxt, docname, NULL, XML_PARSE_DTDVALID);

  if (doc == NULL) {
      ThrowCME("CameraMetaData: XML Document could not be parsed successfully. Error was: %s", ctxt->lastError.message);
  }

  if (ctxt->valid == 0) {
    if (ctxt->lastError.code == 0x5e) {
      printf("CameraMetaData: Unable to locate DTD, attempting to ignore.");
    } else {
      ThrowCME("CameraMetaData: XML file does not validate. DTD Error was: %s", ctxt->lastError.message);
    }
  }

  xmlNodePtr cur;
  cur = xmlDocGetRootElement(doc);
  if (xmlStrcmp(cur->name, (const xmlChar *) "Cameras")) {
    ThrowCME("CameraMetaData: XML document of the wrong type, root node is not cameras.");
    return;
  }

  cur = cur->xmlChildrenNode;
  while (cur != NULL) {
    if ((!xmlStrcmp(cur->name, (const xmlChar *)"Camera"))) {
      Camera *camera = new Camera(doc, cur);
      addCamera(camera);

      // Create cameras for aliases.
      for (uint32 i = 0; i < camera->aliases.size(); i++) {
        addCamera(new Camera(camera, i));
      }
    }
    cur = cur->next;
  }
  if (doc)
    xmlFreeDoc(doc);
  doc = 0;
  if (ctxt)
    xmlFreeParserCtxt(ctxt);
  ctxt = 0;
}

CameraMetaData::~CameraMetaData(void) {
  map<string, Camera*>::iterator i = cameras.begin();
  for (; i != cameras.end(); i++) {
    delete((*i).second);
  }
  if (doc)
    xmlFreeDoc(doc);
  doc = 0;
  if (ctxt)
    xmlFreeParserCtxt(ctxt);
  ctxt = 0;
}

Camera* CameraMetaData::getCamera(string make, string model, string mode) {
  string id = string(make).append(model).append(mode);
  if (cameras.end() == cameras.find(id))
    return NULL;
  return cameras[id];
}

void CameraMetaData::addCamera( Camera* cam )
{
  string id = string(cam->make).append(cam->model).append(cam->mode);
  if (cameras.end() != cameras.find(id))
    printf("CameraMetaData: Duplicate entry found for camera: %s %s, Skipping!\n", cam->make.c_str(), cam->model.c_str());
  else
    cameras[id] = cam;
}

} // namespace RawSpeed
