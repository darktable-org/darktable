/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Compare the production RawSpeed VC-5 decoder against the official GoPro
 * SDK's sensor samples. Neither side uses an embedded RGB/JPEG preview. */
#include "RawSpeed-API.h"
#include "adt/Array2DRef.h"
#include "gpr.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#define CHECK(condition) do { if(!(condition)) { \
  std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
  return EXIT_FAILURE; } } while(0)

int main(int argc, char **argv)
{
  CHECK(argc == 3); // Actual GPR sample and RawSpeed's matching cameras.xml.
  const gpr_allocator allocator{std::malloc, std::free};
  gpr_buffer original{nullptr, 0}, reference{nullptr, 0};
  gpr_parameters info;
  gpr_parameters_set_defaults(&info);
  CHECK(read_from_file(&original, argv[1], allocator.Alloc, allocator.Free) == 0);
  CHECK(gpr_parse_metadata(&allocator, &original, &info));
  CHECK(gpr_convert_gpr_to_raw(&allocator, &original, &reference));
  CHECK(reference.size == static_cast<size_t>(info.input_width) * info.input_height * sizeof(uint16_t));

  try
  {
    rawspeed::CameraMetaData metadata(argv[2]);
    rawspeed::FileReader reader(argv[1]);
    auto [storage, source] = reader.readFile();
    rawspeed::RawParser parser(source);
    auto decoder = parser.getDecoder(&metadata);
    CHECK(decoder);
    decoder->failOnUnknown = true; // Same initial policy as darktable's loader.
    // Compare decompressed sensor data before optional bad-pixel/opcode edits.
    decoder->interpolateBadPixels = false;
    decoder->applyStage1DngOpcodes = false;
    decoder->applyCrop = false;
    decoder->checkSupport(&metadata);
    decoder->decodeRaw();
    decoder->decodeMetaData(&metadata);
    const auto image = decoder->mRaw;
    CHECK(image->getDataType() == rawspeed::RawImageType::UINT16);
    const auto pixels = image->getU16DataAsUncroppedArray2DRef();
    CHECK(pixels.width() == static_cast<int>(info.input_width));
    CHECK(pixels.height() == static_cast<int>(info.input_height));
    const auto *expected = static_cast<const uint16_t *>(reference.buffer);
    for(int row = 0; row < pixels.height(); row++)
      if(std::memcmp(&pixels(row, 0), expected + static_cast<size_t>(row) * pixels.width(),
                     static_cast<size_t>(pixels.width()) * sizeof(uint16_t)))
        for(int column = 0; column < pixels.width(); column++)
        {
          const auto reference_value = expected[static_cast<size_t>(row) * pixels.width() + column];
          if(pixels(row, column) == reference_value) continue;
          std::fprintf(stderr, "%s: sensor (%d, %d) differs: RawSpeed %u, SDK %u\n",
                       argv[1], column, row, pixels(row, column), reference_value);
          return EXIT_FAILURE;
        }
    std::printf("%s: RawSpeed VC-5 full RAW %u x %u matches %zu official SDK sensor bytes\n",
                argv[1], info.input_width, info.input_height, reference.size);
  }
  catch(const std::exception &error)
  {
    std::fprintf(stderr, "RawSpeed VC-5 decoding failed: %s\n", error.what());
    return EXIT_FAILURE;
  }
  gpr_parameters_destroy(&info, std::free);
  std::free(original.buffer);
  std::free(reference.buffer);
  return EXIT_SUCCESS;
}
