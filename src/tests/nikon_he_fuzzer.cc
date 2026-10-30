/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

#include "imageio/nikon_he/nikon_he_decode.h"
#include "imageio/nikon_he/nikon_he_picture_header.h"

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
  nikon_he::PictureHeader header;
  if(!nikon_he::parse_picture_header(data, size, header)
     || !nikon_he::is_supported_picture_header(header, size))
    return 0;
  std::vector<uint16_t> output(static_cast<size_t>(header.hdr_width) * header.hdr_height);
  nikon_he::decode_nikon_he_image(data, size, header.hdr_width, header.hdr_height,
                                output.data(), header.hdr_width);
  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
