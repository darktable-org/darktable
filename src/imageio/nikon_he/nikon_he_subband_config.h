/* -*- C++ -*-
 * File: nikon_he_subband_config.h
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE sub-band layout configuration interface

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE sub-band layout configuration.
//
// Computes the spatial layout of 26 sub-bands within 8 line-blocks (LBs)
// for one pass of a half-width image row. The output tables drive scatter
// address computation, horizontal IDWT ordering, and buffer assignment
// throughout the per-precinct and per-tile pipelines.
//
// Layout summary (8 LBs, 26 sub-bands):
//   LB 0 (lift):    sb 0-5        -> buffer A
//   LB 1 (lift):    sb 6-11       -> buffer A
//   LB 2 (memcpy):  sb 12 (LL)    -> buffer A
//   LB 3 (lift):    sb 13-18      -> buffer A
//   LB 4 (lift):    sb 19-20      -> buffer B
//   LB 5 (lift):    sb 21-22      -> buffer B
//   LB 6 (memcpy):  sb 23 (LL)    -> buffer B
//   LB 7 (lift):    sb 24-25      -> buffer B
//
// The LayoutInfo struct is shared across all 26 SubbandConfig entries
// (they all point to the same instance), providing computed region sizes,
// HL offset tables, and ng counts for the entire layout.

#ifndef LIBRAW_NIKON_HE_SUBBAND_CONFIG_H
#define LIBRAW_NIKON_HE_SUBBAND_CONFIG_H

#include <cstdint>

namespace nikon_he {

// Static layout information shared across all sub-bands.
// Computed once per image width, drives both passes.
struct LayoutInfo {
    int ng_max;     // ceil(W / 8)
    int ng_LL;      // W / 4

    // Per-LB ng_lift values for the 6 lift LBs (0, 1, 3, 4, 5, 7).
    // Indexed by lift-LB-local index (0-5).
    int ng_lift[6];

    // LB region sizes (in coefficient-bytes, but stored as coefficient count).
    int memcpy_LB;  // round_up(ng_LL, 32)
    int lift_LB;    // memcpy_LB + round_up(ceil(ng_max / 16), 32)

    // HL (high/low) offset tables for horizontal IDWT.
    // passA[0..5]: cumsum of round_up(ng_lift[i], 8) for pass A lift LBs.
    // passB[0..1]: cumsum for pass B lift LBs.
    int passA_hl_offset[6];
    int passB_hl_offset[2];

    // Bytes of significance flags in each LB's packet (its sub-bands' significance
    // groups, padded to a byte); a raw-GCLI packet has none.
    int sig_bytes[8];
};

// One entry per sub-band (0..25).
struct SubbandConfig {
    // Number of coefficient groups in this sub-band.
    // Each group has 4 coefficients → 4*ng int32_t coefficients.
    int ng;

    // Scatter offset into the line-buffer (bufA or bufB).
    // Write 4*ng coefficients starting at buffer[x24 * 4].
    int x24;

    // Processing priority (1..26). Lower = earlier decode.
    int priority;

    // Which buffer this sub-band writes into: 0 = bufA, 1 = bufB.
    int buffer_idx;

    // Pointer to the shared layout info (all entries point to same struct).
    const LayoutInfo* layout_info;
};

// Compute the full sub-band layout for a given half-pass width.
//
// half_pass_W = image_width / 2  (full-res Z9: 8256 → 4128).
//
// Populates `out` (26 entries) with ng, x24, priority, buffer_idx, fills
// `layout`, and points every entry's layout_info at it. The caller owns
// `layout`, one per decode, so decodes on several threads don't share it.
//
void compute_subband_layout(int half_pass_W, SubbandConfig out[26], LayoutInfo& layout);

}  // namespace nikon_he

#endif  // LIBRAW_NIKON_HE_SUBBAND_CONFIG_H
