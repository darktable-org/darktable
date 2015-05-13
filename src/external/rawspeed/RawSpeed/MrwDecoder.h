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
#ifndef MRW_DECODER_H
#define MRW_DECODER_H

#include "RawDecoder.h"
#include "TiffIFDBE.h"

namespace RawSpeed {

typedef struct {
  const char* code;
  const char* name;
} mrw_camera_t;

class MrwDecoder :
  public RawDecoder
{
public:
  MrwDecoder(FileMap* file);
  virtual ~MrwDecoder(void);
  virtual RawImage decodeRawInternal();
  virtual void checkSupportInternal(CameraMetaData *meta);
  virtual void decodeMetaDataInternal(CameraMetaData *meta);
  static int isMRW(FileMap* input);
protected:
  virtual void parseHeader();
  uint32 raw_width, raw_height, data_offset, packed;
  TiffIFD *tiff_meta;
  float wb_coeffs[4];
};

} // namespace RawSpeed

#endif
