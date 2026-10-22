/* -*- C++ -*-
 * File: nikon_he_bayer.h
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE Bayer reconstruction interface

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE Bayer reconstruction.
//
// After tile decoding, tile_coeff_buf contains 32 stripes of 4 sub-band planes
// per tile. Each stripe has 4 regions at offsets {0, kBand, 2*kBand, 3*kBand}:
//
//   offset 0:   LH band (LB 0 lift output)
//   offset kBand:   LL band (LB 1 lift output)
//   offset 2*kBand: HL band (LB 3 lift output)
//   offset 3*kBand: HH band (LB 2 memcpy region)
//
// Two passes reconstruct the final Bayer image:
//
//   step1: merge 4 planes → L and H (2D inverse 5/3 lift, >>3 coefficients)
//   step2: final inverse with tone-curve LUT → uint16 RGGB Bayer
//
// Each IDWT stripe produces 2 Bayer rows.
//


#ifndef LIBRAW_NIKON_HE_BAYER_H
#define LIBRAW_NIKON_HE_BAYER_H

#include <cstdint>

namespace nikon_he {

// Bayer reconstruction constants.
constexpr int kLutShift = 2;
constexpr int kLutBitWidth = 14;
constexpr int kLutSize = 81792;
constexpr int kClipMax = (1 << kLutBitWidth) - 1;  // 16383

// Step 1: Merge 4 sub-band planes (p1=LL, p2=LH, p3=HH, p4=HL) into
// two planes (out_L, out_H) using a 2D inverse 5/3 wavelet with >>3
// lifting coefficients.
//
// Parameters:
//   p1, p2, p3, p4  — input planes (int32_t)
//   w_rows, w_cols   — stripe dimensions
//   stride1          — row stride for p1/p4 (= bufStripeInts)
//   stride2          — row stride for p2/p3 (= bufStripeInts)
//   out_L, out_H     — output planes (int32_t)
//   stride_out       — row stride for outputs
//   is_first_tile    — boundary handling for row 0
//   is_last_tile     — boundary handling for last row
//
void step1_merge_4_to_2(
    const int32_t* p1, const int32_t* p2,
    const int32_t* p3, const int32_t* p4,
    int w_rows, int w_cols,
    int stride1, int stride2,
    int32_t* out_L, int32_t* out_H,
    int stride_out,
    bool is_first_tile, bool is_last_tile);

// Step 2: Final inverse with tone-curve LUT.
//
// Takes 4 planes (from tile_coeff_buf and step1 output) and produces two
// Bayer rows per IDWT stripe.
//
//   p1 = LL_band (from tile_coeff_buf)
//   p4 = HL_band (from tile_coeff_buf)
//   p2 = L_band  (from step1)
//   p3 = H_band  (from step1)
//
// The LUT indexes with a 32768 midpoint bias and clamps to 14-bit.
//
// Parameters:
//   p1, p2, p3, p4  — input planes
//   w_rows, w_cols   — stripe dimensions
//   stride13         — row stride for p1/p3
//   stride24         — row stride for p2/p4
//   lut              — tone-curve LUT (kLutSize entries)
//   out_bayer        — output uint16 Bayer buffer (image-wide)
//   image_w          - output row stride in uint16_t samples
//   tile_row_start   - starting row within the full image
//   is_first_tile
//   is_last_tile
//
void step2_bayer_rows(
    const int32_t* p1, const int32_t* p2,
    const int32_t* p3, const int32_t* p4,
    int w_rows, int w_cols,
    int stride13, int stride24,
    const int32_t* lut,
    uint16_t* out_bayer,
    int image_w, int tile_row_start,
    bool is_first_tile, bool is_last_tile);

}  // namespace nikon_he

#endif  // LIBRAW_NIKON_HE_BAYER_H
