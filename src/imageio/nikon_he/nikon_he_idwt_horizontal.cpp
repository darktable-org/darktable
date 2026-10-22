/* -*- C++ -*-
 * File: nikon_he_idwt_horizontal.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE horizontal inverse DWT implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE horizontal 5/3 IDWT — implementation.
//
// See nikon_he_idwt_horizontal.h for the public API.

#include "nikon_he_idwt_horizontal.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace nikon_he {

// Round up to next multiple of m.
static inline int round_up(int x, int m) {
    return ((x + m - 1) / m) * m;
}

// One level of inverse 5/3 lift.
//
// Given low-pass coefficients L (length n_L) and high-pass coefficients H
// (length n_H), reconstruct the n = n_L + n_H samples into out.
//
// Layout: out[2i]   ← L[i],  out[2i+1] ← H[i] interleaved.
// Then inverse UPDATE at even positions and inverse PREDICT at odd positions
// with whole-sample-symmetric boundary extension.
void idwt53_inverse_one_level(
    const int32_t* L, int n_L,
    const int32_t* H, int n_H,
    int32_t* out) {

    const int n = n_L + n_H;
    if (n == 0) return;

    // Step 1: Place L at even positions, H at odd positions.
    for (int i = 0; i < n_L; ++i) {
        const int pos = 2 * i;
        if (pos < n) out[pos] = L[i];
    }
    for (int i = 0; i < n_H; ++i) {
        const int pos = 2 * i + 1;
        if (pos < n) out[pos] = H[i];
    }

    // Step 2: Inverse UPDATE on even positions:
    //   s[i] -= (s[i-1] + s[i+1] + 2) >> 2
    // Whole-sample-symmetric extension: s[-1] = s[1]; s[n] = s[n-2].
    for (int i = 0; i < n; i += 2) {
        const int left  = (i > 0)        ? out[i - 1]
                       : ((n > 1)        ? out[1]     : 0);
        const int right = (i + 1 < n)    ? out[i + 1]
                       : ((i > 0)        ? out[i - 1] : 0);
        out[i] -= (left + right + 2) >> 2;
    }

    // Step 3: Inverse PREDICT on odd positions:
    //   s[i] += (s[i-1] + s[i+1]) >> 1
    for (int i = 1; i < n; i += 2) {
        const int left  = out[i - 1];
        const int right = (i + 1 < n) ? out[i + 1] : out[i - 1];
        out[i] += (left + right) >> 1;
    }
}

// Inverse N-level horizontal 5/3 wavelet for one LB.
//
// in_buf      : per-LB input buffer; LL at [0..N[levels]), H_k at
//               [hl_offsets[k]..) for k = 1..levels. hl_offsets[0] is
//               unused (it's the "stride to next LB" sentinel in the
//               production layout — not a read offset).
// n           : output sample count (= W half-pass width)
// levels      : 5 for pass A, 1 for pass B
// hl_offsets  : array of `levels + 1` entries; only indices 1..levels read
// out         : output buffer of at least n ints
// work        : scratch of at least n ints
//
// After call: out[0..n) holds the reconstructed signal.
void idwt53_horizontal_lift_all(
    const int32_t* in_buf,
    int n,
    int levels,
    const int* hl_offsets,
    int32_t* out,
    int32_t* work) {

    if (levels < 1 || n < 1) return;

    // N[k] = number of samples at level k. N[0] = n; N[k] = ceil(N[k-1]/2).
    std::vector<int> N(levels + 1);
    N[0] = n;
    for (int k = 1; k <= levels; ++k) {
        N[k] = (N[k - 1] + 1) / 2;
    }

    // Ping-pong: choose buffers so the final lift result lands in `out`.
    // After `levels` swaps starting from `cur`, the final cur should == out.
    int32_t* buf_a = out;
    int32_t* buf_b = work;
    int32_t* cur = (levels & 1) ? buf_b : buf_a;
    int32_t* dst = (cur == buf_a) ? buf_b : buf_a;

    // Seed cur with the deepest LL.
    const int ll_size = N[levels];
    for (int i = 0; i < ll_size; ++i) cur[i] = in_buf[i];

    // Lift up, deepest level first.
    for (int k = levels; k >= 1; --k) {
        const int n_L = N[k];
        const int n_H = N[k - 1] - N[k];
        const int32_t* H = in_buf + hl_offsets[k];
        idwt53_inverse_one_level(cur, n_L, H, n_H, dst);
        std::swap(cur, dst);
    }

    // cur now holds the final reconstructed signal in `out` (by parity).
    // (No copy needed if parity was set correctly.)
}

void idwt_horizontal_pass(
    const int32_t* buf,
    const SubbandConfig config[26],
    bool is_passA,
    int32_t* out_stripe,
    int32_t* work) {

    const LayoutInfo* li = config[0].layout_info;
    const int lift_LB    = li->lift_LB;
    const int memcpy_LB  = li->memcpy_LB;
    const int n          = li->ng_LL * 4;       // W = ng_LL * 4 = output samples per LB
    const int lift_st    = lift_LB * 4;

    // Build `hl_offsets` (production convention): `levels+1` entries where
    // entry[0] = end-of-LB sentinel (= round_up cumulative sum × 4), entries
    // [1..levels] = byte offsets of H_k within an LB. Cumulative sums are
    // computed from ng_lift sub-band sizes (round_up to 8 groups).
    //
    // For pass A (5-level, 6 sub-bands per lift LB):
    //   ng_lift = li->ng_lift[0..5]
    //   round_up_ng[i] = round_up(ng_lift[i], 8)
    //   cum[i] = sum of round_up_ng[0..i]
    //   hl_offsets_in_groups[k] = cum[5 - k] for k=0..5
    //   hl_offsets_in_ints[k]   = 4 * hl_offsets_in_groups[k]
    auto build_hl = [&](int levels, const int* ng_lift,
                        std::vector<int>& out_offsets) {
        std::vector<int> cum(levels + 1, 0);
        for (int i = 0; i < levels + 1; ++i) {
            cum[i] = (i == 0 ? 0 : cum[i - 1])
                   + (i < levels + 1 ? round_up(ng_lift[i], 8) : 0);
        }
        out_offsets.assign(levels + 1, 0);
        // Reverse cumulative: hl_offsets[k] = 4 * cum[levels - k].
        // For k=0: 4 * cum[levels] = end-of-LB sentinel (unused).
        // For k=1..levels: 4 * cum[levels - k] = start of H at level k.
        for (int k = 0; k <= levels; ++k) {
            out_offsets[k] = 4 * cum[levels - k];
        }
    };

    if (is_passA) {
        // Pass A: 4 LBs. LB 0/1/3 are lift LBs (5 levels). LB 2 is memcpy.
        const int passA_levels = 5;
        std::vector<int> hl_offsets;
        build_hl(passA_levels, li->ng_lift, hl_offsets);

        // Place LBs in out_stripe LB-ordered:
        //   LB 0 at 0, LB 1 at lift_st, LB 2 at 2*lift_st (memcpy),
        //   LB 3 at 2*lift_st + memcpy_LB*4
        const int lb0_out = 0;
        const int lb1_out = lift_st;
        const int lb2_out = 2 * lift_st;
        const int lb3_out = 2 * lift_st + memcpy_LB * 4;

        // Each lift LB has its own bufA region of size lift_LB*4 ints.
        // LB k starts at int offset k * lift_LB*4 within bufA. (LB 2 is at
        // 2*lift_LB*4 with size memcpy_LB*4; LB 3 follows.)
        const int32_t* lb0_in = buf + 0;
        const int32_t* lb1_in = buf + lift_st;
        const int32_t* lb3_in = buf + 2 * lift_st + memcpy_LB * 4;  // = passA_stride*1 ints in

        idwt53_horizontal_lift_all(lb0_in, n, passA_levels,
                                   hl_offsets.data(),
                                   out_stripe + lb0_out, work);
        idwt53_horizontal_lift_all(lb1_in, n, passA_levels,
                                   hl_offsets.data(),
                                   out_stripe + lb1_out, work);

        // LB 2: memcpy LL band (sb 12). sb 12's data is at config[12].x24*4
        // within bufA, with ng_LL coefs. Copy to out_stripe + lb2_out.
        std::memcpy(out_stripe + lb2_out,
                    buf + config[12].x24 * 4,
                    static_cast<size_t>(config[12].ng * 4)
                    * sizeof(int32_t));

        idwt53_horizontal_lift_all(lb3_in, n, passA_levels,
                                   hl_offsets.data(),
                                   out_stripe + lb3_out, work);
    } else {
        // Pass B: 4 LBs, 1-level lift (2 sub-bands each), LB 6 memcpy.
        const int passB_levels = 1;
        // For pass B, ng_lift conceptually is [ng_max, ng_max] (2 sub-bands).
        const int ng_passB[2] = { li->ng_max, li->ng_max };
        std::vector<int> hl_offsets;
        build_hl(passB_levels, ng_passB, hl_offsets);

        const int lb4_out = 0;
        const int lb5_out = lift_st;
        const int lb6_out = 2 * lift_st;
        const int lb7_out = 2 * lift_st + memcpy_LB * 4;

        // For pass B, bufB layout (after the +bufStripeInts offset by caller):
        //   LB 4 at 0, LB 5 at lift_st, LB 6 at 2*lift_st, LB 7 at 2*lift_st + memcpy_LB*4
        const int32_t* lb4_in = buf + 0;
        const int32_t* lb5_in = buf + lift_st;
        const int32_t* lb7_in = buf + 2 * lift_st + memcpy_LB * 4;

        idwt53_horizontal_lift_all(lb4_in, n, passB_levels,
                                   hl_offsets.data(),
                                   out_stripe + lb4_out, work);
        idwt53_horizontal_lift_all(lb5_in, n, passB_levels,
                                   hl_offsets.data(),
                                   out_stripe + lb5_out, work);

        // LB 6: memcpy of sb 23 (LL of pass B).
        std::memcpy(out_stripe + lb6_out,
                    buf + config[23].x24 * 4,
                    static_cast<size_t>(config[23].ng * 4)
                    * sizeof(int32_t));

        idwt53_horizontal_lift_all(lb7_in, n, passB_levels,
                                   hl_offsets.data(),
                                   out_stripe + lb7_out, work);
    }
}

}  // namespace nikon_he
