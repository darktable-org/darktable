#include "StdAfx.h"
#include "Camera.h"
#include <iostream>
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

Camera::Camera(xmlDocPtr doc, xmlNodePtr cur) {
  xmlChar *key;
  key = xmlGetProp(cur, (const xmlChar *)"make");
  if (!key)
    ThrowCME("Camera XML Parser: \"make\" attribute not found.");
  make = string((const char*)key);
  xmlFree(key);

  key = xmlGetProp(cur, (const xmlChar *)"model");
  if (!key)
    ThrowCME("Camera XML Parser: \"model\" attribute not found.");
  model = string((const char*)key);
  xmlFree(key);

  supported = true;
  key = xmlGetProp(cur, (const xmlChar *)"supported");
  if (key) {
    string s = string((const char*)key);
    if (s.compare("no") == 0)
      supported = false;
    xmlFree(key);
  }

  key = xmlGetProp(cur, (const xmlChar *)"mode");
  if (key) {
    mode = string((const char*)key);
    xmlFree(key);
  } else {
    mode = string("");
  }

  key = xmlGetProp(cur, (const xmlChar *)"decoder_version");
  if (key) {
    decoderVersion = getAttributeAsInt(cur, cur->name, "decoder_version");
  } else {
    decoderVersion = 0;
  }

  cur = cur->xmlChildrenNode;
  while (cur != NULL) {
    parseCameraChild(doc, cur);
    cur = cur->next;
  }
}

Camera::Camera( const Camera* camera, uint32 alias_num)
{
  if (alias_num >= camera->aliases.size())
    ThrowCME("Camera: Internal error, alias number out of range specified.");

  make = camera->make;
  model = camera->aliases[alias_num];
  mode = camera->mode;
  cfa = camera->cfa;
  black = camera->black;
  white = camera->white;
  supported = camera->supported;
  cropSize = camera->cropSize;
  cropPos = camera->cropPos;
  decoderVersion = camera->decoderVersion;
  for (uint32 i = 0; i < camera->blackAreas.size(); i++) {
    blackAreas.push_back(camera->blackAreas[i]);
  }

  map<string,string>::const_iterator mi = camera->hints.begin();
  for (; mi != camera->hints.end(); ++mi) {
    hints.insert(make_pair((*mi).first, (*mi).second));
  }
}

Camera::~Camera(void) {
}

void Camera::parseCameraChild(xmlDocPtr doc, xmlNodePtr cur) {
  if (!xmlStrcmp(cur->name, (const xmlChar *) "CFA")) {
    if (2 != getAttributeAsInt(cur, cur->name, "width"))
      ThrowCME("Unsupported CFA size in camera %s %s", make.c_str(), model.c_str());
    if (2 != getAttributeAsInt(cur, cur->name, "height"))
      ThrowCME("Unsupported CFA size in camera %s %s", make.c_str(), model.c_str());

    cur = cur->xmlChildrenNode;
    while (cur != NULL) {
      parseCFA(doc, cur);
      cur = cur->next;
    }

    return;
  }

  if (!xmlStrcmp(cur->name, (const xmlChar *) "Crop")) {
    cropPos.x = getAttributeAsInt(cur, cur->name, "x");
    cropPos.y = getAttributeAsInt(cur, cur->name, "y");

    if (cropPos.x < 0)
      ThrowCME("Negative X axis crop specified in camera %s %s", make.c_str(), model.c_str());
    if (cropPos.y < 0)
      ThrowCME("Negative Y axis crop specified in camera %s %s", make.c_str(), model.c_str());

    cropSize.x = getAttributeAsInt(cur, cur->name, "width");
    cropSize.y = getAttributeAsInt(cur, cur->name, "height");
    return;
  }

  if (!xmlStrcmp(cur->name, (const xmlChar *) "Sensor")) {
    black = getAttributeAsInt(cur, cur->name, "black");
    white = getAttributeAsInt(cur, cur->name, "white");
    return;
  }

  if (!xmlStrcmp(cur->name, (const xmlChar *) "BlackAreas")) {
    cur = cur->xmlChildrenNode;
    while (cur != NULL) {
      parseBlackAreas(doc, cur);
      cur = cur->next;
    }
    return;
  }

  if (!xmlStrcmp(cur->name, (const xmlChar *) "Aliases")) {
    cur = cur->xmlChildrenNode;
    while (cur != NULL) {
      parseAlias(doc, cur);
      cur = cur->next;
    }
    return;
  }

  if (!xmlStrcmp(cur->name, (const xmlChar *) "Hints")) {
    cur = cur->xmlChildrenNode;
    while (cur != NULL) {
      parseHint(doc, cur);
      cur = cur->next;
    }
    return;
  }
}

void Camera::parseCFA(xmlDocPtr doc, xmlNodePtr cur) {
  xmlChar *key;
  if (!xmlStrcmp(cur->name, (const xmlChar *) "Color")) {
    int x = getAttributeAsInt(cur, cur->name, "x");
    if (x < 0 || x > 1) {
      ThrowCME("Invalid x coordinate in CFA array of in camera %s %s", make.c_str(), model.c_str());
    }

    int y = getAttributeAsInt(cur, cur->name, "y");
    if (y < 0 || y > 1) {
      ThrowCME("Invalid y coordinate in CFA array of in camera %s %s", make.c_str(), model.c_str());
    }

    key = xmlNodeListGetString(doc, cur->children, 1);
    if (!xmlStrcmp(key, (const xmlChar *) "GREEN"))
      cfa.setColorAt(iPoint2D(x, y), CFA_GREEN);
    else if (!xmlStrcmp(key, (const xmlChar *) "RED"))
      cfa.setColorAt(iPoint2D(x, y), CFA_RED);
    else if (!xmlStrcmp(key, (const xmlChar *) "BLUE"))
      cfa.setColorAt(iPoint2D(x, y), CFA_BLUE);

    xmlFree(key);

  }
}

void Camera::parseBlackAreas(xmlDocPtr doc, xmlNodePtr cur) {
  if (!xmlStrcmp(cur->name, (const xmlChar *) "Vertical")) {

    int x = getAttributeAsInt(cur, cur->name, "x");
    if (x < 0) {
      ThrowCME("Invalid x coordinate in vertical BlackArea of in camera %s %s", make.c_str(), model.c_str());
    }

    int w = getAttributeAsInt(cur, cur->name, "width");
    if (w < 0) {
      ThrowCME("Invalid width in vertical BlackArea of in camera %s %s", make.c_str(), model.c_str());
    }

    blackAreas.push_back(BlackArea(x, w, true));

  } else if (!xmlStrcmp(cur->name, (const xmlChar *) "Horizontal")) {

    int y = getAttributeAsInt(cur, cur->name, "y");
    if (y < 0) {
      ThrowCME("Invalid y coordinate in horizontal BlackArea of in camera %s %s", make.c_str(), model.c_str());
    }

    int h = getAttributeAsInt(cur, cur->name, "height");
    if (h < 0) {
      ThrowCME("Invalid width in horizontal BlackArea of in camera %s %s", make.c_str(), model.c_str());
    }

    blackAreas.push_back(BlackArea(y, h, false));

  }
}

int Camera::StringToInt(const xmlChar *in, const xmlChar *tag, const char* attribute) {
  int i;

#if defined(__unix__) || defined(__MINGW32__)
  if (EOF == sscanf((const char*)in, "%d", &i))
#else
  if (EOF == sscanf_s((const char*)in, "%d", &i))
#endif
    ThrowCME("Error parsing attribute %s in tag %s, in camera %s %s.", attribute, tag, make.c_str(), model.c_str());

  return i;
}


int Camera::getAttributeAsInt(xmlNodePtr cur , const xmlChar *tag, const char* attribute) {
  xmlChar *key = xmlGetProp(cur, (const xmlChar *)attribute);

  if (!key)
    ThrowCME("Could not find attribute %s in tag %s, in camera %s %s.", attribute, tag, make.c_str(), model.c_str());

  int i = StringToInt(key, tag, attribute);

  return i;
}

void Camera::parseAlias( xmlDocPtr doc, xmlNodePtr cur )
{
  if (!xmlStrcmp(cur->name, (const xmlChar *) "Alias")) {
    cur = cur->xmlChildrenNode;
    while (NULL != cur) {
      if (cur && !xmlStrcmp(cur->name, (const xmlChar *) "text"))
        aliases.push_back(string((const char*)cur->content));
      cur = cur->next;
    }
  }
}

void Camera::parseHint( xmlDocPtr doc, xmlNodePtr cur )
{
  if (!xmlStrcmp(cur->name, (const xmlChar *) "Hint")) {
    xmlChar *key;
    string hint_name, hint_value;
    key = xmlGetProp(cur, (const xmlChar *)"name");
    if (key)
      hint_name = string((const char*)key);
    else 
      ThrowCME("CameraMetadata: Could not find name for hint for %s %s camera.", make.c_str(), model.c_str());

    key = xmlGetProp(cur, (const xmlChar *)"value");
    if (key)
      hint_value = string((const char*)key);
    else 
      ThrowCME("CameraMetadata: Could not find value for hint %s for %s %s camera.", hint_name.c_str(), make.c_str(), model.c_str());

    hints.insert(make_pair(hint_name, hint_value));
  }
}

} // namespace RawSpeed
