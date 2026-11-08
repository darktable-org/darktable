/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dt_mobile_gpr.h"
#include "gpr.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
  assert(argc == 3); // Real official GPR sample, writable test cache directory.
  char error[256];
  char *path = nullptr;
  assert(dt_mobile_gpr_prepare(nullptr, argv[2], &path, error, sizeof(error)) == DT_MOBILE_ERROR_INVALID_ARGUMENT);
  assert(path == nullptr);
  assert(dt_mobile_gpr_prepare("missing.GPR", argv[2], &path, error, sizeof(error)) == DT_MOBILE_ERROR_IO);
  assert(path == nullptr);
  char truncated[4096];
  assert(std::snprintf(truncated, sizeof(truncated), "%s/broken-XXXXXX", argv[2]) < static_cast<int>(sizeof(truncated)));
  const int broken_fd = mkstemp(truncated);
  assert(broken_fd >= 0);
  const char garbage[32] = {'I', 'I', 42};
  assert(write(broken_fd, garbage, sizeof(garbage)) == sizeof(garbage));
  assert(close(broken_fd) == 0);
  assert(dt_mobile_gpr_prepare(truncated, argv[2], &path, error, sizeof(error)) == DT_MOBILE_ERROR_DECODE);
  assert(path == nullptr);
  assert(unlink(truncated) == 0);

  const gpr_allocator allocator{std::malloc, std::free};
  gpr_buffer original{nullptr, 0}, dng{nullptr, 0}, raw_original{nullptr, 0}, raw_dng{nullptr, 0};
  assert(read_from_file(&original, argv[1], allocator.Alloc, allocator.Free) == 0);
  gpr_parameters source_info, dng_info;
  gpr_parameters_set_defaults(&source_info);
  gpr_parameters_set_defaults(&dng_info);
  assert(gpr_parse_metadata(&allocator, &original, &source_info));
  const auto result = dt_mobile_gpr_prepare(argv[1], argv[2], &path, error, sizeof(error));
  if(result != DT_MOBILE_OK) std::fprintf(stderr, "%s\n", error);
  assert(result == DT_MOBILE_OK && path);
  struct stat cache;
  assert(stat(path, &cache) == 0 && (cache.st_mode & 0777) == 0600);
  assert(read_from_file(&dng, path, allocator.Alloc, allocator.Free) == 0);
  assert(gpr_parse_metadata(&allocator, &dng, &dng_info));
  assert(source_info.input_width == dng_info.input_width);
  assert(source_info.input_height == dng_info.input_height);
  assert(source_info.tuning_info.pixel_format == dng_info.tuning_info.pixel_format);
  assert(source_info.tuning_info.orientation == dng_info.tuning_info.orientation);
  assert(std::strcmp(source_info.exif_info.camera_make, dng_info.exif_info.camera_make) == 0);
  assert(std::strcmp(source_info.exif_info.camera_model, dng_info.exif_info.camera_model) == 0);
  assert(source_info.exif_info.iso_speed_rating == dng_info.exif_info.iso_speed_rating);
  assert(source_info.profile_info.illuminant1 == dng_info.profile_info.illuminant1);
  assert(source_info.profile_info.illuminant2 == dng_info.profile_info.illuminant2);
  for(int row = 0; row < 3; row++) for(int column = 0; column < 3; column++)
  {
    assert(std::fabs(source_info.profile_info.color_matrix_1[row][column] - dng_info.profile_info.color_matrix_1[row][column]) < 1e-5);
    assert(std::fabs(source_info.profile_info.color_matrix_2[row][column] - dng_info.profile_info.color_matrix_2[row][column]) < 1e-5);
  }
  assert(std::fabs(source_info.tuning_info.wb_gains.r_gain - dng_info.tuning_info.wb_gains.r_gain) < 1e-5);
  assert(std::fabs(source_info.tuning_info.wb_gains.g_gain - dng_info.tuning_info.wb_gains.g_gain) < 1e-5);
  assert(std::fabs(source_info.tuning_info.wb_gains.b_gain - dng_info.tuning_info.wb_gains.b_gain) < 1e-5);
  assert(source_info.input_width >= 3000 && source_info.input_height >= 2000);
  assert(gpr_convert_gpr_to_raw(&allocator, &original, &raw_original));
  assert(gpr_convert_dng_to_raw(&allocator, &dng, &raw_dng));
  assert(raw_original.size == raw_dng.size);
  assert(raw_dng.size >= static_cast<size_t>(source_info.input_width) * source_info.input_height * 2);
  // Every decoded sensor sample must survive conversion, before demosaicing.
  assert(std::memcmp(raw_original.buffer, raw_dng.buffer, raw_dng.size) == 0);
  gpr_buffer unchanged{nullptr, 0};
  assert(read_from_file(&unchanged, argv[1], allocator.Alloc, allocator.Free) == 0);
  assert(unchanged.size == original.size);
  assert(std::memcmp(unchanged.buffer, original.buffer, original.size) == 0);
  char *removed = ::strdup(path);
  dt_mobile_gpr_cleanup(path);
  assert(access(removed, F_OK) != 0);
  std::free(removed);
  std::printf("%s: full RAW %u x %u, %zu sensor bytes match, original unchanged, private cache removed\n",
              argv[1], source_info.input_width, source_info.input_height, raw_dng.size);
  for(void *buffer : {original.buffer, dng.buffer, raw_original.buffer, raw_dng.buffer, unchanged.buffer}) std::free(buffer);
  gpr_parameters_destroy(&source_info, std::free);
  gpr_parameters_destroy(&dng_info, std::free);
  return 0;
}
