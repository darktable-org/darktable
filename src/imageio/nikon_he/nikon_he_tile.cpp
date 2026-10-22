/* -*- C++ -*-
 * File: nikon_he_tile.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE per-tile decode orchestration implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE tile orchestrator — implementation.
//
// See nikon_he_tile.h for the public API.

#include "nikon_he_tile.h"
#include "nikon_he_precinct_decode.h"
#include <cstring>
#include <vector>

namespace nikon_he {

// --- helpers ---------------------------------------------------------------

// Run one ver_lift "loop" over 4 LB components.
//
// `x0_per_lb[k]` is the source for LB k (or nullptr if all sources are nullptr
// for a partial-tile tail call). `x1_base` is the destination base in
// tile_coeff_buf's tile_buf. lb_stride pattern is {lift_st, lift_st, 0, lift_st}: LB 2
// is the memcpy LB and ticks state without writing.
//
// The x1 cursor advances by lb_stride[k] between LBs.
// Because lb_stride[2]==0, LB 3 ends up at offset 2*lift_st
// from x1_base — same slot LB 2 would have occupied. LB 2's data is
// written separately via memcpy_cursor.
//
// Each LB's x2/x3 carry buffers live at fixed per-LB offsets in the
// tile-wide carry arrays; we use k * lift_st as the per-LB offset.
static void run_one_ver_lift_loop(const int32_t* const x0_per_lb[4],
                                  int32_t* x1_base,
                                  int32_t* x2_carry_tile,
                                  int32_t* x3_carry_tile,
                                  VerLiftStatePerLB st[4],
                                  int lift_st) {
    int lb_stride[4] = { lift_st, lift_st, 0, lift_st };

    int32_t* x1 = x1_base;
    for (int k = 0; k < 4; ++k) {
        int n = lb_stride[k];
        // Per-LB carry buffers (fixed per LB, persisted across precincts).
        st[k].x2_carry = x2_carry_tile + k * lift_st;
        st[k].x3_carry = x3_carry_tile + k * lift_st;
        ver_lift_lb_step(x0_per_lb[k], x1, n, st[k]);
        if (x1 != nullptr) x1 += n;
    }
}

// --- main ------------------------------------------------------------------

TileDecodeResult decode_tile(
    const uint8_t* const* precinct_data,
    const size_t* precinct_sizes,
    const PictureHeader& header,
    const SubbandConfig config[26],
    PrecinctPredecessorState& pred_state,
    const uint8_t* predict_lut,
    int32_t* tile_coeff_buf,
    int32_t* overflow_carry,
    bool is_first_tile,
    int precinct_count) {

    TileDecodeResult result = {0, false};

    const LayoutInfo* li    = config[0].layout_info;
    const int stripe_ints   = compute_buf_stripe_ints(li);   // = 4*passA_stride
    const int passA_stride  = 3 * li->lift_LB + li->memcpy_LB;
    const int lift_st       = li->lift_LB * 4;                // = kBand
    const int memcpy_st     = li->memcpy_LB * 4;
    const int kBand         = lift_st;

    // Per-tile state carry buffers (one int32 region per LB component).
    std::vector<int32_t> x2_carry(4 * lift_st, 0);
    std::vector<int32_t> x3_carry(4 * lift_st, 0);

    // Per-LB phase state. All 4 LBs (including memcpy LB 2) start at the
    // same state — even when n==0, the state still advances.
    VerLiftStatePerLB ver_st[4];
    for (int k = 0; k < 4; ++k) {
        ver_st[k].state = is_first_tile ? VerLiftState::kInit2
                                        : VerLiftState::kInit0;
        ver_st[k].x2_carry = x2_carry.data() + k * lift_st;
        ver_st[k].x3_carry = x3_carry.data() + k * lift_st;
        ver_st[k].failed = false;
    }

    // Per-precinct working buffers.
    const int kBufInts = 4 * passA_stride;
    std::vector<int32_t> bufA(kBufInts, 0);
    std::vector<int32_t> bufB(kBufInts, 0);
    std::vector<int32_t> h_out(4 * lift_st, 0);
    std::vector<int32_t> h_work(4 * lift_st + 16, 0);

    // Tile-wide write target — sized generously to hold the tile + tail
    // overflow without bleeding into tile_coeff_buf's next-tile region.
    constexpr int kMaxStripes = 40;            // 32 + tail + headroom
    std::vector<int32_t> tile_buf(kMaxStripes * stripe_ints, 0);

    // For tile k>0, seed the first 2 stripes from previous tile's overflow.
    if (!is_first_tile) {
        std::memcpy(tile_buf.data(), overflow_carry,
                    static_cast<size_t>(2 * stripe_ints) * sizeof(int32_t));
    }

    // x1_write_offset: per-loop x1 write offset (advances by 4 if pre-tick state>5)
    // memcpy_cursor: per-call memcpy_out cursor (advances by stripe_ints)
    int x1_write_offset = is_first_tile ? 0 : 8;
    int memcpy_cursor = 0;

    // h_out layout produced by idwt_horizontal_pass (LB-ordered):
    //   LB 0     at h_out + 0
    //   LB 1     at h_out + lift_st
    //   LB 2/6   at h_out + 2*lift_st   (memcpy LL)
    //   LB 3/7   at h_out + 2*lift_st + memcpy_st
    //
    // For ver_lift we feed LB 0/1/3 only (LB 2 is memcpy via memcpy_cursor; n=0
    // ticks its state without reading). Build per-LB x0 pointers per call.
    // A vertical lift tick writes LBs 0, 1 and 3 (lift_st ints each) from x1's
    // offset; it fails when that would pass tile_buf's end, or when an LB was
    // given no rows in a state that needs them.
    auto tick = [&](const int32_t* const x0_per_lb[4]) -> bool {
        const size_t at = static_cast<size_t>(x1_write_offset) * passA_stride;
        if (x1_write_offset < 0 || at + 3 * static_cast<size_t>(lift_st) > tile_buf.size())
            return false;
        const int pre_tick = ver_st[0].state;
        run_one_ver_lift_loop(x0_per_lb, tile_buf.data() + at,
                              x2_carry.data(), x3_carry.data(),
                              ver_st, lift_st);
        for (int k = 0; k < 4; ++k)
            if (ver_st[k].failed) return false;
        if (pre_tick > 5) x1_write_offset += 4;
        return true;
    };
    auto run_passA_or_B_verlift = [&](const int32_t* h) -> bool {
        const int32_t* x0_per_lb[4] = { h, h + lift_st, nullptr, h + 2 * lift_st + memcpy_st };
        return tick(x0_per_lb);
    };
    // The memcpy LB's LL band goes to tile_buf[memcpy_cursor + 3*kBand].
    auto copy_ll = [&](const int32_t* h) -> bool {
        const size_t at = static_cast<size_t>(memcpy_cursor) + 3 * static_cast<size_t>(kBand);
        if (at + memcpy_st > tile_buf.size()) return false;
        std::memcpy(tile_buf.data() + at, h + 2 * lift_st,
                    static_cast<size_t>(memcpy_st) * sizeof(int32_t));
        memcpy_cursor += stripe_ints;
        return true;
    };

    // --- main per-precinct loop ---
    for (int p = 0; p < precinct_count; ++p) {
        // 1. Entropy → bufA, bufB
        std::memset(bufA.data(), 0, bufA.size() * sizeof(int32_t));
        std::memset(bufB.data(), 0, bufB.size() * sizeof(int32_t));
        PrecinctDecodeResult prec = decode_precinct(
            precinct_data[p], precinct_sizes[p], header,
            config, pred_state, predict_lut, bufA.data(), bufB.data());
        if (!prec.success) return result;
        result.precincts_decoded++;

        // 2. Pass A h1_l12 → h_out (LB-ordered layout).
        std::memset(h_out.data(), 0, h_out.size() * sizeof(int32_t));
        std::memset(h_work.data(), 0, h_work.size() * sizeof(int32_t));
        idwt_horizontal_pass(bufA.data(), config, /*is_passA=*/true,
                             h_out.data(), h_work.data());

        // 3. Write LB 2 (memcpy LL) to tile_buf[memcpy_cursor + 3*kBand].
        //    Source = h_out's LB 2 region (= 2*lift_st .. 2*lift_st+memcpy_st).
        if (!copy_ll(h_out.data())) return result;

        // 4. ver_lift pass A (skip for path-B prec 0).
        const bool skip_passA_verlift = (!is_first_tile && p == 0);
        if (!skip_passA_verlift && !run_passA_or_B_verlift(h_out.data())) return result;

        // 5. Pass B h1_l12 → h_out.
        std::memset(h_out.data(), 0, h_out.size() * sizeof(int32_t));
        std::memset(h_work.data(), 0, h_work.size() * sizeof(int32_t));
        idwt_horizontal_pass(bufB.data(), config, /*is_passA=*/false,
                             h_out.data(), h_work.data());

        // 6. LB 6 memcpy.
        if (!copy_ll(h_out.data())) return result;

        // 7. ver_lift pass B.
        if (!run_passA_or_B_verlift(h_out.data())) return result;
    }

    // Partial-tile tail loop: 2 extra ver_lift calls with x0=NULL.
    // Flushes states 7/8 → 9 → 11 for the last few stripes.
    const int32_t* tail_x0[4] = { nullptr, nullptr, nullptr, nullptr };
    // a final tile with just one precinct already has both lifted rows in the
    // incoming overlap; its only new rows belong to the untransformed LL plane
    if (precinct_count >= 2 || is_first_tile)
        for (int tail = 0; tail < 2; ++tail)
            if (!tick(tail_x0)) return result;

    // Copy first 32 stripes of tile_buf to tile_coeff_buf at this tile's region.
    int32_t* tile_out = tile_coeff_buf;
    std::memcpy(tile_out, tile_buf.data(),
                static_cast<size_t>(kStripesPerTile * stripe_ints)
                * sizeof(int32_t));

    // Save next 2 stripes as overflow for the next tile.
    std::memcpy(overflow_carry,
                tile_buf.data() + kStripesPerTile * stripe_ints,
                static_cast<size_t>(2 * stripe_ints) * sizeof(int32_t));

    result.success = true;
    return result;
}

}  // namespace nikon_he
