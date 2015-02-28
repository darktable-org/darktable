#include "StdAfx.h"
#include "Cr2Decoder.h"
#include "TiffParserHeaderless.h"
#include "ByteStreamSwap.h"

/*
    RawSpeed - RAW file decoder.

    Copyright (C) 2009-2014 Klaus Post
    Copyright (C) 2015 Roman Lebedev

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

Cr2Decoder::Cr2Decoder(TiffIFD *rootIFD, FileMap* file) :
    RawDecoder(file), mRootIFD(rootIFD) {
  decoderVersion = 5;
}

Cr2Decoder::~Cr2Decoder(void) {
  if (mRootIFD)
    delete mRootIFD;
  mRootIFD = NULL;
}

RawImage Cr2Decoder::decodeRawInternal() {
  if(hints.find("old_format") != hints.end()) {
    uint32 off = 0;
    if (mRootIFD->getEntryRecursive((TiffTag)0x81))
      off = mRootIFD->getEntryRecursive((TiffTag)0x81)->getInt();
    else {
      vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(CFAPATTERN);
      if (data.empty())
        ThrowRDE("CR2 Decoder: Couldn't find offset");
      else {
        if (mRootIFD->getEntryRecursive(STRIPOFFSETS))
          off = data[0]->getEntryRecursive(STRIPOFFSETS)->getInt();
        else
          ThrowRDE("CR2 Decoder: Couldn't find offset");
      }
    }

    ByteStream *b;
    if (getHostEndianness() == big)
      b = new ByteStream(mFile->getData(off+41), mFile->getSize());
    else
      b = new ByteStreamSwap(mFile->getData(off+41), mFile->getSize());
    uint32 height = b->getShort();
    uint32 width = b->getShort();

    // Every two lines can be encoded as a single line, probably to try and get
    // better compression by getting the same RGBG sequence in every line
    if(hints.find("double_line_ljpeg") != hints.end()) {
      height *= 2;
      mRaw->dim = iPoint2D(width*2, height/2);
    }
    else {
      width *= 2;
      mRaw->dim = iPoint2D(width, height);
    }

    mRaw->createData();
    LJpegPlain l(mFile, mRaw);
    l.startDecoder(off, mFile->getSize()-off, 0, 0);

    if(hints.find("double_line_ljpeg") != hints.end()) {
      // We now have a double width half height image we need to convert to the
      // normal format
      iPoint2D final_size(width, height);
      RawImage procRaw = RawImage::create(final_size, TYPE_USHORT16, 1);
      procRaw->clearArea(iRectangle2D(iPoint2D(0,0), procRaw->dim));
      procRaw->metadata = mRaw->metadata;

      for (uint32 y = 0; y < height; y++) {
        ushort16 *dst = (ushort16*)procRaw->getData(0,y);
        ushort16 *src = (ushort16*)mRaw->getData(y%2 == 0 ? 0 : width, y/2);
        for (uint32 x = 0; x < width; x++)
          dst[x] = src[x];
      }
      mRaw = procRaw;
    }

    if (!uncorrectedRawValues && mRootIFD->getEntryRecursive((TiffTag)0x123)) {
      TiffEntry *curve = mRootIFD->getEntryRecursive((TiffTag)0x123);
      if (curve->type == TIFF_SHORT && curve->count == 4096) {
        const ushort16 *linearization = mRootIFD->getEntryRecursive((TiffTag)0x123)->getShortArray();
        for (uint32 y = 0; y < height; y++) {
          ushort16 *img = (ushort16*)mRaw->getData(0,y);
          for (uint32 x = 0; x < width; x++)
            img[x] = linearization[img[x]];
        }
      }
    }

    return mRaw;
  }

  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag((TiffTag)0xc5d8);

  if (data.empty())
    ThrowRDE("CR2 Decoder: No image data found");


  TiffIFD* raw = data[0];
  mRaw = RawImage::create();
  mRaw->isCFA = true;
  vector<Cr2Slice> slices;
  int completeH = 0;

  try {
    TiffEntry *offsets = raw->getEntry(STRIPOFFSETS);
    TiffEntry *counts = raw->getEntry(STRIPBYTECOUNTS);
    // Iterate through all slices
    for (uint32 s = 0; s < offsets->count; s++) {
      Cr2Slice slice;
      slice.offset = offsets[0].getInt();
      slice.count = counts[0].getInt();
      SOFInfo sof;
      LJpegPlain l(mFile, mRaw);
      l.getSOF(&sof, slice.offset, slice.count);
      slice.w = sof.w * sof.cps;
      slice.h = sof.h;
      if (!slices.empty())
        if (slices[0].w != slice.w)
          ThrowRDE("CR2 Decoder: Slice width does not match.");

      if (mFile->isValid(slice.offset + slice.count)) // Only decode if size is valid
        slices.push_back(slice);
      completeH += slice.h;
    }
  } catch (TiffParserException) {
    ThrowRDE("CR2 Decoder: Unsupported format.");
  }

  if (slices.empty()) {
    ThrowRDE("CR2 Decoder: No Slices found.");
  }

  mRaw->dim = iPoint2D(slices[0].w, completeH);

  // Fix for Canon 6D mRaw, which has flipped width & height for some part of the image
  // In that case, we swap width and height, since this is the correct dimension
  bool flipDims = false;
  if (raw->hasEntry((TiffTag)0xc6c5)) {
    ushort16 ss = raw->getEntry((TiffTag)0xc6c5)->getInt();
    // sRaw
    if (ss == 4) {
      mRaw->dim.x /= 3;
      mRaw->setCpp(3);
      mRaw->isCFA = false;
    }
    flipDims = mRaw->dim.x < mRaw->dim.y;
    if (flipDims) {
      int w = mRaw->dim.x;
      mRaw->dim.x = mRaw->dim.y;
      mRaw->dim.y = w;
    }
  }

  mRaw->createData();

  vector<int> s_width;
  if (raw->hasEntry(CANONCR2SLICE)) {
    const ushort16 *ss = raw->getEntry(CANONCR2SLICE)->getShortArray();
    for (int i = 0; i < ss[0]; i++) {
      s_width.push_back(ss[1]);
    }
    s_width.push_back(ss[2]);
  } else {
    s_width.push_back(slices[0].w);
  }
  uint32 offY = 0;

  if (s_width.size() > 15)
    ThrowRDE("CR2 Decoder: No more than 15 slices supported");
  _RPT1(0,"Org slices:%d\n", s_width.size());
  for (uint32 i = 0; i < slices.size(); i++) {
    Cr2Slice slice = slices[i];
    try {
      LJpegPlain l(mFile, mRaw);
      l.addSlices(s_width);
      l.mUseBigtable = true;
      l.mCanonFlipDim = flipDims;
      l.startDecoder(slice.offset, slice.count, 0, offY);
    } catch (RawDecoderException &e) {
      if (i == 0)
        throw;
      // These may just be single slice error - store the error and move on
      mRaw->setError(e.what());
    } catch (IOException &e) {
      // Let's try to ignore this - it might be truncated data, so something might be useful.
      mRaw->setError(e.what());
    }
    offY += slice.w;
  }

  if (mRaw->metadata.subsampling.x > 1 || mRaw->metadata.subsampling.y > 1)
    sRawInterpolate();

  return mRaw;
}

void Cr2Decoder::checkSupportInternal(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("CR2 Support check: Model name not found");
  if (!data[0]->hasEntry(MAKE))
    ThrowRDE("CR2 Support: Make name not found");
  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();

  // Check for sRaw mode
  data = mRootIFD->getIFDsWithTag((TiffTag)0xc5d8);
  if (!data.empty()) {
    TiffIFD* raw = data[0];
    if (raw->hasEntry((TiffTag)0xc6c5)) {
      ushort16 ss = raw->getEntry((TiffTag)0xc6c5)->getInt();
      if (ss == 4) {
        this->checkCameraSupported(meta, make, model, "sRaw1");
        return;
      }
    }
  }
  this->checkCameraSupported(meta, make, model, "");
}

void Cr2Decoder::decodeMetaDataInternal(CameraMetaData *meta) {
  int iso = 0;
  mRaw->cfa.setCFA(iPoint2D(2,2), CFA_RED, CFA_GREEN, CFA_GREEN2, CFA_BLUE);
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("CR2 Meta Decoder: Model name not found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  string mode = "";

  if (mRaw->metadata.subsampling.y == 2 && mRaw->metadata.subsampling.x == 2)
    mode = "sRaw1";

  if (mRaw->metadata.subsampling.y == 1 && mRaw->metadata.subsampling.x == 2)
    mode = "sRaw2";

  if (mRootIFD->hasEntryRecursive(ISOSPEEDRATINGS))
    iso = mRootIFD->getEntryRecursive(ISOSPEEDRATINGS)->getInt();

  // Fetch the white balance
  if (mRootIFD->hasEntryRecursive(CANONCOLORDATA)) {
    TiffEntry *color_data = mRootIFD->getEntryRecursive(CANONCOLORDATA);

    // this entry is a big table, and different cameras store used WB in
    // different parts, so find the offset

    // correct offset for most cameras
    int offset = 126;

    // check for the hint that we need to use other offset
    if (hints.find("wb_offset") != hints.end()) {
      stringstream wb_offset(hints.find("wb_offset")->second);
      wb_offset >> offset;
    }

    /*
     * Canon PowerShot cameras (color_data->count == 5120) identify this tag
     * as TIFF_UNDEFINED, while they still write normal TIFF_SHORT data there
     */
    if ((color_data->type == TIFF_SHORT || color_data->count == 5120) && color_data->count >= (uint32)(offset/2) + 3) {
      const ushort16* data = color_data->getShortArray();

      // RGGB !
      float cam_mul[4];
      for(int c = 0; c < 4; c++)
      {
        cam_mul[c] = (float) data[offset/2 + c];
      }
      if (cam_mul[1] + cam_mul[2] > 0) {
        const float green = (cam_mul[1] + cam_mul[2]) / 2.0f;
        mRaw->metadata.wbCoeffs[0] = cam_mul[0] / green;
        mRaw->metadata.wbCoeffs[1] = 1.0f;
        mRaw->metadata.wbCoeffs[2] = cam_mul[3] / green;
      } else {
        writeLog(DEBUG_PRIO_INFO, "CR2 Decoder: Invalid WB; Green was 0.");
      }
    } else {
      writeLog(DEBUG_PRIO_INFO, "CR2 Decoder: CanonColorData has to be SHORT, %d found.\n", color_data->type);
    }
  } else {
    vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

    if(make.compare("Canon") == 0 && model.compare("Canon PowerShot G9") == 0 &&
        mRootIFD->hasEntryRecursive(CANONSHOTINFO) &&
        mRootIFD->hasEntryRecursive(CANONPOWERSHOTG9WB))
    {

      TiffEntry *shot_info = mRootIFD->getEntryRecursive(CANONSHOTINFO);
      if (shot_info->type == TIFF_SHORT && shot_info->count >= 7) {
        ushort16 wb_index = shot_info->getShortArray()[14/2];

        /* Canon PowerShot G9 */
        TiffEntry *g9_wb = mRootIFD->getEntryRecursive(CANONPOWERSHOTG9WB);
        if (g9_wb->type == TIFF_BYTE) {
          int wb_offset = (wb_index < 18) ? "012347800000005896"[wb_index]-'0' : 0;
          wb_offset = wb_offset*32 + 8;

          if (g9_wb->count >= (uint32)wb_offset + 4*3) {
            // GRBG !
            float cam_mul[4];
            for(int c = 0; c < 4; c++) {
              cam_mul[c] = (float) get4LE(g9_wb->getData(), wb_offset + 4*c);
            }

            const float green = (cam_mul[0] + cam_mul[3]) / 2.0f;
            mRaw->metadata.wbCoeffs[0] = cam_mul[1] / green;
            mRaw->metadata.wbCoeffs[1] = 1.0f;
            mRaw->metadata.wbCoeffs[2] = cam_mul[2] / green;
          } else {
            writeLog(DEBUG_PRIO_INFO, "CR2 Decoder: CANONPOWERSHOTG9WB is too small. Count is %d, but should be at least %d", g9_wb->count, wb_offset + 4*3);
          }
        } else {
          writeLog(DEBUG_PRIO_INFO, "CR2 Decoder: CANONPOWERSHOTG9WB has to be BYTE, %d found.", g9_wb->type);
        }
      } else {
        writeLog(DEBUG_PRIO_INFO, "CR2 Decoder: CANONSHOTINFO has to be SHORT, %d found.", shot_info->type);
      }
    } else if (mRootIFD->hasEntryRecursive((TiffTag) 0xa4)) {
      // WB for the old 1D and 1DS
      TiffEntry *wb = mRootIFD->getEntryRecursive((TiffTag) 0xa4);
      if (wb->count >= 3) {
        const ushort16 *tmp = wb->getShortArray();
        mRaw->metadata.wbCoeffs[0] = (float)tmp[0];
        mRaw->metadata.wbCoeffs[1] = (float)tmp[1];
        mRaw->metadata.wbCoeffs[2] = (float)tmp[2];
      }
    }
  }

  setMetaData(meta, make, model, mode, iso);

}

int Cr2Decoder::getHue() {
  if (hints.find("old_sraw_hue") != hints.end())
    return (mRaw->metadata.subsampling.y * mRaw->metadata.subsampling.x);

  if (!mRootIFD->hasEntryRecursive((TiffTag)0x10)) {
    return 0;
  }
  uint32 model_id = mRootIFD->getEntryRecursive((TiffTag)0x10)->getInt();
  if (model_id >= 0x80000281 || model_id == 0x80000218 || (hints.find("force_new_sraw_hue") != hints.end()))
    return ((mRaw->metadata.subsampling.y * mRaw->metadata.subsampling.x) - 1) >> 1;

  return (mRaw->metadata.subsampling.y * mRaw->metadata.subsampling.x);
    
}

// Interpolate and convert sRaw data.
void Cr2Decoder::sRawInterpolate() {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(CANONCOLORDATA);
  if (data.empty())
    ThrowRDE("CR2 sRaw: Unable to locate WB info.");

  const ushort16 *wb_data = data[0]->getEntry(CANONCOLORDATA)->getShortArray();

  // Offset to sRaw coefficients used to reconstruct uncorrected RGB data.
  wb_data = &wb_data[4+(126+22)/2];

  sraw_coeffs[0] = wb_data[0];
  sraw_coeffs[1] = (wb_data[1] + wb_data[2] + 1) >> 1;
  sraw_coeffs[2] = wb_data[3];

  if (hints.find("invert_sraw_wb") != hints.end()) {
    sraw_coeffs[0] = (int)(1024.0f/((float)sraw_coeffs[0]/1024.0f));
    sraw_coeffs[2] = (int)(1024.0f/((float)sraw_coeffs[2]/1024.0f));
  }

  /* Determine sRaw coefficients */
  bool isOldSraw = hints.find("sraw_40d") != hints.end();
  bool isNewSraw = hints.find("sraw_new") != hints.end();

  if (mRaw->metadata.subsampling.y == 1 && mRaw->metadata.subsampling.x == 2) {
    if (isOldSraw)
      interpolate_422_old(mRaw->dim.x / 2, mRaw->dim.y , 0, mRaw->dim.y);
    else if (isNewSraw)
      interpolate_422_new(mRaw->dim.x / 2, mRaw->dim.y , 0, mRaw->dim.y);
    else
      interpolate_422(mRaw->dim.x / 2, mRaw->dim.y , 0, mRaw->dim.y);
  } else if (mRaw->metadata.subsampling.y == 2 && mRaw->metadata.subsampling.x == 2) {
    if (isNewSraw)
      interpolate_420_new(mRaw->dim.x / 2, mRaw->dim.y / 2 , 0 , mRaw->dim.y / 2);
    else
      interpolate_420(mRaw->dim.x / 2, mRaw->dim.y / 2 , 0 , mRaw->dim.y / 2);
  } else
    ThrowRDE("CR2 Decoder: Unknown subsampling");
}

#define YUV_TO_RGB(Y, Cb, Cr) r = sraw_coeffs[0] * ((int)Y + (( 50*(int)Cb + 22929*(int)Cr) >> 12));\
  g = sraw_coeffs[1] * ((int)Y + ((-5640*(int)Cb - 11751*(int)Cr) >> 12));\
  b = sraw_coeffs[2] * ((int)Y + ((29040*(int)Cb - 101*(int)Cr) >> 12));\
  r >>= 8; g >>=8; b >>=8;

#define STORE_RGB(X,A,B,C) X[A] = clampbits(r,16); X[B] = clampbits(g,16); X[C] = clampbits(b,16);

/* sRaw interpolators - ugly as sin, but does the job in reasonably speed */

// Note: Thread safe.

void Cr2Decoder::interpolate_422(int w, int h, int start_h , int end_h) {
  // Last pixel should not be interpolated
  w--;

  // Current line
  ushort16* c_line;
  const int hue = -getHue() + 16384;
  for (int y = start_h; y < end_h; y++) {
    c_line = (ushort16*)mRaw->getData(0, y);
    int r, g, b;
    int off = 0;
    for (int x = 0; x < w; x++) {
      int Y = c_line[off];
      int Cb = c_line[off+1] - hue;
      int Cr = c_line[off+2] - hue;
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(c_line, off, off + 1, off + 2);
      off += 3;

      Y = c_line[off];
      int Cb2 = (Cb + c_line[off+1+3] - hue) >> 1;
      int Cr2 = (Cr + c_line[off+2+3] - hue) >> 1;
      YUV_TO_RGB(Y, Cb2, Cr2);
      STORE_RGB(c_line, off, off + 1, off + 2);
      off += 3;
    }
    // Last two pixels
    int Y = c_line[off];
    int Cb = c_line[off+1] - hue;
    int Cr = c_line[off+2] - hue;
    YUV_TO_RGB(Y, Cb, Cr);
    STORE_RGB(c_line, off, off + 1, off + 2);

    Y = c_line[off+3];
    YUV_TO_RGB(Y, Cb, Cr);
    STORE_RGB(c_line, off + 3, off + 4, off + 5);
  }
}


// Note: Not thread safe, since it writes inplace.
void Cr2Decoder::interpolate_420(int w, int h, int start_h , int end_h) {
  // Last pixel should not be interpolated
  w--;

  bool atLastLine = FALSE;

  if (end_h == h) {
    end_h--;
    atLastLine = TRUE;
  }

  // Current line
  ushort16* c_line;
  // Next line
  ushort16* n_line;
  // Next line again
  ushort16* nn_line;

  int off;
  int r, g, b;
  const int hue = -getHue() + 16384;

  for (int y = start_h; y < end_h; y++) {
    c_line = (ushort16*)mRaw->getData(0, y * 2);
    n_line = (ushort16*)mRaw->getData(0, y * 2 + 1);
    nn_line = (ushort16*)mRaw->getData(0, y * 2 + 2);
    off = 0;
    for (int x = 0; x < w; x++) {
      int Y = c_line[off];
      int Cb = c_line[off+1] - hue;
      int Cr = c_line[off+2] - hue;
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(c_line, off, off + 1, off + 2);

      Y = c_line[off+3];
      int Cb2 = (Cb + c_line[off+1+6] - hue) >> 1;
      int Cr2 = (Cr + c_line[off+2+6] - hue) >> 1;
      YUV_TO_RGB(Y, Cb2, Cr2);
      STORE_RGB(c_line, off + 3, off + 4, off + 5);

      // Next line
      Y = n_line[off];
      int Cb3 = (Cb + nn_line[off+1] - hue) >> 1;
      int Cr3 = (Cr + nn_line[off+2] - hue) >> 1;
      YUV_TO_RGB(Y, Cb3, Cr3);
      STORE_RGB(n_line, off, off + 1, off + 2);

      Y = n_line[off+3];
      Cb = (Cb + Cb2 + Cb3 + nn_line[off+1+6] - hue) >> 2;  //Left + Above + Right +Below
      Cr = (Cr + Cr2 + Cr3 + nn_line[off+2+6] - hue) >> 2;
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(n_line, off + 3, off + 4, off + 5);
      off += 6;
    }
    int Y = c_line[off];
    int Cb = c_line[off+1] - hue;
    int Cr = c_line[off+2] - hue;
    YUV_TO_RGB(Y, Cb, Cr);
    STORE_RGB(c_line, off, off + 1, off + 2);

    Y = c_line[off+3];
    YUV_TO_RGB(Y, Cb, Cr);
    STORE_RGB(c_line, off + 3, off + 4, off + 5);

    // Next line
    Y = n_line[off];
    Cb = (Cb + nn_line[off+1] - hue) >> 1;
    Cr = (Cr + nn_line[off+2] - hue) >> 1;
    YUV_TO_RGB(Y, Cb, Cr);
    STORE_RGB(n_line, off, off + 1, off + 2);

    Y = n_line[off+3];
    YUV_TO_RGB(Y, Cb, Cr);
    STORE_RGB(n_line, off + 3, off + 4, off + 5);
  }

  if (atLastLine) {
    c_line = (ushort16*)mRaw->getData(0, end_h * 2);
    n_line = (ushort16*)mRaw->getData(0, end_h * 2 + 1);
    off = 0;

    // Last line
    for (int x = 0; x < w; x++) {
      int Y = c_line[off];
      int Cb = c_line[off+1] - hue;
      int Cr = c_line[off+2] - hue;
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(c_line, off, off + 1, off + 2);

      Y = c_line[off+3];
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(c_line, off + 3, off + 4, off + 5);

      // Next line
      Y = n_line[off];
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(n_line, off, off + 1, off + 2);

      Y = n_line[off+3];
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(n_line, off + 3, off + 4, off + 5);
      off += 6;
    }
  }
}

#undef YUV_TO_RGB

#define YUV_TO_RGB(Y, Cb, Cr) r = sraw_coeffs[0] * (Y + Cr -512 );\
  g = sraw_coeffs[1] * (Y + ((-778*Cb - (Cr << 11)) >> 12) - 512);\
  b = sraw_coeffs[2] * (Y + (Cb - 512));\
  r >>= 8; g >>=8; b >>=8;


// Note: Thread safe.

void Cr2Decoder::interpolate_422_old(int w, int h, int start_h , int end_h) {
  // Last pixel should not be interpolated
  w--;

  // Current line
  ushort16* c_line;
  const int hue = -getHue() + 16384;

  for (int y = start_h; y < end_h; y++) {
    c_line = (ushort16*)mRaw->getData(0, y);
    int r, g, b;
    int off = 0;
    for (int x = 0; x < w; x++) {
      int Y = c_line[off];
      int Cb = c_line[off+1] - hue;
      int Cr = c_line[off+2] - hue;
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(c_line, off, off + 1, off + 2);
      off += 3;

      Y = c_line[off];
      int Cb2 = (Cb + c_line[off+1+3] - hue) >> 1;
      int Cr2 = (Cr + c_line[off+2+3] - hue) >> 1;
      YUV_TO_RGB(Y, Cb2, Cr2);
      STORE_RGB(c_line, off, off + 1, off + 2);
      off += 3;
    }
    // Last two pixels
    int Y = c_line[off];
    int Cb = c_line[off+1] - 16384;
    int Cr = c_line[off+2] - 16384;
    YUV_TO_RGB(Y, Cb, Cr);
    STORE_RGB(c_line, off, off + 1, off + 2);

    Y = c_line[off+3];
    YUV_TO_RGB(Y, Cb, Cr);
    STORE_RGB(c_line, off + 3, off + 4, off + 5);
  }
}

/* Algorithm found in EOS 5d Mk III */

#undef YUV_TO_RGB

#define YUV_TO_RGB(Y, Cb, Cr) r = sraw_coeffs[0] * (Y + Cr);\
  g = sraw_coeffs[1] * (Y + ((-778*Cb - (Cr << 11)) >> 12) );\
  b = sraw_coeffs[2] * (Y + Cb);\
  r >>= 8; g >>=8; b >>=8;

void Cr2Decoder::interpolate_422_new(int w, int h, int start_h , int end_h) {
  // Last pixel should not be interpolated
  w--;

  // Current line
  ushort16* c_line;
  const int hue = -getHue() + 16384;

  for (int y = start_h; y < end_h; y++) {
    c_line = (ushort16*)mRaw->getData(0, y);
    int r, g, b;
    int off = 0;
    for (int x = 0; x < w; x++) {
      int Y = c_line[off];
      int Cb = c_line[off+1] - hue;
      int Cr = c_line[off+2] - hue;
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(c_line, off, off + 1, off + 2);
      off += 3;

      Y = c_line[off];
      int Cb2 = (Cb + c_line[off+1+3] - hue) >> 1;
      int Cr2 = (Cr + c_line[off+2+3] - hue) >> 1;
      YUV_TO_RGB(Y, Cb2, Cr2);
      STORE_RGB(c_line, off, off + 1, off + 2);
      off += 3;
    }
    // Last two pixels
    int Y = c_line[off];
    int Cb = c_line[off+1] - 16384;
    int Cr = c_line[off+2] - 16384;
    YUV_TO_RGB(Y, Cb, Cr);
    STORE_RGB(c_line, off, off + 1, off + 2);

    Y = c_line[off+3];
    YUV_TO_RGB(Y, Cb, Cr);
    STORE_RGB(c_line, off + 3, off + 4, off + 5);
  }
}


// Note: Not thread safe, since it writes inplace.
void Cr2Decoder::interpolate_420_new(int w, int h, int start_h , int end_h) {
  // Last pixel should not be interpolated
  w--;

  bool atLastLine = FALSE;

  if (end_h == h) {
    end_h--;
    atLastLine = TRUE;
  }

  // Current line
  ushort16* c_line;
  // Next line
  ushort16* n_line;
  // Next line again
  ushort16* nn_line;
  const int hue = -getHue() + 16384;

  int off;
  int r, g, b;

  for (int y = start_h; y < end_h; y++) {
    c_line = (ushort16*)mRaw->getData(0, y * 2);
    n_line = (ushort16*)mRaw->getData(0, y * 2 + 1);
    nn_line = (ushort16*)mRaw->getData(0, y * 2 + 2);
    off = 0;
    for (int x = 0; x < w; x++) {
      int Y = c_line[off];
      int Cb = c_line[off+1] - hue;
      int Cr = c_line[off+2] - hue;
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(c_line, off, off + 1, off + 2);

      Y = c_line[off+3];
      int Cb2 = (Cb + c_line[off+1+6] - hue) >> 1;
      int Cr2 = (Cr + c_line[off+2+6] - hue) >> 1;
      YUV_TO_RGB(Y, Cb2, Cr2);
      STORE_RGB(c_line, off + 3, off + 4, off + 5);

      // Next line
      Y = n_line[off];
      int Cb3 = (Cb + nn_line[off+1] - hue) >> 1;
      int Cr3 = (Cr + nn_line[off+2] - hue) >> 1;
      YUV_TO_RGB(Y, Cb3, Cr3);
      STORE_RGB(n_line, off, off + 1, off + 2);

      Y = n_line[off+3];
      Cb = (Cb + Cb2 + Cb3 + nn_line[off+1+6] - hue) >> 2;  //Left + Above + Right +Below
      Cr = (Cr + Cr2 + Cr3 + nn_line[off+2+6] - hue) >> 2;
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(n_line, off + 3, off + 4, off + 5);
      off += 6;
    }
    int Y = c_line[off];
    int Cb = c_line[off+1] - hue;
    int Cr = c_line[off+2] - hue;
    YUV_TO_RGB(Y, Cb, Cr);
    STORE_RGB(c_line, off, off + 1, off + 2);

    Y = c_line[off+3];
    YUV_TO_RGB(Y, Cb, Cr);
    STORE_RGB(c_line, off + 3, off + 4, off + 5);

    // Next line
    Y = n_line[off];
    Cb = (Cb + nn_line[off+1] - hue) >> 1;
    Cr = (Cr + nn_line[off+2] - hue) >> 1;
    YUV_TO_RGB(Y, Cb, Cr);
    STORE_RGB(n_line, off, off + 1, off + 2);

    Y = n_line[off+3];
    YUV_TO_RGB(Y, Cb, Cr);
    STORE_RGB(n_line, off + 3, off + 4, off + 5);
  }

  if (atLastLine) {
    c_line = (ushort16*)mRaw->getData(0, end_h * 2);
    n_line = (ushort16*)mRaw->getData(0, end_h * 2 + 1);
    off = 0;

    // Last line
    for (int x = 0; x < w; x++) {
      int Y = c_line[off];
      int Cb = c_line[off+1] - hue;
      int Cr = c_line[off+2] - hue;
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(c_line, off, off + 1, off + 2);

      Y = c_line[off+3];
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(c_line, off + 3, off + 4, off + 5);

      // Next line
      Y = n_line[off];
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(n_line, off, off + 1, off + 2);

      Y = n_line[off+3];
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(n_line, off + 3, off + 4, off + 5);
      off += 6;
    }
  }
}

} // namespace RawSpeed
