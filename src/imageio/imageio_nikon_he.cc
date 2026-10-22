/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.
    Portions adapted from the Nikon HE decoder, Copyright (C) 2026 Dmitri Sotnikov.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable. If not, see <http://www.gnu.org/licenses/>.
*/

#include "imageio/imageio_nikon_he.h"
#include "imageio/nikon_he/nikon_he_decode.h"

#include "adt/Array2DRef.h"
#include "common/RawImage.h"
#include "decoders/NefDecoder.h"
#include "decoders/RawDecoderException.h"
#include "io/Buffer.h"
#include "metadata/CameraMetaData.h"
#include "tiff/TiffIFD.h"
#include "tiff/TiffTag.h"

#include <memory>
#include <string>
#include <utility>

namespace
{
// retain NEF metadata handling while using RawDecoder's normal decode wrapper
class _nikon_he_decoder final : public rawspeed::RawDecoder
{
public:
  _nikon_he_decoder(std::unique_ptr<rawspeed::RawDecoder> nef,
                    const rawspeed::Buffer file)
    : RawDecoder(file), _nef(std::move(nef))
  {
    mRaw = _nef->mRaw;
  }

  rawspeed::TiffIFD *getRootIFD() override
  {
    return _nef->getRootIFD();
  }

private:
  std::unique_ptr<rawspeed::RawDecoder> _nef;

  const rawspeed::TiffIFD *_raw_ifd() const
  {
    return static_cast<rawspeed::NefDecoder &>(*_nef).getIFDWithLargestImage(
      rawspeed::TiffTag::CFAPATTERN);
  }

  void checkSupportInternal(const rawspeed::CameraMetaData *metadata) override
  {
    using namespace rawspeed;
    _nef->failOnUnknown = failOnUnknown;
    _nef->checkSupport(metadata);
    noSamples = _nef->noSamples;

    // use the same dimension-specific camera hints as the 14-bit NEF decoder
    const auto *raw = _raw_ifd();
    const auto width = raw->getEntry(TiffTag::IMAGEWIDTH)->getU32();
    const auto height = raw->getEntry(TiffTag::IMAGELENGTH)->getU32();
    const std::string mode = "14bit-compressed";
    const std::string extended = std::to_string(width) + "x" + std::to_string(height)
      + "-" + mode;
    const auto &make = mRaw->metadata.make;
    const auto &model = mRaw->metadata.model;
    const auto *camera = metadata->getCamera(make, model, extended);
    if(!camera) camera = metadata->getCamera(make, model, mode);
    if(camera) hints = camera->hints;
  }

  rawspeed::RawImage decodeRawInternal() override
  {
    using namespace rawspeed;
    const auto *raw = _raw_ifd();
    if(raw->getEntry(TiffTag::COMPRESSION)->getU32() != 34713
       || raw->getEntry(TiffTag::BITSPERSAMPLE)->getU32() != 14)
      ThrowRDE("invalid Z5II HE compression or bit depth");

    const uint32_t width = raw->getEntry(TiffTag::IMAGEWIDTH)->getU32();
    const uint32_t height = raw->getEntry(TiffTag::IMAGELENGTH)->getU32();
    if(width < 64 || width > 6064 || width % 8
       || height < 64 || height > 4040 || height % 4)
      ThrowRDE("unsupported Z5II HE dimensions: %u x %u", width, height);

    const auto *offsets = raw->getEntry(TiffTag::STRIPOFFSETS);
    const auto *counts = raw->getEntry(TiffTag::STRIPBYTECOUNTS);
    if(offsets->count != 1 || counts->count != 1)
      ThrowRDE("invalid Z5II HE strip count");
    const Buffer strip = mFile.getSubView(offsets->getU32(), counts->getU32());

    mRaw->dim = iPoint2D(width, height);
    mRaw->createData();
    const auto output = mRaw->getU16DataAsUncroppedArray2DRef();
    const auto result = nikon_he::decode_nikon_he_image(
      strip.begin(), strip.getSize(), width, height, &output(0, 0), output.pitch());
    if(!result.success) ThrowRDE("unsupported or corrupt Nikon HE codestream");
    return mRaw;
  }

  void decodeMetaDataInternal(const rawspeed::CameraMetaData *metadata) override
  {
    _nef->applyCrop = applyCrop;
    _nef->uncorrectedRawValues = uncorrectedRawValues;
    _nef->decodeMetaData(metadata);
  }

  // camera decoder-version checks are performed by the original NEF decoder
  int getDecoderVersion() const override { return 0; }
};
}

std::unique_ptr<rawspeed::RawDecoder> dt_imageio_nikon_he_decoder(
  std::unique_ptr<rawspeed::RawDecoder> decoder,
  const rawspeed::Buffer file)
{
  using namespace rawspeed;
  if(!dynamic_cast<NefDecoder *>(decoder.get())) return decoder;
  const auto *root = decoder->getRootIFD();
  const auto *make = root->getEntryRecursive(TiffTag::MAKE);
  const auto *model = root->getEntryRecursive(TiffTag::MODEL);
  if(!make || !model || make->getString() != "NIKON CORPORATION"
     || model->getString() != "NIKON Z5_2")
    return decoder;

  const auto *compression = root->getEntryRecursive(static_cast<TiffTag>(0x0051));
  if(!compression) return decoder;
  // the compression value is a short at byte 10 of Nikon's RawImageInfo block
  auto raw_info = compression->getData();
  raw_info.skipBytes(10);
  const uint16_t nef_compression = raw_info.getU16();
  if(nef_compression != 13 && nef_compression != 14) return decoder;
  return std::make_unique<_nikon_he_decoder>(std::move(decoder), file);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
