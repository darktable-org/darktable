#include "StdAfx.h"
#include "DngDecoder.h"
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

DngDecoder::DngDecoder(TiffIFD *rootIFD, FileMap* file) : RawDecoder(file), mRootIFD(rootIFD) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(DNGVERSION);
  const uchar8* v = data[0]->getEntry(DNGVERSION)->getData();

  if (v[0] != 1)
    ThrowRDE("Not a supported DNG image format: v%u.%u.%u.%u", (int)v[0], (int)v[1], (int)v[2], (int)v[3]);
  if (v[1] > 3)
    ThrowRDE("Not a supported DNG image format: v%u.%u.%u.%u", (int)v[0], (int)v[1], (int)v[2], (int)v[3]);

  if ((v[0] <= 1) && (v[1] < 1))  // Prior to v1.1.xxx  fix LJPEG encoding bug
    mFixLjpeg = true;
  else
    mFixLjpeg = false;
}

DngDecoder::~DngDecoder(void) {
}

RawImage DngDecoder::decodeRaw() {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(COMPRESSION);

  if (data.empty())
    ThrowRDE("DNG Decoder: No image data found");

  // Erase the ones not with JPEG compression
  for (vector<TiffIFD*>::iterator i = data.begin(); i != data.end();) {
    int compression = (*i)->getEntry(COMPRESSION)->getShort();
    bool isSubsampled = false;
    try {
      isSubsampled = (*i)->getEntry(NEWSUBFILETYPE)->getInt() & 1; // bit 0 is on if image is subsampled
    } catch (TiffParserException) {}
    if ((compression != 7 && compression != 1) || isSubsampled) {  // Erase if subsampled, or not JPEG or uncompressed
      i = data.erase(i);
    } else {
      i++;
    }
  }

  if (data.empty())
    ThrowRDE("DNG Decoder: No RAW chunks found");

  if (data.size() > 1) {
    _RPT0(0, "Multiple RAW chunks found - using first only!");
  }

  TiffIFD* raw = data[0];
  uint32 sample_format = 1;
  uint32 bps = raw->getEntry(BITSPERSAMPLE)->getInt();

  if (raw->hasEntry(SAMPLEFORMAT))
    sample_format = raw->getEntry(SAMPLEFORMAT)->getInt();

  if (sample_format == 1)
    mRaw = RawImage::create(TYPE_USHORT16);
  else if (sample_format == 3)
    mRaw = RawImage::create(TYPE_FLOAT32);
  else
    ThrowRDE("DNG Decoder: Only 16 bit unsigned or float point data supported.");

  mRaw->isCFA = (raw->getEntry(PHOTOMETRICINTERPRETATION)->getShort() == 32803);

  if (mRaw->isCFA)
    _RPT0(0, "This is a CFA image\n");
  else
    _RPT0(0, "This is NOT a CFA image\n");

  if (sample_format == 1 && bps > 16)
    ThrowRDE("DNG Decoder: Integer precision larger than 16 bits currently not supported.");

  if (sample_format == 3 && bps != 32)
    ThrowRDE("DNG Decoder: Float point must be 32 bits per sample.");

  try {
    mRaw->dim.x = raw->getEntry(IMAGEWIDTH)->getInt();
    mRaw->dim.y = raw->getEntry(IMAGELENGTH)->getInt();
  } catch (TiffParserException) {
    ThrowRDE("DNG Decoder: Could not read basic image information.");
  }

  try {

    int compression = raw->getEntry(COMPRESSION)->getShort();
    if (mRaw->isCFA) {

      // Check if layout is OK, if present
      if (raw->hasEntry(CFALAYOUT))
        if (raw->getEntry(CFALAYOUT)->getShort() != 1)
          ThrowRDE("DNG Decoder: Unsupported CFA Layout.");

      const unsigned short* pDim = raw->getEntry(CFAREPEATPATTERNDIM)->getShortArray(); // Get the size
      const uchar8* cPat = raw->getEntry(CFAPATTERN)->getData();                 // Does NOT contain dimensions as some documents state
      /*
            if (raw->hasEntry(CFAPLANECOLOR)) {
              TiffEntry* e = raw->getEntry(CFAPLANECOLOR);
              const unsigned char* cPlaneOrder = e->getData();       // Map from the order in the image, to the position in the CFA
              printf("Planecolor: ");
              for (uint32 i = 0; i < e->count; i++) {
                printf("%u,",cPlaneOrder[i]);
              }
              printf("\n");
            }
      */
      iPoint2D cfaSize(pDim[1], pDim[0]);
      if (pDim[0] != 2)
        ThrowRDE("DNG Decoder: Unsupported CFA configuration.");
      if (pDim[1] != 2)
        ThrowRDE("DNG Decoder: Unsupported CFA configuration.");

      if (cfaSize.area() != raw->getEntry(CFAPATTERN)->count)
        ThrowRDE("DNG Decoder: CFA pattern dimension and pattern count does not match: %d.");

      for (int y = 0; y < cfaSize.y; y++) {
        for (int x = 0; x < cfaSize.x; x++) {
          uint32 c1 = cPat[x+y*cfaSize.x];
          CFAColor c2;
          switch (c1) {
            case 0:
              c2 = CFA_RED; break;
            case 1:
              c2 = CFA_GREEN; break;
            case 2:
              c2 = CFA_BLUE; break;
            default:
              c2 = CFA_UNKNOWN;
              ThrowRDE("DNG Decoder: Unsupported CFA Color.");
          }
          mRaw->cfa.setColorAt(iPoint2D(x, y), c2);
        }
      }
    }


    // Now load the image
    if (compression == 1) {  // Uncompressed.
      try {
        if (!mRaw->isCFA)
        {
          uint32 cpp = raw->getEntry(SAMPLESPERPIXEL)->getInt();
          ThrowRDE("DNG Decoder: More than 4 samples per pixel is not supported.");
          mRaw->setCpp(cpp);
        }
        uint32 nslices = raw->getEntry(STRIPOFFSETS)->count;
        TiffEntry *TEoffsets = raw->getEntry(STRIPOFFSETS);
        TiffEntry *TEcounts = raw->getEntry(STRIPBYTECOUNTS);
        const uint32* offsets = TEoffsets->getIntArray();
        const uint32* counts = TEcounts->getIntArray();
        uint32 yPerSlice = raw->getEntry(ROWSPERSTRIP)->getInt();
        uint32 width = raw->getEntry(IMAGEWIDTH)->getInt();
        uint32 height = raw->getEntry(IMAGELENGTH)->getInt();
          
        if (TEcounts->count != TEoffsets->count) {
          ThrowRDE("DNG Decoder: Byte count number does not match strip size: count:%u, strips:%u ", TEcounts->count, TEoffsets->count);
        }

        uint32 offY = 0;
        vector<DngStrip> slices;
        for (uint32 s = 0; s < nslices; s++) {
          DngStrip slice;
          slice.offset = offsets[s];
          slice.count = counts[s];
          slice.offsetY = offY;
          if (offY + yPerSlice > height)
            slice.h = height - offY;
          else
            slice.h = yPerSlice;

          offY += yPerSlice;

          if (mFile->isValid(slice.offset + slice.count)) // Only decode if size is valid
            slices.push_back(slice);
        }

        mRaw->createData();

        for (uint32 i = 0; i < slices.size(); i++) {
          DngStrip slice = slices[i];
          ByteStream in(mFile->getData(slice.offset), slice.count);
          iPoint2D size(width, slice.h);
          iPoint2D pos(0, slice.offsetY);

          bool big_endian = (raw->endian == big);
          // DNG spec says that if not 8 or 16 bit/sample, always use big endian
          if (bps != 8 && bps != 16)
            big_endian = true;
          try {
            readUncompressedRaw(in, size, pos, width*bps / 8, bps, big_endian);
          } catch(IOException ex) {
            if (i > 0)
              errors.push_back(_strdup(ex.what()));
            else
              ThrowRDE("DNG decoder: IO error occurred in first slice, unable to decode more. Error is: %s", ex.what());
          }
        }

      } catch (TiffParserException) {
        ThrowRDE("DNG Decoder: Unsupported format, uncompressed with no strips.");
      }
    } else if (compression == 7) {
      try {
        // Let's try loading it as tiles instead

        if (!mRaw->isCFA) {
          mRaw->setCpp(raw->getEntry(SAMPLESPERPIXEL)->getInt());
        }
        mRaw->createData();

        if (sample_format != 1)
           ThrowRDE("DNG Decoder: Only 16 bit unsigned data supported for compressed data.");

        DngDecoderSlices slices(mFile, mRaw);
        if (raw->hasEntry(TILEOFFSETS)) {
          uint32 tilew = raw->getEntry(TILEWIDTH)->getInt();
          uint32 tileh = raw->getEntry(TILELENGTH)->getInt();
          if (!tilew || !tileh)
            ThrowRDE("DNG Decoder: Invalid tile size");

          uint32 tilesX = (mRaw->dim.x + tilew - 1) / tilew;
          uint32 tilesY = (mRaw->dim.y + tileh - 1) / tileh;
          uint32 nTiles = tilesX * tilesY;

          TiffEntry *TEoffsets = raw->getEntry(TILEOFFSETS);
          const uint32* offsets = TEoffsets->getIntArray();

          TiffEntry *TEcounts = raw->getEntry(TILEBYTECOUNTS);
          const uint32* counts = TEcounts->getIntArray();

          if (TEoffsets->count != TEcounts->count || TEoffsets->count != nTiles)
            ThrowRDE("DNG Decoder: Tile count mismatch: offsets:%u count:%u, calculated:%u", TEoffsets->count, TEcounts->count, nTiles);

          slices.mFixLjpeg = mFixLjpeg;

          for (uint32 y = 0; y < tilesY; y++) {
            for (uint32 x = 0; x < tilesX; x++) {
              DngSliceElement e(offsets[x+y*tilesX], counts[x+y*tilesX], tilew*x, tileh*y);
              e.mUseBigtable = tilew * tileh > 1024 * 1024;
              slices.addSlice(e);
            }
          }
        } else {  // Strips
          TiffEntry *TEoffsets = raw->getEntry(STRIPOFFSETS);
          TiffEntry *TEcounts = raw->getEntry(STRIPBYTECOUNTS);

          const uint32* offsets = TEoffsets->getIntArray();
          const uint32* counts = TEcounts->getIntArray();
          uint32 yPerSlice = raw->getEntry(ROWSPERSTRIP)->getInt();

          if (TEcounts->count != TEoffsets->count) {
            ThrowRDE("DNG Decoder: Byte count number does not match strip size: count:%u, stips:%u ", TEcounts->count, TEoffsets->count);
          }

          if (yPerSlice == 0 || yPerSlice > (uint32)mRaw->dim.y)
            ThrowRDE("DNG Decoder: Invalid y per slice");

          uint32 offY = 0;
          for (uint32 s = 0; s < TEcounts->count; s++) {
            DngSliceElement e(offsets[s], counts[s], 0, offY);
            e.mUseBigtable = yPerSlice * mRaw->dim.y > 1024 * 1024;
            offY += yPerSlice;

            if (mFile->isValid(e.byteOffset + e.byteCount)) // Only decode if size is valid
              slices.addSlice(e);
          }
        }
        uint32 nSlices = slices.size();
        if (!nSlices)
          ThrowRDE("DNG Decoder: No valid slices found.");

        slices.startDecoding();

        if (!slices.errors.empty())
          errors = slices.errors;

        if (errors.size() >= nSlices)
          ThrowRDE("DNG Decoding: Too many errors encountered. Giving up.\nFirst Error:%s", errors[0]);
      } catch (TiffParserException e) {
        ThrowRDE("DNG Decoder: Unsupported format, tried strips and tiles:\n%s", e.what());
      }
    } else {
      ThrowRDE("DNG Decoder: Unknown compression: %u", compression);
    }
  } catch (TiffParserException e) {
    ThrowRDE("DNG Decoder: Image could not be read:\n%s", e.what());
  }
  iPoint2D new_size(mRaw->dim.x, mRaw->dim.y);

  // Crop
  if (raw->hasEntry(ACTIVEAREA)) {
    const uint32 *corners = raw->getEntry(ACTIVEAREA)->getIntArray();
    if (iPoint2D(corners[1], corners[0]).isThisInside(mRaw->dim)) {
      if (iPoint2D(corners[3], corners[2]).isThisInside(mRaw->dim)) {
        iPoint2D top_left(corners[1], corners[0]);
        new_size = iPoint2D(corners[3] - corners[1], corners[2] - corners[0]);
        mRaw->subFrame(top_left, new_size);
      }
    }

  } else if (raw->hasEntry(DEFAULTCROPORIGIN)) {

    iPoint2D top_left(0, 0);

    if (raw->getEntry(DEFAULTCROPORIGIN)->type == TIFF_LONG) {
      const uint32* tl = raw->getEntry(DEFAULTCROPORIGIN)->getIntArray();
      const uint32* sz = raw->getEntry(DEFAULTCROPSIZE)->getIntArray();
      if (iPoint2D(tl[0], tl[1]).isThisInside(mRaw->dim) && iPoint2D(sz[0], sz[1]).isThisInside(mRaw->dim)) {
        top_left = iPoint2D(tl[0], tl[1]);
        new_size = iPoint2D(sz[0], sz[1]);
      }
    } else if (raw->getEntry(DEFAULTCROPORIGIN)->type == TIFF_SHORT) {
      const ushort16* tl = raw->getEntry(DEFAULTCROPORIGIN)->getShortArray();
      const ushort16* sz = raw->getEntry(DEFAULTCROPSIZE)->getShortArray();
      if (iPoint2D(tl[0], tl[1]).isThisInside(mRaw->dim) && iPoint2D(sz[0], sz[1]).isThisInside(mRaw->dim)) {
        top_left = iPoint2D(tl[0], tl[1]);
        new_size = iPoint2D(sz[0], sz[1]);
      }
    }
    mRaw->subFrame(top_left, new_size);
    if (top_left.x %2 == 1)
      mRaw->cfa.shiftLeft();
    if (top_left.y %2 == 1)
      mRaw->cfa.shiftDown();
  }
  // Linearization

  if (raw->hasEntry(LINEARIZATIONTABLE)) {
    const ushort16* intable = raw->getEntry(LINEARIZATIONTABLE)->getShortArray();
    uint32 len =  raw->getEntry(LINEARIZATIONTABLE)->count;
    ushort16 table[65536];
    for (uint32 i = 0; i < 65536 ; i++) {
      if (i < len)
        table[i] = intable[i];
      else
        table[i] = intable[len-1];
    }
    for (int y = 0; y < mRaw->dim.y; y++) {
      uint32 cw = mRaw->dim.x * mRaw->getCpp();
      ushort16* pixels = (ushort16*)mRaw->getData(0, y);
      for (uint32 x = 0; x < cw; x++) {
        pixels[x]  = table[pixels[x]];
      }
    }
  }

 // Default white level is (2 ** BitsPerSample) - 1
  mRaw->whitePoint = (1 >> raw->getEntry(BITSPERSAMPLE)->getShort()) - 1;

  if (raw->hasEntry(WHITELEVEL)) {
    TiffEntry *whitelevel = raw->getEntry(WHITELEVEL);
    if (whitelevel->isInt())
      mRaw->whitePoint = whitelevel->getInt();
  }
  // Set black
  setBlack(raw);

  return mRaw;
}

void DngDecoder::decodeMetaData(CameraMetaData *meta) {
}

void DngDecoder::checkSupport(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("DNG Support check: Model name found");

  // We set this, since DNG's are not explicitly added. 
  failOnUnknown = FALSE;
  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  this->checkCameraSupported(meta, make, model, "dng");
}

/* Decodes DNG masked areas into blackareas in the image */
bool DngDecoder::decodeMaskedAreas(TiffIFD* raw) {
  TiffEntry *masked = raw->getEntry(MASKEDAREAS);
  int nrects = masked->count/4;

  if (0 == nrects)
    return FALSE;

  /* Since we may both have short or int, copy it to int array. */
  int *rects = new int[nrects*4];
  if (masked->type == TIFF_SHORT) {
    const ushort16* r = masked->getShortArray();
    for (int i = 0; i< nrects*4; i++)
      rects[i] = r[i];
  } else if (masked->type == TIFF_LONG) {
    const uint32* r = masked->getIntArray();
    for (int i = 0; i< nrects*4; i++)
      rects[i] = r[i];
  } else {
    delete[] rects;
    return FALSE;
  }

  iPoint2D top = mRaw->getCropOffset();

  for (int i=0; i<nrects; i++) {
    iPoint2D topleft = iPoint2D(rects[i*4+1], rects[i*4]);
    iPoint2D bottomright = iPoint2D(rects[i*4+3], rects[i*4+2]);
    // Is this a horizontal box, only add it if it covers the active width of the image
    if (topleft.x <= top.x && bottomright.x >= (mRaw->dim.x+top.x))
      mRaw->blackAreas.push_back(BlackArea(topleft.y, bottomright.y-topleft.y, FALSE));
    // Is it a vertical box, only add it if it covers the active height of the image
    else if (topleft.y <= top.y && bottomright.y >= (mRaw->dim.y+top.y)) {
        mRaw->blackAreas.push_back(BlackArea(topleft.x, bottomright.x-topleft.x, TRUE));
    }
  }
  delete[] rects;
  return !!mRaw->blackAreas.size();
}

bool DngDecoder::decodeBlackLevels(TiffIFD* raw) {
  iPoint2D blackdim(1,1);
  if (raw->hasEntry(BLACKLEVELREPEATDIM)) {
    const ushort16 *dim = raw->getEntry(BLACKLEVELREPEATDIM)->getShortArray();
    blackdim = iPoint2D(dim[0], dim[1]);
  }

  if (blackdim.x == 0 || blackdim.y == 0)
    return FALSE;

  if (!raw->hasEntry(BLACKLEVEL))
    return TRUE;

  if (mRaw->getCpp() != 1)
    return FALSE;

  TiffEntry* black_entry = raw->getEntry(BLACKLEVEL);
  const uint32* iblackarray = NULL;
  const ushort16* sblackarray = NULL;
  if (black_entry->type == TIFF_SHORT)
    sblackarray = black_entry->getShortArray();
  else
    iblackarray = black_entry->getIntArray();

  if (blackdim.x < 2 || blackdim.y < 2) {
    // We so not have enough to fill all individually, read a single and copy it
    for (int y = 0; y < 2; y++) {
      for (int x = 0; x < 2; x++) {
        int offset = 0;
        if (black_entry->type == TIFF_RATIONAL) {
          if (iblackarray[offset*2+1])
            mRaw->blackLevelSeparate[y*2+x] = iblackarray[offset*2] / iblackarray[offset*2+1];
          else
            mRaw->blackLevelSeparate[y*2+x] = 0;
        } else if (black_entry->type == TIFF_LONG) {
          mRaw->blackLevelSeparate[y*2+x] = iblackarray[offset];
        } else if (black_entry->type == TIFF_SHORT) {
          mRaw->blackLevelSeparate[y*2+x] = sblackarray[offset];
        }
      }
    }
  } else {
    for (int y = 0; y < 2; y++) {
      for (int x = 0; x < 2; x++) {
        int offset = y*blackdim.x+x;
        if (black_entry->type == TIFF_RATIONAL) {
          if (iblackarray[offset*2+1])
            mRaw->blackLevelSeparate[y*2+x] = iblackarray[offset*2] / iblackarray[offset*2+1];
          else
            mRaw->blackLevelSeparate[y*2+x] = 0;
        } else if (black_entry->type == TIFF_LONG) {
          mRaw->blackLevelSeparate[y*2+x] = iblackarray[offset];
        } else if (black_entry->type == TIFF_SHORT) {
          mRaw->blackLevelSeparate[y*2+x] = sblackarray[offset];
        }
      }
    }
  }

  // DNG Spec says we must add black in deltav and deltah
  if (raw->hasEntry(BLACKLEVELDELTAV)) {
    const int *blackarrayv = (const int*)raw->getEntry(BLACKLEVELDELTAV)->getIntArray();
    float black_sum[2] = {0.0f, 0.0f};
    for (int i = 0; i < mRaw->dim.y; i++)
      if (blackarrayv[i*2+1])
        black_sum[i&1] += blackarrayv[i*2] / blackarrayv[i*2+1];

    for (int i = 0; i < 4; i++)
      mRaw->blackLevelSeparate[i] += (int)(black_sum[i>>1] / (float)mRaw->dim.y * 2.0f);
  } 

  if (raw->hasEntry(BLACKLEVELDELTAH)){
    const int *blackarrayh = (const int*)raw->getEntry(BLACKLEVELDELTAH)->getIntArray();
    float black_sum[2] = {0.0f, 0.0f};
    for (int i = 0; i < mRaw->dim.x; i++)
      if (blackarrayh[i*2+1])
        black_sum[i&1] += blackarrayh[i*2] / blackarrayh[i*2+1];

    for (int i = 0; i < 4; i++)
      mRaw->blackLevelSeparate[i] += (int)(black_sum[i&1] / (float)mRaw->dim.x * 2.0f);
  }
  return TRUE;
}

void DngDecoder::setBlack(TiffIFD* raw) {

  if (raw->hasEntry(MASKEDAREAS))
    if (decodeMaskedAreas(raw))
      return;

  // Black defaults to 0
  memset(mRaw->blackLevelSeparate,0,sizeof(mRaw->blackLevelSeparate)); 

  if (raw->hasEntry(BLACKLEVEL))
    decodeBlackLevels(raw);
}
} // namespace RawSpeed
