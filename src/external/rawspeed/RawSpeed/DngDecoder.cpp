#include "StdAfx.h"
#include "DngDecoder.h"
#include <iostream>

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

DngDecoder::DngDecoder(TiffIFD *rootIFD, FileMap* file) : RawDecoder(file), mRootIFD(rootIFD) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(DNGVERSION);
  const uchar8* v = data[0]->getEntry(DNGVERSION)->getData();

  if (v[0] != 1)
    ThrowRDE("Not a supported DNG image format: v%u.%u.%u.%u", (int)v[0], (int)v[1], (int)v[2], (int)v[3]);
//  if (v[1] > 4)
//    ThrowRDE("Not a supported DNG image format: v%u.%u.%u.%u", (int)v[0], (int)v[1], (int)v[2], (int)v[3]);

  if ((v[0] <= 1) && (v[1] < 1))  // Prior to v1.1.xxx  fix LJPEG encoding bug
    mFixLjpeg = true;
  else
    mFixLjpeg = false;
}

DngDecoder::~DngDecoder(void) {
  if (mRootIFD)
    delete mRootIFD;
  mRootIFD = NULL;
}

RawImage DngDecoder::decodeRawInternal() {
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
    if ((compression != 7 && compression != 1 && compression != 0x884c) || isSubsampled) {  // Erase if subsampled, or not JPEG or uncompressed
      i = data.erase(i);
    } else {
      ++i;
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

  int compression = -1;

  try {
    compression = raw->getEntry(COMPRESSION)->getShort();
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
      mRaw->cfa.setSize(cfaSize);
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
            case 3:
              c2 = CFA_CYAN; break;
            case 4:
              c2 = CFA_MAGENTA; break;
            case 5:
              c2 = CFA_YELLOW; break;
            case 6:
              c2 = CFA_WHITE; break;
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
        uint32 cpp = raw->getEntry(SAMPLESPERPIXEL)->getInt();
        if (cpp > 4)
          ThrowRDE("DNG Decoder: More than 4 samples per pixel is not supported.");
        mRaw->setCpp(cpp);

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
          if (hints.find("ignore_bytecount") != hints.end())
            slice.count = mFile->getSize() - slice.offset;
          ByteStream in(mFile->getData(slice.offset), slice.count);
          iPoint2D size(width, slice.h);
          iPoint2D pos(0, slice.offsetY);

          bool big_endian = (raw->endian == big);
          // DNG spec says that if not 8 or 16 bit/sample, always use big endian
          if (bps != 8 && bps != 16)
            big_endian = true;
          try {
            readUncompressedRaw(in, size, pos, mRaw->getCpp()* width * bps / 8, bps, big_endian ? BitOrder_Jpeg : BitOrder_Plain);
          } catch(IOException &ex) {
            if (i > 0)
              mRaw->setError(ex.what());
            else
              ThrowRDE("DNG decoder: IO error occurred in first slice, unable to decode more. Error is: %s", ex.what());
          }
        }

      } catch (TiffParserException) {
        ThrowRDE("DNG Decoder: Unsupported format, uncompressed with no strips.");
      }
    } else if (compression == 7 || compression == 0x884c) {
      try {
        // Let's try loading it as tiles instead

        mRaw->setCpp(raw->getEntry(SAMPLESPERPIXEL)->getInt());
        mRaw->createData();

        if (sample_format != 1)
           ThrowRDE("DNG Decoder: Only 16 bit unsigned data supported for compressed data.");

        DngDecoderSlices slices(mFile, mRaw, compression);
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

        if (mRaw->errors.size() >= nSlices)
          ThrowRDE("DNG Decoding: Too many errors encountered. Giving up.\nFirst Error:%s", mRaw->errors[0]);
      } catch (TiffParserException e) {
        ThrowRDE("DNG Decoder: Unsupported format, tried strips and tiles:\n%s", e.what());
      }
    } else {
      ThrowRDE("DNG Decoder: Unknown compression: %u", compression);
    }
  } catch (TiffParserException e) {
    ThrowRDE("DNG Decoder: Image could not be read:\n%s", e.what());
  }

  // Fetch the white balance
  if (mRootIFD->hasEntryRecursive(ASSHOTNEUTRAL)) {
    TiffEntry *as_shot_neutral = mRootIFD->getEntryRecursive(ASSHOTNEUTRAL);
    if (as_shot_neutral->count == 3) {
      if (as_shot_neutral->type == TIFF_SHORT) {
        // Commented out because I didn't have an example file to verify it's correct
        /* const ushort16 *tmp = as_shot_neutral->getShortArray();
        for (uint32 i=0; i<3; i++)
          mRaw->metadata.wbCoeffs[i] = tmp[i];*/
      } else if (as_shot_neutral->type == TIFF_RATIONAL) {
        const uint32 *tmp = as_shot_neutral->getIntArray();
        for (uint32 i=0; i<3; i++)
          mRaw->metadata.wbCoeffs[i] = (tmp[i*2+1]*1.0f)/tmp[i*2];
      }
    }
  } else if (mRootIFD->hasEntryRecursive(ASSHOTWHITEXY)) {
    // Commented out because I didn't have an example file to verify it's correct
    /* TiffEntry *as_shot_white_xy = mRootIFD->getEntryRecursive(ASSHOTWHITEXY);
    if (as_shot_white_xy->count == 2) {
      const uint32 *tmp = as_shot_white_xy->getIntArray();
      mRaw->metadata.wbCoeffs[0] = tmp[1]/tmp[0];
      mRaw->metadata.wbCoeffs[1] = tmp[3]/tmp[2];
      mRaw->metadata.wbCoeffs[2] = 1 - mRaw->metadata.wbCoeffs[0] - mRaw->metadata.wbCoeffs[1];

      const float d65_white[3] = { 0.950456, 1, 1.088754 };
      for (uint32 i=0; i<3; i++)
          mRaw->metadata.wbCoeffs[i] /= d65_white[i];
    } */
  }

  // Crop
  if (raw->hasEntry(ACTIVEAREA)) {
    iPoint2D new_size(mRaw->dim.x, mRaw->dim.y);

    TiffEntry *active_area = raw->getEntry(ACTIVEAREA);
    if (active_area->count != 4)
      ThrowRDE("DNG: active area has %d values instead of 4", active_area->count);

    const uint32 *corners = NULL;
    if (active_area->type == TIFF_LONG) {
      corners = active_area->getIntArray();
    } else if (active_area->type == TIFF_SHORT) {
      const ushort16 *short_corners = active_area->getShortArray();
      uint32 *tmp = new uint32[4];
      for (uint32 i=0; i<4; i++)
        tmp[i] = short_corners[i];
      corners = tmp;
    }
    else {
      ThrowRDE("DNG: active area has to be LONG or SHORT");
    }
    if (iPoint2D(corners[1], corners[0]).isThisInside(mRaw->dim)) {
      if (iPoint2D(corners[3], corners[2]).isThisInside(mRaw->dim)) {
        iRectangle2D crop(corners[1], corners[0], corners[3] - corners[1], corners[2] - corners[0]);
        mRaw->subFrame(crop);
      }
    }
  }

  if (raw->hasEntry(DEFAULTCROPORIGIN) && raw->hasEntry(DEFAULTCROPSIZE)) {
    iRectangle2D cropped(0, 0, mRaw->dim.x, mRaw->dim.y);
    TiffEntry *origin_entry = raw->getEntry(DEFAULTCROPORIGIN);
    TiffEntry *size_entry = raw->getEntry(DEFAULTCROPSIZE);

    /* Read crop position */
    if (origin_entry->type == TIFF_LONG) {
      const uint32* tl = origin_entry->getIntArray();
      if (iPoint2D(tl[0], tl[1]).isThisInside(mRaw->dim))
        cropped = iRectangle2D(tl[0], tl[1], 0, 0);
    } else if (origin_entry->type == TIFF_SHORT) {
      const ushort16* tl = origin_entry->getShortArray();
      if (iPoint2D(tl[0], tl[1]).isThisInside(mRaw->dim))
        cropped = iRectangle2D(tl[0], tl[1], 0, 0);
    } else if (origin_entry->type == TIFF_RATIONAL) {
      // Crop as rational numbers, really?
      const uint32* tl = origin_entry->getIntArray();
      if (tl[1] && tl[3]) {
        if (iPoint2D(tl[0]/tl[1],tl[2]/tl[3]).isThisInside(mRaw->dim))
          cropped = iRectangle2D(tl[0]/tl[1], tl[2]/tl[3], 0, 0);
      }
    }
    cropped.dim = mRaw->dim - cropped.pos;
    // Read size
    if (size_entry->type == TIFF_LONG) {
      const uint32* sz = size_entry->getIntArray();
      iPoint2D size(sz[0], sz[1]);
      if ((size + cropped.pos).isThisInside(mRaw->dim))
        cropped.dim = size;      
    } else if (size_entry->type == TIFF_SHORT) {
      const ushort16* sz = size_entry->getShortArray();
      iPoint2D size(sz[0], sz[1]);
      if ((size + cropped.pos).isThisInside(mRaw->dim))
        cropped.dim = size;      
    } else if (size_entry->type == TIFF_RATIONAL) {
      // Crop as rational numbers, really?
      const uint32* sz = size_entry->getIntArray();
      if (sz[1] && sz[3]) {
        iPoint2D size(sz[0]/sz[1], sz[2]/sz[3]);
        if ((size + cropped.pos).isThisInside(mRaw->dim))
          cropped.dim = size;      
      }
    }
    if (!cropped.hasPositiveArea())
      ThrowRDE("DNG Decoder: No positive crop area");

    mRaw->subFrame(cropped);
    if (mRaw->isCFA && cropped.pos.x %2 == 1)
      mRaw->cfa.shiftLeft();
    if (mRaw->isCFA && cropped.pos.y %2 == 1)
      mRaw->cfa.shiftDown();
  }
  if (mRaw->dim.area() <= 0)
    ThrowRDE("DNG Decoder: No image left after crop");

  // Apply stage 1 opcodes
  if (applyStage1DngOpcodes) {
    if (raw->hasEntry(OPCODELIST1))
    {
      // Apply stage 1 codes
      try{
        DngOpcodes codes(raw->getEntry(OPCODELIST1));
        mRaw = codes.applyOpCodes(mRaw);
      } catch (RawDecoderException &e) {
        // We push back errors from the opcode parser, since the image may still be usable
        mRaw->setError(e.what());
      }
    }
  }

  // Linearization
  if (raw->hasEntry(LINEARIZATIONTABLE)) {
    const ushort16* intable = raw->getEntry(LINEARIZATIONTABLE)->getShortArray();
    uint32 len =  raw->getEntry(LINEARIZATIONTABLE)->count;
    mRaw->setTable(intable, len, !uncorrectedRawValues);
    if (!uncorrectedRawValues) {
      mRaw->sixteenBitLookup();
      mRaw->setTable(NULL);
    }

    if (0) {
      // Test average for bias
      uint32 cw = mRaw->dim.x * mRaw->getCpp();
      ushort16* pixels = (ushort16*)mRaw->getData(0, 500);
      float avg = 0.0f;
      for (uint32 x = 0; x < cw; x++) {
        avg += (float)pixels[x];
      }
      printf("Average:%f\n", avg/(float)cw);    
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

  // Apply opcodes to lossy DNG 
  if (compression == 0x884c && !uncorrectedRawValues) {
    if (raw->hasEntry(OPCODELIST2))
    {
      // We must apply black/white scaling
      mRaw->scaleBlackWhite();
      // Apply stage 2 codes
      try{
        DngOpcodes codes(raw->getEntry(OPCODELIST2));
        mRaw = codes.applyOpCodes(mRaw);
      } catch (RawDecoderException &e) {
        // We push back errors from the opcode parser, since the image may still be usable
        mRaw->setError(e.what());
      }
      mRaw->blackAreas.clear();
      mRaw->blackLevel = 0;
      mRaw->blackLevelSeparate[0] = mRaw->blackLevelSeparate[1] = mRaw->blackLevelSeparate[2] = mRaw->blackLevelSeparate[3] = 0;
      mRaw->whitePoint = 65535;
    }
  }
  return mRaw;
}

void DngDecoder::decodeMetaDataInternal(CameraMetaData *meta) {
  if (mRootIFD->hasEntryRecursive(ISOSPEEDRATINGS))
    mRaw->metadata.isoSpeed = mRootIFD->getEntryRecursive(ISOSPEEDRATINGS)->getInt();

}

/* DNG Images are assumed to be decodable unless explicitly set so */
void DngDecoder::checkSupportInternal(CameraMetaData *meta) {
  if (!(mRootIFD->hasEntryRecursive(MAKE) && mRootIFD->hasEntryRecursive(MODEL))) {
    // Check "Unique Camera Model" instead, uses this for both make + model.
    if (mRootIFD->hasEntryRecursive(UNIQUECAMERAMODEL)) {
      string unique = mRootIFD->getEntryRecursive(UNIQUECAMERAMODEL)->getString();
      this->checkCameraSupported(meta, unique, unique, "dng");
      return;
    } else {
      // If we don't have make/model we cannot tell, but still assume yes.
      return;
    }
  }

  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

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
