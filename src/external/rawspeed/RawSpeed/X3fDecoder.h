#ifndef X3F_DECODER_H
#define X3F_DECODER_H
#include "RawDecoder.h"
#include "X3fParser.h"

/* 
RawSpeed - RAW file decoder.

Copyright (C) 2013 Klaus Post

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



class X3fDecoder :
  public RawDecoder
{
public:
  X3fDecoder(FileMap* file);
  virtual ~X3fDecoder(void);
  virtual RawImage decodeRawInternal();
  virtual void decodeMetaDataInternal(CameraMetaData *meta);
  virtual void checkSupportInternal(CameraMetaData *meta);
  virtual FileMap* getCompressedData();
  vector<X3fDirectory> mDirectory;
  vector<X3fImage> mImages;
  X3fPropertyCollection mProperties;

protected:
  virtual void decodeThreaded(RawDecoderThread* t);
  void readDirectory();
  string getId();
  ByteStream *bytes;
  bool hasProp(const char* key) { return mProperties.props.find(key) != mProperties.props.end();};
  string getProp(const char* key);
  void decompressSigma( X3fImage &image );
  void createSigmaTable(ByteStream *bytes, int codes);
  int SigmaDecode(BitPumpMSB *bits);
  string getIdAsString(ByteStream *bytes);
  void SigmaSkipOne(BitPumpMSB *bits);
  boolean readName();
  X3fImage *curr_image;
  int pred[3];
  uint32 plane_sizes[3];
  uint32 plane_offset[3];
  iPoint2D planeDim[3];
  uchar8 code_table[256];
  int32 big_table[1<<14];
  uint32 *line_offsets;
  ushort16 *huge_table;
  short curve[1024];
  uint32 max_len;
  string camera_make;
  string camera_model;
};

} // namespace RawSpeed
#endif