/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "dt_mobile_gpr.h"
#include "gpr.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
// The SDK contains shared metadata scratch buffers. Serialize both parse and
// conversion, including cleanup, across mobile sessions.
std::mutex sdk_lock;
struct FileCloser {
  void operator()(FILE *file) const { std::fclose(file); }
};
struct Buffer {
  gpr_buffer value{nullptr, 0};
  ~Buffer() { std::free(value.buffer); }
};
struct Parameters {
  gpr_parameters value;
  Parameters() { gpr_parameters_set_defaults(&value); }
  ~Parameters() { gpr_parameters_destroy(&value, std::free); }
};
dt_mobile_status fail(char *error, size_t size, dt_mobile_status status, const char *message)
{
  if(error && size) std::snprintf(error, size, "%s", message);
  return status;
}
}

extern "C" dt_mobile_status dt_mobile_gpr_prepare(const char *source_path, const char *cache_directory,
                                                  char **out_path, char *error, size_t error_size)
{
  if(out_path) *out_path = nullptr;
  if(error && error_size) error[0] = '\0';
  if(!source_path || !source_path[0] || !out_path)
    return fail(error, error_size, DT_MOBILE_ERROR_INVALID_ARGUMENT, "a GPR source and output path are required");
  try
  {
    std::lock_guard<std::mutex> guard(sdk_lock);
    std::unique_ptr<FILE, FileCloser> source(std::fopen(source_path, "rb"));
    struct stat info;
    if(!source || fstat(fileno(source.get()), &info) || !S_ISREG(info.st_mode) || info.st_size < 16)
      return fail(error, error_size, DT_MOBILE_ERROR_IO, "unable to read a complete GPR source");
    if(static_cast<uintmax_t>(info.st_size) > std::numeric_limits<size_t>::max())
      return fail(error, error_size, DT_MOBILE_ERROR_OUT_OF_MEMORY, "GPR source is too large for this device");
    Buffer input, output;
    input.value.size = static_cast<size_t>(info.st_size);
    input.value.buffer = std::malloc(input.value.size);
    if(!input.value.buffer)
      return fail(error, error_size, DT_MOBILE_ERROR_OUT_OF_MEMORY, "unable to allocate GPR input");
    if(std::fread(input.value.buffer, 1, input.value.size, source.get()) != input.value.size)
      return fail(error, error_size, DT_MOBILE_ERROR_IO, "GPR source read failed");
    source.reset();
    const gpr_allocator allocator{std::malloc, std::free};
    Parameters parameters;
    // Parsing the source parameters first preserves its camera color matrices,
    // white balance, black/white levels, CFA layout, EXIF and lens gain maps.
    if(!gpr_parse_metadata(&allocator, &input.value, &parameters.value)
       || !parameters.value.input_width || !parameters.value.input_height
       || !gpr_convert_gpr_to_dng(&allocator, &parameters.value, &input.value, &output.value)
       || !output.value.buffer || !output.value.size)
      return fail(error, error_size, DT_MOBILE_ERROR_DECODE, "GoPro VC-5 RAW decoding failed");

    const char *directory = cache_directory && cache_directory[0] ? cache_directory : std::getenv("TMPDIR");
    if(!directory || !directory[0]) directory = "/tmp";
    std::string temporary = std::string(directory) + "/dt-gpr-XXXXXX.dng";
    char *path = ::strdup(temporary.c_str());
    if(!path)
      return fail(error, error_size, DT_MOBILE_ERROR_OUT_OF_MEMORY, "unable to allocate GPR cache path");
    const int fd = mkstemps(path, 4);
    if(fd < 0)
    {
      std::free(path);
      return fail(error, error_size, DT_MOBILE_ERROR_IO, "unable to create private decoded GPR cache");
    }
    size_t offset = 0;
    bool written = true;
    while(offset < output.value.size)
    {
      const size_t remaining = output.value.size - offset;
      const size_t count = remaining > static_cast<size_t>(std::numeric_limits<ssize_t>::max())
        ? static_cast<size_t>(std::numeric_limits<ssize_t>::max()) : remaining;
      const ssize_t result = write(fd, static_cast<const char *>(output.value.buffer) + offset, count);
      if(result < 0 && errno == EINTR) continue;
      if(result <= 0) { written = false; break; }
      offset += static_cast<size_t>(result);
    }
    if(close(fd)) written = false;
    if(!written)
    {
      dt_mobile_gpr_cleanup(path);
      return fail(error, error_size, DT_MOBILE_ERROR_IO, "unable to write decoded GPR cache");
    }
    *out_path = path;
    return DT_MOBILE_OK;
  }
  catch(const std::bad_alloc &)
  {
    return fail(error, error_size, DT_MOBILE_ERROR_OUT_OF_MEMORY, "GoPro decoder ran out of memory");
  }
  catch(...)
  {
    return fail(error, error_size, DT_MOBILE_ERROR_DECODE, "invalid or unsupported GoPro GPR data");
  }
}

extern "C" void dt_mobile_gpr_cleanup(char *path)
{
  if(!path) return;
  unlink(path);
  std::free(path);
}
