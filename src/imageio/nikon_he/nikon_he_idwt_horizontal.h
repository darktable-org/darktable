/* -*- C++ -*-
 * File: nikon_he_idwt_horizontal.h
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE horizontal inverse DWT interface

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE horizontal 5/3 inverse DWT.
//
// Two layers of API:
//
// 1. Low-level: idwt53_inverse_one_level() — one synthesis level of the
//    LeGall 5/3 wavelet. Interleaves L (even samples) and H (odd samples),
//    applies UPDATE then PREDICT lifting steps with whole-sample-symmetric
//    boundary extension.
//
// 2. Mid-level: idwt53_horizontal_lift_all() — applies the full multi-level
//    horizontal 5/3 IDWT to one lift-line-block's worth of data.
//    Uses ping-pong buffers (out/work) and processes levels deepest-first,
//    seeding the deepest LL band.
//
// 3. High-level: idwt_horizontal_pass() — processes one pass (A or B) of
//    a precinct's horizontal IDWT, handling both lift LBs and memcpy LBs.
//    Scatters output into a contiguous stripe buffer.
//


#ifndef LIBRAW_NIKON_HE_IDWT_HORIZONTAL_H
#define LIBRAW_NIKON_HE_IDWT_HORIZONTAL_H

#include "nikon_he_subband_config.h"
#include <cstdint>

namespace nikon_he {

// One synthesis level of LeGall 5/3 IDWT.
//
// Given L (n_L even samples) and H (n_H odd samples), produces the
// reconstructed signal in `out`. The output length is n_L + n_H.
//
// Uses whole-sample-symmetric boundary extension at both ends.
//
void idwt53_inverse_one_level(
    const int32_t* L, int n_L,
    const int32_t* H, int n_H,
    int32_t* out);

// Apply the full multi-level horizontal 5/3 IDWT to one lift-LB.
//
// Parameters:
//   in_buf      — source data (L and H sub-bands concatenated at hl_offsets)
//   n           — total number of samples in the reconstructed output
//   levels      — number of wavelet decomposition levels (5 for 5-level LBs)
//   hl_offsets  — array of level offsets into in_buf
//   out         — primary output buffer
//   work        — secondary buffer (same size as out)
//
void idwt53_horizontal_lift_all(
    const int32_t* in_buf,
    int n,
    int levels,
    const int* hl_offsets,
    int32_t* out,
    int32_t* work);

// Process one horizontal pass (A or B) for a precinct row.
//
// Pass A (LBs 0-3): LB 0,1,3 = lift (5-level), LB 2 = memcpy
// Pass B (LBs 4-7): LB 4,5,7 = lift (1-level), LB 6 = memcpy
//
// Parameters:
//   buf           — source buffer (bufA for pass A, bufB for pass B)
//   config        — sub-band layout from compute_subband_layout()
//   is_passA      — true for pass A, false for pass B
//   out_stripe    — output stripe buffer (contiguous reconstructed samples)
//   work          — work buffer (same size as out_stripe)
//
void idwt_horizontal_pass(
    const int32_t* buf,
    const SubbandConfig config[26],
    bool is_passA,
    int32_t* out_stripe,
    int32_t* work);

}  // namespace nikon_he

#endif  // LIBRAW_NIKON_HE_IDWT_HORIZONTAL_H
