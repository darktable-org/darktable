/* -*- C++ -*-
 * File: nikon_he_subband_config.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE sub-band layout configuration implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE sub-band layout configuration — implementation.
//
// See nikon_he_subband_config.h for the public API.

#include "nikon_he_subband_config.h"
#include <algorithm>
#include <cstring>

namespace nikon_he {

static inline int round_up(int x, int m) {
    return ((x + m - 1) / m) * m;
}

void compute_subband_layout(int half_pass_W, SubbandConfig out[26], LayoutInfo& layout_info) {
    // --- Core sizes ---
    int ng_max = (half_pass_W + 7) / 8;   // ceil(W/8)
    int ng_LL  = half_pass_W / 4;

    // Coefficients per band, as JPEG XS's wavelet splits a line: each level's low
    // band keeps ceil(n / 2) samples and its high band floor(n / 2). A code group
    // is 4 coefficients.
    auto groups = [](int coefficients) { return (coefficients + 3) / 4; };
    int low[6], high[6];
    low[0] = half_pass_W;
    high[0] = 0;
    for (int k = 1; k <= 5; ++k) {
        high[k] = low[k - 1] / 2;
        low[k]  = low[k - 1] - high[k];
    }

    // Per-sub-band ng for a 5-level lift LB (LBs 0, 1, 3): LL5, H5, H4, H3, H2, H1.
    int ng_5level[6] = {
        groups(low[5]), groups(high[5]), groups(high[4]),
        groups(high[3]), groups(high[2]), groups(high[1]),
    };
    // A 1-level lift LB (LBs 4, 5, 7): L1, H1.
    const int ng_1level[2] = { groups(low[1]), groups(high[1]) };

    // --- LB region sizes ---
    int memcpy_LB = round_up(ng_LL, 32);
    int lift_LB   = memcpy_LB + round_up((ng_max + 15) / 16, 32);

    // --- Sub-band definitions ---
    //
    // LB 0: sb 0-5  (6 sb, lift 5-level, bufA) ng = ng_5level[]
    // LB 1: sb 6-11 (6 sb, lift 5-level, bufA) ng = ng_5level[]
    // LB 2: sb 12   (1 sb, memcpy LL,    bufA) ng = ng_LL
    // LB 3: sb 13-18(6 sb, lift 5-level, bufA) ng = ng_5level[]
    // LB 4: sb 19-20(2 sb, lift 1-level, bufB) ng = ng_max
    // LB 5: sb 21-22(2 sb, lift 1-level, bufB) ng = ng_max
    // LB 6: sb 23   (1 sb, memcpy LL,    bufB) ng = ng_LL
    // LB 7: sb 24-25(2 sb, lift 1-level, bufB) ng = ng_max
    //
    // Priorities:
    //   {0,1,2,3,4,5, 18,19,20,21,22,23, 36, 54,55,56,57,58,59, 6,7, 24,25, 36, 60,61}

    struct SbDef { int sb; int lb; int ng_val; int priority; };

    const SbDef defs[26] = {
        // LB 0: sb 0-5, priorities 0-5
        { 0, 0, ng_5level[0],  0},
        { 1, 0, ng_5level[1],  1},
        { 2, 0, ng_5level[2],  2},
        { 3, 0, ng_5level[3],  3},
        { 4, 0, ng_5level[4],  4},
        { 5, 0, ng_5level[5],  5},
        // LB 1: sb 6-11, priorities 18-23
        { 6, 1, ng_5level[0], 18},
        { 7, 1, ng_5level[1], 19},
        { 8, 1, ng_5level[2], 20},
        { 9, 1, ng_5level[3], 21},
        {10, 1, ng_5level[4], 22},
        {11, 1, ng_5level[5], 23},
        // LB 2: sb 12, priority 36 (memcpy LL)
        {12, 2, ng_LL,        36},
        // LB 3: sb 13-18, priorities 54-59
        {13, 3, ng_5level[0], 54},
        {14, 3, ng_5level[1], 55},
        {15, 3, ng_5level[2], 56},
        {16, 3, ng_5level[3], 57},
        {17, 3, ng_5level[4], 58},
        {18, 3, ng_5level[5], 59},
        // LB 4: sb 19-20, priorities 6-7 (1-level lift)
        {19, 4, ng_1level[0],  6},
        {20, 4, ng_1level[1],  7},
        // LB 5: sb 21-22, priorities 24-25
        {21, 5, ng_1level[0], 24},
        {22, 5, ng_1level[1], 25},
        // LB 6: sb 23, priority 36 (memcpy LL)
        {23, 6, ng_LL,        36},
        // LB 7: sb 24-25, priorities 60-61
        {24, 7, ng_1level[0], 60},
        {25, 7, ng_1level[1], 61},
    };

    // --- HL offset tables ---
    // The 6-entry ng_lift array holds the wavelet-level-level ng values
    // for the 5-level decomposition, used by horizontal IDWT.
    int ng_lift_for_hl[6];
    for (int i = 0; i < 6; i++) ng_lift_for_hl[i] = ng_5level[i];

    int passA_hl_offset[6] = {0};
    int cumsum = 0;
    for (int i = 0; i < 6; i++) {
        passA_hl_offset[i] = cumsum;
        cumsum += round_up(ng_lift_for_hl[i], 8);
    }

    // Pass B: 1-level lift has just 2 sub-bands (ng_max, ng_max)
    int passB_hl_offset[2] = {0, round_up(ng_max, 8)};

    // --- Shared LayoutInfo ---
    layout_info = LayoutInfo{};
    layout_info.ng_max = ng_max;
    layout_info.ng_LL  = ng_LL;
    std::memcpy(layout_info.ng_lift, ng_lift_for_hl, sizeof(ng_lift_for_hl));
    layout_info.memcpy_LB = memcpy_LB;
    layout_info.lift_LB   = lift_LB;
    std::memcpy(layout_info.passA_hl_offset, passA_hl_offset, sizeof(passA_hl_offset));
    std::memcpy(layout_info.passB_hl_offset, passB_hl_offset, sizeof(passB_hl_offset));

    // --- Compute x24 scatter offsets ---
    // x24 accumulates across LBs. LB region sizes define how much space
    // each LB occupies in the buffer (in x24 units = groups).
    //   lift LBs:   lift_LB groups
    //   memcpy LBs: memcpy_LB groups
    //
    // Within each LB, sub-bands are packed with round_up(ng, 8) spacing.

    // But x24 is buffer-local: bufA and bufB each accumulate independently.
    // LB positions within bufA: LB 0 at x24=0, LB 1 at lift_LB, LB 2 at 2*lift_LB,
    //                           LB 3 at 2*lift_LB+memcpy_LB
    // LB positions within bufB: LB 4 at x24=0, LB 5 at lift_LB, LB 6 at 2*lift_LB,
    //                           LB 7 at 2*lift_LB+memcpy_LB

    // Track within-LB sb_x24 per LB separately.
    int lb_sb_x24[8] = {0};
    // Track cumulative x24 base per LB within each buffer.
    int bufA_lb_base[4] = {0, lift_LB, 2 * lift_LB, 2 * lift_LB + memcpy_LB};
    int bufB_lb_base[4] = {0, lift_LB, 2 * lift_LB, 2 * lift_LB + memcpy_LB};

    std::memset(out, 0, sizeof(SubbandConfig) * 26);

    for (int i = 0; i < 26; i++) {
        const SbDef& d = defs[i];
        SubbandConfig& cfg = out[d.sb];

        cfg.ng = d.ng_val;
        cfg.priority = d.priority;
        cfg.buffer_idx = (d.lb >= 4) ? 1 : 0;
        cfg.layout_info = &layout_info;

        int lb = d.lb;
        if (cfg.buffer_idx == 0) {
            cfg.x24 = bufA_lb_base[lb] + lb_sb_x24[lb];
            lb_sb_x24[lb] += round_up(d.ng_val, 8);
        } else {
            int blb = lb - 4; // local LB index within bufB (0..3)
            cfg.x24 = bufB_lb_base[blb] + lb_sb_x24[lb];
            lb_sb_x24[lb] += round_up(d.ng_val, 8);
        }
    }

    // A packet's significance flags: one bit per significance group (8 code groups)
    // of each of its sub-bands, padded to a whole byte.
    int sig_bits[8] = {0};
    for (int i = 0; i < 26; i++)
        sig_bits[defs[i].lb] += (defs[i].ng_val + 7) / 8;
    for (int lb = 0; lb < 8; lb++)
        layout_info.sig_bytes[lb] = (sig_bits[lb] + 7) / 8;
}

}  // namespace nikon_he
