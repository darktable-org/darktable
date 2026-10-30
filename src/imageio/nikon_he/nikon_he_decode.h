/* -*- C++ -*-
 * File: nikon_he_decode.h
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE image-level decoder interface

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE image-level decoder orchestration.
//
// Top-level entry point for decoding a Nikon HE-compressed NEF image.
// Walks the precinct byte stream, dispatches per-tile decoding
// (entropy → horizontal IDWT → vertical IDWT → tile_coeff_buf), then runs
// Bayer reconstruction (step1 → step2) to produce the final uint16
// RGGB Bayer image.
//


#ifndef LIBRAW_NIKON_HE_DECODE_H
#define LIBRAW_NIKON_HE_DECODE_H

#include "nikon_he_bayer.h"
#include "nikon_he_subband_config.h"
#include "nikon_he_tile.h"
#include <cstddef>
#include <cstdint>

namespace nikon_he {

// Result of a full image decode.
struct HeDecodeResult {
    bool success;
    int tiles_decoded;
    int total_precincts;
};

// validate and decode a complete Nikon HE codestream into a strided Bayer buffer
// dimensions must agree with the picture header; stride is measured in uint16_t
HeDecodeResult decode_nikon_he_image(const uint8_t* strip, size_t strip_size,
                                     int image_width, int image_height,
                                     uint16_t* out_bayer, int output_stride);

}  // namespace nikon_he

#endif  // LIBRAW_NIKON_HE_DECODE_H
