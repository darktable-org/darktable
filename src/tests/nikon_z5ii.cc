/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

#include <glib.h>
#include "RawSpeed-API.h"
#include "imageio/imageio_nikon_he.h"
#include "imageio/nikon_he/nikon_he_picture_header.h"
#include "tiff/TiffIFD.h"
#include "decoders/NefDecoder.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <vector>

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

int rawspeed_get_number_of_processor_cores()
{
  return 2;
}

// CC0 samples and golden mosaics documented in dev-doc/Nikon_Z5II.md
struct sample_t
{
  const char *name;
  int width, height;
  unsigned compression;
  const char *sha256;
};

static const sample_t samples[] = {
  { "Z5_2_16x9_HE_6d087e88", 6064, 3416, 13,
    "703c7c15f366a52b419319cf7ee1744aacbc6484ba8a4ecaae041b748e333994" },
  { "Z5_2_16x9_HEstar_06069e1c", 6064, 3416, 14,
    "a16e52433efac5c942a09e7d098c099f84a6087197448531d30aba28ea75dbdb" },
  { "Z5_2_1x1_HE_465aba7a", 4048, 4040, 13,
    "1d642854161456fe4c927328bfa60d378403b7ae97282d0a18538be37464c844" },
  { "Z5_2_1x1_HEstar_a605497e", 4048, 4040, 14,
    "9014140fd54c00c7fa2a5ef8c6309d16694995849f1b11c298da2adb87d5a1ad" },
  { "Z5_2_3x2_HE_196971ea", 4000, 2672, 13,
    "090d6e66ca9b65997120b129492f07fd977b2948e6a8d501c79e89c1dd43c0a2" },
  { "Z5_2_3x2_HE_d8938c5d", 6064, 4040, 13,
    "cc8439d8ac026c9f4e085a40e1b364bb82690bd91ed4d4ec924bb3979073a6e5" },
  { "Z5_2_3x2_HEstar_276ef4ad", 4000, 2672, 14,
    "d59e7ea4250ff32d1152265126e5ed22987d3e71e3b2d4f1ceca87dada5fee0b" },
  { "Z5_2_3x2_HEstar_b44bb317", 6064, 4040, 14,
    "2e0248af6a801d2586d0089b69836dd00b8059dae01d75c41ae5e4e37f4a6804" }
};

static bool _decode(rawspeed::Buffer file, const rawspeed::CameraMetaData &metadata,
                    const sample_t *sample)
{
  rawspeed::RawParser parser(file);
  auto decoder = dt_imageio_nikon_he_decoder(parser.getDecoder(&metadata), file);
  decoder->failOnUnknown = true;
  decoder->checkSupport(&metadata);
  if(sample)
  {
    auto info = decoder->getRootIFD()->getEntryRecursive(static_cast<rawspeed::TiffTag>(0x0051))->getData();
    info.skipBytes(10);
    if(info.getU16() != sample->compression) return false;
  }
  decoder->decodeRaw();
  decoder->decodeMetaData(&metadata);
  const auto image = decoder->mRaw;
  const auto pixels = image->getU16DataAsUncroppedArray2DRef();
  bool valid = image->blackLevel == 1008 && image->whitePoint == 15892
    && image->metadata.canonical_model == "Z5_2" && image->metadata.wbCoeffs
    && image->metadata.colorMatrix.size() == 9 && image->getCropOffset() == rawspeed::iPoint2D(0, 0);
  if(image->metadata.wbCoeffs)
    for(int channel = 0; channel < 3; channel++)
      valid = valid && std::isfinite((*image->metadata.wbCoeffs)[channel])
        && (*image->metadata.wbCoeffs)[channel] > 0;
  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  std::vector<guchar> row_bytes(2 * pixels.width());
  for(int row = 0; row < pixels.height(); row++)
  {
    for(int column = 0; column < pixels.width(); column++)
    {
      row_bytes[2 * column] = pixels(row, column) & 255;
      row_bytes[2 * column + 1] = pixels(row, column) >> 8;
    }
    g_checksum_update(checksum, row_bytes.data(), row_bytes.size());
  }
  if(sample)
    valid = valid && pixels.width() == sample->width && pixels.height() == sample->height
      && !std::strcmp(g_checksum_get_string(checksum), sample->sha256);
  std::printf("%d x %d, black %d, white %d, SHA-256 %s\n", pixels.width(), pixels.height(),
              image->blackLevel, image->whitePoint.value_or(-1), g_checksum_get_string(checksum));
  g_checksum_free(checksum);
  return valid;
}

static bool _expect_rejected(rawspeed::Buffer file, const rawspeed::CameraMetaData &metadata)
{
  try
  {
    rawspeed::RawParser parser(file);
    auto decoder = dt_imageio_nikon_he_decoder(parser.getDecoder(&metadata), file);
    decoder->failOnUnknown = true;
    decoder->checkSupport(&metadata);
    decoder->decodeRaw();
  }
  catch(const rawspeed::RawspeedException &)
  {
    return true;
  }
  return false;
}

static bool _check_malformed(rawspeed::Buffer file, const rawspeed::CameraMetaData &metadata)
{
  bool valid = _expect_rejected(file.getSubView(0, file.getSize() / 2), metadata);
  rawspeed::RawParser parser(file);
  auto decoder = dt_imageio_nikon_he_decoder(parser.getDecoder(&metadata), file);
  const auto *raw = decoder->getRootIFD()->getIFDWithTag(rawspeed::TiffTag::CFAPATTERN);
  const auto offset = raw->getEntry(rawspeed::TiffTag::STRIPOFFSETS)->getU32();
  const auto count = raw->getEntry(rawspeed::TiffTag::STRIPBYTECOUNTS)->getU32();
  const auto strip = file.getSubView(offset, count);
  nikon_he::PictureHeader header;
  if(!nikon_he::parse_picture_header(strip.begin(), strip.getSize(), header)) return false;
  std::vector<uint8_t> corrupted(file.begin(), file.end());
  auto check = [&]()
  {
    const bool rejected = _expect_rejected(rawspeed::Buffer(corrupted.data(), corrupted.size()), metadata);
    std::memcpy(corrupted.data(), file.begin(), file.getSize());
    return rejected;
  };
  corrupted[offset] = 0;
  valid = check() && valid;
  // a zero first precinct must not yield a zero-filled image
  std::memset(corrupted.data() + offset + header.precinct_offset, 0, 3);
  valid = check() && valid;
  // an impossible packet length must fail before reading its payload
  std::memset(corrupted.data() + offset + header.precinct_offset + 12, 255, 7);
  valid = check() && valid;
  // the codestream and TIFF dimensions must agree
  bool found_picture_header = false;
  bool found_component_table = false;
  for(size_t marker = 2; marker + 16 < header.precinct_offset;)
  {
    if(strip[marker] == 255 && strip[marker + 1] == 0x12)
    {
      found_picture_header = true;
      corrupted[offset + marker + 12] ^= 1;
      valid = check() && valid;
    }
    if(strip[marker] == 255 && strip[marker + 1] == 0x13)
    {
      found_component_table = true;
      corrupted[offset + marker + 4] = 0;
      valid = check() && valid;
    }
    marker += 2 + (strip[marker + 2] << 8) + strip[marker + 3];
  }
  corrupted[offset + count - 1] = 0;
  valid = check() && valid;
  return valid && found_picture_header && found_component_table;
}

static bool _check_decoder_lifecycle(const char *directory)
{
  rawspeed::CameraMetaData metadata(NIKON_Z5II_CAMERAS_XML);
  auto &camera = metadata.cameras.at({"NIKON CORPORATION", "NIKON Z5_2",
                                     "14bit-compressed"});
  camera->hints.add("pixel_aspect_ratio", "1.5");
  gchar *path = g_strdup_printf("%s/%s.NEF", directory, samples[7].name);
  gchar *contents = nullptr;
  gsize size = 0;
  bool valid = false;
  if(g_file_get_contents(path, &contents, &size, nullptr) && size <= INT32_MAX)
  {
    try
    {
      const rawspeed::Buffer file(reinterpret_cast<const uint8_t *>(contents), size);
      rawspeed::RawParser parser(file);
      auto decoder = dt_imageio_nikon_he_decoder(parser.getDecoder(&metadata), file);
      decoder->failOnUnknown = true;
      decoder->checkSupport(&metadata);
      // the common decode wrapper must consume pending bad pixels and camera hints
      decoder->mRaw->mBadPixelPositions.push_back((100u << 16) | 100u);
      decoder->decodeRaw();
      valid = decoder->mRaw->mBadPixelPositions.empty()
        && decoder->mRaw->metadata.pixelAspectRatio == 1.5;
      decoder->decodeMetaData(&metadata);
      valid = valid && decoder->mRaw->metadata.pixelAspectRatio == 1.5;
    }
    catch(const std::exception &error)
    {
      std::fprintf(stderr, "decoder lifecycle: %s\n", error.what());
    }
  }
  g_free(contents);
  g_free(path);
  std::printf("RawSpeed decode lifecycle: %s\n", valid ? "PASS" : "FAIL");
  return valid;
}

static bool _check_sample(const char *path, const rawspeed::CameraMetaData &metadata,
                          const sample_t *sample)
{
  gchar *contents = nullptr;
  gsize size = 0;
  if(!g_file_get_contents(path, &contents, &size, nullptr) || size > INT32_MAX)
  {
    std::fprintf(stderr, "missing or oversized sample: %s\n", path);
    g_free(contents);
    return false;
  }
  bool valid = false;
  try
  {
    const rawspeed::Buffer file(reinterpret_cast<const uint8_t *>(contents), size);
    valid = _decode(file, metadata, sample);
    if(sample) valid = _check_malformed(file, metadata) && valid;
  }
  catch(const std::exception &error)
  {
    std::fprintf(stderr, "%s: %s\n", path, error.what());
    valid = false;
  }
  g_free(contents);
  std::printf("%s: %s\n", path, valid ? "PASS" : "FAIL");
  return valid;
}

static bool _check_lossless(const char *directory, const char *name,
                            const rawspeed::CameraMetaData &metadata)
{
  gchar *path = g_strdup_printf("%s/%s.NEF", directory, name);
  gchar *contents = nullptr;
  gsize size = 0;
  bool valid = false;
  if(g_file_get_contents(path, &contents, &size, nullptr) && size <= INT32_MAX)
  {
    try
    {
      const rawspeed::Buffer file(reinterpret_cast<const uint8_t *>(contents), size);
      rawspeed::RawParser parser(file);
      auto decoder = dt_imageio_nikon_he_decoder(parser.getDecoder(&metadata), file);
      decoder->failOnUnknown = true;
      decoder->checkSupport(&metadata);
      valid = dynamic_cast<rawspeed::NefDecoder *>(decoder.get()) != nullptr;
      decoder->decodeRaw();
      decoder->decodeMetaData(&metadata);
    }
    catch(const std::exception &error)
    {
      std::fprintf(stderr, "%s: %s\n", path, error.what());
      valid = false;
    }
  }
  std::printf("%s: lossless fallback %s\n", path, valid ? "PASS" : "FAIL");
  g_free(contents);
  g_free(path);
  return valid;
}

int main(int argc, char *argv[])
{
  const rawspeed::CameraMetaData metadata(NIKON_Z5II_CAMERAS_XML);
  bool valid = true;
  if(argc > 1)
  {
    for(int i = 1; i < argc; i++)
      valid = _check_sample(argv[i], metadata, nullptr) && valid;
    return valid ? 0 : 1;
  }
  const char *directory = g_getenv("NIKON_Z5II_SAMPLE_DIR");
  if(!directory || !*directory)
  {
    std::puts("set NIKON_Z5II_SAMPLE_DIR to the verified CC0 sample directory");
    return 77;
  }

  for(const sample_t &sample : samples)
  {
    gchar *path = g_strdup_printf("%s/%s.NEF", directory, sample.name);
    valid = _check_sample(path, metadata, &sample) && valid;
    g_free(path);
  }
  for(const char *name : { "Z5_2_1x1_lossless_133d8a62", "Z5_2_3x2_lossless_387d613e",
                           "Z5_2_3x2_lossless_ba6c66ec" })
    valid = _check_lossless(directory, name, metadata) && valid;

  // different sizes and quantization profiles must not share mutable decode state
  auto parallel = std::async(std::launch::async, [&]()
  {
    gchar *path = g_strdup_printf("%s/%s.NEF", directory, samples[0].name);
    const bool result = _check_sample(path, metadata, &samples[0]);
    g_free(path);
    return result;
  });
  gchar *path = g_strdup_printf("%s/%s.NEF", directory, samples[7].name);
  valid = _check_sample(path, metadata, &samples[7]) && valid;
  g_free(path);
  valid = parallel.get() && valid;
  valid = _check_decoder_lifecycle(directory) && valid;
  return valid ? 0 : 1;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
