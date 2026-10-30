/* -*- C++ -*-
 * File: nikon_he_precinct_decode.h
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE per-precinct entropy decode interface

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE per-precinct entropy decode orchestration.
//
// Walks the 26 sub-bands of a single precinct, dispatching each to the
// GCLI decoder, coefficient unpacker, sign-bit applicator, and
// dequantizer. Results are scattered into bufA (LBs 0-3) or bufB
// (LBs 4-7) at the x24 offset specified by the sub-band configuration.
//


#ifndef LIBRAW_NIKON_HE_PRECINCT_DECODE_H
#define LIBRAW_NIKON_HE_PRECINCT_DECODE_H

#include "nikon_he_bit_reader.h"
#include "nikon_he_picture_header.h"
#include "nikon_he_precinct_header.h"
#include "nikon_he_predecessor.h"
#include "nikon_he_subband_config.h"
#include <cstdint>

namespace nikon_he {

struct PrecinctDecodeResult {
    int total_sub_bands_decoded;
    bool success;
};

PrecinctDecodeResult decode_precinct(
    const uint8_t* precinct_data,
    size_t precinct_size,
    const PictureHeader& header,
    const SubbandConfig config[26],
    PrecinctPredecessorState& pred_state,
    const uint8_t* predict_lut,
    int32_t* bufA,
    int32_t* bufB);

}  // namespace nikon_he

#endif  // LIBRAW_NIKON_HE_PRECINCT_DECODE_H
