#include "StdAfx.h"
#include "NakedDecoder.h"
/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post
    Copyright (C) 2014 Pedro Côrte-Real

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

NakedDecoder::NakedDecoder(FileMap* file, Camera* c) :
    RawDecoder(file) {
  cam = c;
}

NakedDecoder::~NakedDecoder(void) {
}

RawImage NakedDecoder::decodeRawInternal() {
  uint32 width=0, height=0, filesize=0, bits=0, offset=0;
  if(cam->hints.find("full_width") != cam->hints.end()) {
    string tmp = cam->hints.find(string("full_width"))->second;
    width = (uint32) atoi(tmp.c_str());
  } else
    ThrowRDE("Naked: couldn't find width");

  if(cam->hints.find("full_height") != cam->hints.end()) {
    string tmp = cam->hints.find(string("full_height"))->second;
    height = (uint32) atoi(tmp.c_str());
  } else
    ThrowRDE("Naked: couldn't find height");

  if(cam->hints.find("filesize") != cam->hints.end()) {
    string tmp = cam->hints.find(string("filesize"))->second;
    filesize = (uint32) atoi(tmp.c_str());
  } else
    ThrowRDE("Naked: couldn't find filesize");

  if(cam->hints.find("offset") != cam->hints.end()) {
    string tmp = cam->hints.find(string("offset"))->second;
    offset = (uint32) atoi(tmp.c_str());
  }

  if(cam->hints.find("bits") != cam->hints.end()) {
    string tmp = cam->hints.find(string("bits"))->second;
    bits = (uint32) atoi(tmp.c_str());
  } else
    bits = (filesize-offset)*8/width/height;

  BitOrder bo = BitOrder_Jpeg16;  // Default
  if(cam->hints.find("order") != cam->hints.end()) {
    string tmp = cam->hints.find(string("order"))->second;
    if (tmp.compare("plain") == 0) {
      bo = BitOrder_Plain;
    } else if (tmp.compare("jpeg") == 0) {
      bo = BitOrder_Jpeg;
    } else if (tmp.compare("jpeg16") == 0) {
      bo = BitOrder_Jpeg16;    
    } else if (tmp.compare("jpeg32") == 0) {
      bo = BitOrder_Jpeg32;    
    }
  }

  mRaw->dim = iPoint2D(width, height);
  mRaw->createData();

  ByteStream input(mFile, offset);
  iPoint2D pos(0, 0);
  readUncompressedRaw(input, mRaw->dim, pos, width*bits/8, bits, bo);

  return mRaw;
}

void NakedDecoder::checkSupportInternal(CameraMetaData *meta) {
  this->checkCameraSupported(meta, cam->make, cam->model, cam->mode);
}

void NakedDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  setMetaData(meta, cam->make, cam->model, cam->mode, 0);
}

} // namespace RawSpeed
