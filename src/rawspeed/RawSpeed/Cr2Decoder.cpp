#include "StdAfx.h"
#include "Cr2Decoder.h"
#include "TiffParserHeaderless.h"

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

Cr2Decoder::Cr2Decoder(TiffIFD *rootIFD, FileMap* file) :
    RawDecoder(file), mRootIFD(rootIFD) {

}

Cr2Decoder::~Cr2Decoder(void) {
}

RawImage Cr2Decoder::decodeRaw() {

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

  if (raw->hasEntry((TiffTag)0xc6c5)) {
    ushort16 ss = raw->getEntry((TiffTag)0xc6c5)->getInt();
    // sRaw
    if (ss == 4) {
      mRaw->dim.x /= 3;
      mRaw->setCpp(3);
      mRaw->isCFA = false;
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
  
  for (uint32 i = 0; i < slices.size(); i++) {
    Cr2Slice slice = slices[i];
    try {
      LJpegPlain l(mFile, mRaw);
      l.addSlices(s_width);
      l.mUseBigtable = true;
      l.startDecoder(slice.offset, slice.count, 0, offY);
    } catch (RawDecoderException e) {
      if (i == 0)
        throw;
      // These may just be single slice error - store the error and move on
      errors.push_back(_strdup(e.what()));
    } catch (IOException e) {
      // Let's try to ignore this - it might be truncated data, so something might be useful.
      errors.push_back(_strdup(e.what()));
    }
    offY += slice.w;
  }

  if (mRaw->subsampling.x > 1 || mRaw->subsampling.y > 1)
    sRawInterpolate();

  return mRaw;
}

void Cr2Decoder::checkSupport(CameraMetaData *meta) {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);
  if (data.empty())
    ThrowRDE("CR2 Support check: Model name found");
  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  this->checkCameraSupported(meta, make, model, "");
}

void Cr2Decoder::decodeMetaData(CameraMetaData *meta) {
  mRaw->cfa.setCFA(CFA_RED, CFA_GREEN, CFA_GREEN2, CFA_BLUE);
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("CR2 Meta Decoder: Model name not found");

  string make = data[0]->getEntry(MAKE)->getString();
  string model = data[0]->getEntry(MODEL)->getString();
  string mode = "";

  if (mRaw->subsampling.y == 2 && mRaw->subsampling.x == 2)
    mode = "sRaw1";

  if (mRaw->subsampling.y == 1 && mRaw->subsampling.x == 2)
    mode = "sRaw2";

  setMetaData(meta, make, model, mode);

}

// Interpolate and convert sRaw data.
void Cr2Decoder::sRawInterpolate() {
  vector<TiffIFD*> data = mRootIFD->getIFDsWithTag((TiffTag)0x4001);
  if (data.empty())
    ThrowRDE("CR2 sRaw: Unable to locate WB info.");

  const ushort16 *wb_data = data[0]->getEntry((TiffTag)0x4001)->getShortArray();

  // Offset to sRaw coefficients used to reconstruct uncorrected RGB data.
  wb_data = &wb_data[4+(126+22)/2];

  sraw_coeffs[0] = wb_data[0];
  sraw_coeffs[1] = (wb_data[1] + wb_data[2] + 1) >> 1;
  sraw_coeffs[2] = wb_data[3];

  // Check if sRaw2 is using old coefficients
  data = mRootIFD->getIFDsWithTag(MODEL);

  if (data.empty())
    ThrowRDE("CR2 sRaw Decoder: Model name not found");

  string model = data[0]->getEntry(MODEL)->getString();
  bool isOldSraw = (model.compare("Canon EOS 40D") == 0);

  if (mRaw->subsampling.y == 1 && mRaw->subsampling.x == 2) {
    if (isOldSraw)
      interpolate_422_old(mRaw->dim.x / 2, mRaw->dim.y , 0, mRaw->dim.y);
    else
      interpolate_422(mRaw->dim.x / 2, mRaw->dim.y , 0, mRaw->dim.y);
  } else {
    interpolate_420(mRaw->dim.x / 2, mRaw->dim.y / 2 , 0 , mRaw->dim.y / 2);
  }
}

#define YUV_TO_RGB(Y, Cb, Cr) r = sraw_coeffs[0] * ((int)Y + (( 200*(int)Cb + 22929*(int)Cr) >> 12));\
  g = sraw_coeffs[1] * ((int)Y + ((-5640*(int)Cb - 11751*(int)Cr) >> 12));\
  b = sraw_coeffs[2] * ((int)Y + ((29040*(int)Cb - 101*(int)Cr) >> 12));\
  r >>= 10; g >>=10; b >>=10;

#define STORE_RGB(X,A,B,C) X[A] = clampbits(r,16); X[B] = clampbits(g,16); X[C] = clampbits(b,16);

/* sRaw interpolators - ugly as sin, but does the job in reasonably speed */

// Note: Thread safe.

void Cr2Decoder::interpolate_422(int w, int h, int start_h , int end_h) {
  // Last pixel should not be interpolated
  w--;

  // Current line
  ushort16* c_line;

  for (int y = start_h; y < end_h; y++) {
    c_line = (ushort16*)mRaw->getData(0, y);
    int r, g, b;
    int off = 0;
    for (int x = 0; x < w; x++) {
      int Y = c_line[off];
      int Cb = c_line[off+1] - 16384;
      int Cr = c_line[off+2] - 16384;
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(c_line, off, off + 1, off + 2);
      off += 3;

      Y = c_line[off];
      int Cb2 = (Cb + c_line[off+1+3] - 16384) >> 1;
      int Cr2 = (Cr + c_line[off+2+3] - 16384) >> 1;
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

  for (int y = start_h; y < end_h; y++) {
    c_line = (ushort16*)mRaw->getData(0, y * 2);
    n_line = (ushort16*)mRaw->getData(0, y * 2 + 1);
    nn_line = (ushort16*)mRaw->getData(0, y * 2 + 2);
    off = 0;
    for (int x = 0; x < w; x++) {
      int Y = c_line[off];
      int Cb = c_line[off+1] - 16384;
      int Cr = c_line[off+2] - 16384;
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(c_line, off, off + 1, off + 2);

      Y = c_line[off+3];
      int Cb2 = (Cb + c_line[off+1+6] - 16383) >> 1;
      int Cr2 = (Cr + c_line[off+2+6] - 16383) >> 1;
      YUV_TO_RGB(Y, Cb2, Cr2);
      STORE_RGB(c_line, off + 3, off + 4, off + 5);

      // Next line
      Y = n_line[off];
      int Cb3 = (Cb + nn_line[off+1] - 16383) >> 1;
      int Cr3 = (Cr + nn_line[off+2] - 16383) >> 1;
      YUV_TO_RGB(Y, Cb3, Cr3);
      STORE_RGB(n_line, off, off + 1, off + 2);

      Y = n_line[off+3];
      Cb = (Cb + Cb2 + Cb3 + nn_line[off+1+6] - 16382) >> 2;  //Left + Above + Right +Below
      Cr = (Cr + Cr2 + Cr3 + nn_line[off+2+6] - 16382) >> 2;
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(n_line, off + 3, off + 4, off + 5);
      off += 6;
    }
    int Y = c_line[off];
    int Cb = c_line[off+1] - 16384;
    int Cr = c_line[off+2] - 16384;
    YUV_TO_RGB(Y, Cb, Cr);
    STORE_RGB(c_line, off, off + 1, off + 2);

    Y = c_line[off+3];
    YUV_TO_RGB(Y, Cb, Cr);
    STORE_RGB(c_line, off + 3, off + 4, off + 5);

    // Next line
    Y = n_line[off];
    Cb = (Cb + nn_line[off+1] - 16383) >> 1;
    Cr = (Cr + nn_line[off+2] - 16383) >> 1;
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
      int Cb = c_line[off+1] - 16384;
      int Cr = c_line[off+2] - 16384;
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
  r >>= 10; g >>=10; b >>=10;


// Note: Thread safe.

void Cr2Decoder::interpolate_422_old(int w, int h, int start_h , int end_h) {
  // Last pixel should not be interpolated
  w--;

  // Current line
  ushort16* c_line;

  for (int y = start_h; y < end_h; y++) {
    c_line = (ushort16*)mRaw->getData(0, y);
    int r, g, b;
    int off = 0;
    for (int x = 0; x < w; x++) {
      int Y = c_line[off];
      int Cb = c_line[off+1] - 16384;
      int Cr = c_line[off+2] - 16384;
      YUV_TO_RGB(Y, Cb, Cr);
      STORE_RGB(c_line, off, off + 1, off + 2);
      off += 3;

      Y = c_line[off];
      int Cb2 = (Cb + c_line[off+1+3] - 16384) >> 1;
      int Cr2 = (Cr + c_line[off+2+3] - 16384) >> 1;
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

} // namespace RawSpeed
