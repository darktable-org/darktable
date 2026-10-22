/* -*- C++ -*-
 * File: nikon_he_bayer.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE Bayer reconstruction implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE Bayer reconstruction — implementation.
//
// See nikon_he_bayer.h for the public API.

#include "nikon_he_bayer.h"
#include <algorithm>
#include <cstring>

namespace nikon_he {

void step1_merge_4_to_2(
    const int32_t* p1, const int32_t* p2,
    const int32_t* p3, const int32_t* p4,
    int w_rows, int w_cols,
    int stride1, int stride2,
    int32_t* out_L, int32_t* out_H,
    int stride_out,
    bool is_first_tile, bool is_last_tile) {

    if (w_rows <= 0 || w_cols <= 0) return;

    for (int r = 0; r < w_rows; r++) {
        int prev_r = (r == 0 && is_first_tile) ? 0 : r - 1;
        int next_r = (r == w_rows - 1 && is_last_tile) ? r : r + 1;

        // Column c = 0 (boundary)
        {
            int c = 0;
            int cp = (w_cols == 1) ? 0 : 1;

            // p3 indices use stride2
            const int32_t* p3_row = p3 + r * stride2;
            const int32_t* p3_prev = p3 + prev_r * stride2;
            const int32_t* p3_next = p3 + next_r * stride2;
            const int32_t* p1_row = p1 + r * stride1;
            const int32_t* p1_next = p1 + next_r * stride1;
            const int32_t* p4_row = p4 + r * stride1;
            const int32_t* p4_prev = p4 + prev_r * stride1;
            const int32_t* p2_row = p2 + r * stride2;
            const int32_t* p2_next = p2 + next_r * stride2;

            int32_t hh_sum = p3_row[c] + p3_row[cp];
            int32_t lh_predicted = p2_row[c] - ((hh_sum + p3_prev[c] + p3_prev[cp]) >> 3);
            int32_t lh_next_predicted = p2_next[c] - ((hh_sum + p3_next[c] + p3_next[cp]) >> 3);
            // The image's last row extends the prediction already made, not the clamped source.
            if (is_last_tile && r == w_rows - 1)
                lh_next_predicted = lh_predicted;

            int32_t ll_hl_sum = p1_row[c] + p4_row[c];
            out_L[r * stride_out + c] = lh_predicted
                - ((ll_hl_sum + p1_row[cp] + p4_prev[c]) >> 3);

            int32_t pred_2x = (lh_predicted + lh_next_predicted) * 2;
            int32_t hh_predicted = p3_row[c]
                - ((ll_hl_sum + p4_row[c] + p1_next[c]) >> 3);
            out_H[r * stride_out + c] = hh_predicted + (pred_2x >> 2);
        }

        // Carry from c=0 to c=1
        int32_t carry_pred = 0, carry_next_pred = 0;
        {
            int c = 0;
            int cp = (w_cols == 1) ? 0 : 1;
            const int32_t* p3_row = p3 + r * stride2;
            const int32_t* p3_prev = p3 + prev_r * stride2;
            const int32_t* p3_next = p3 + next_r * stride2;
            const int32_t* p2_row = p2 + r * stride2;
            const int32_t* p2_next = p2 + next_r * stride2;

            int32_t hh_sum = p3_row[c] + p3_row[cp];
            carry_pred = p2_row[c] - ((hh_sum + p3_prev[c] + p3_prev[cp]) >> 3);
            carry_next_pred = p2_next[c] - ((hh_sum + p3_next[c] + p3_next[cp]) >> 3);
            if (is_last_tile && r == w_rows - 1)
                carry_next_pred = carry_pred;
        }

        // Columns c = 1 .. w_cols - 1
        for (int c = 1; c < w_cols; c++) {
            int cp = (c == w_cols - 1) ? c : c + 1;

            const int32_t* p3_row = p3 + r * stride2;
            const int32_t* p3_prev = p3 + prev_r * stride2;
            const int32_t* p3_next = p3 + next_r * stride2;
            const int32_t* p1_row = p1 + r * stride1;
            const int32_t* p1_next = p1 + next_r * stride1;
            const int32_t* p4_row = p4 + r * stride1;
            const int32_t* p4_prev = p4 + prev_r * stride1;
            const int32_t* p2_row = p2 + r * stride2;
            const int32_t* p2_next = p2 + next_r * stride2;

            int32_t hh_col_sum = p3_row[c] + p3_row[cp];
            int32_t cur_predict = p2_row[c]
                - ((hh_col_sum + p3_prev[cp] + p3_prev[c]) >> 3);
            int32_t next_predict = p2_next[c]
                - ((hh_col_sum + p3_next[c] + p3_next[cp]) >> 3);
            if (is_last_tile && r == w_rows - 1)
                next_predict = cur_predict;

            int32_t ll_hl_col_sum = p1_row[c] + p4_row[c];
            out_L[r * stride_out + c] = cur_predict
                - ((ll_hl_col_sum + p1_row[cp] + p4_prev[c]) >> 3);

            int32_t pred_sum_4way = carry_pred + cur_predict + next_predict + carry_next_pred;
            int32_t hh_col_predicted = p3_row[c]
                - ((ll_hl_col_sum + p4_row[c - 1] + p1_next[c]) >> 3);
            out_H[r * stride_out + c] = hh_col_predicted + (pred_sum_4way >> 2);

            carry_pred = cur_predict;
            carry_next_pred = next_predict;
        }
    }
}

void step2_bayer_rows(
    const int32_t* p1, const int32_t* p2,
    const int32_t* p3, const int32_t* p4,
    int w_rows, int w_cols,
    int stride13, int stride24,
    const int32_t* lut,
    uint16_t* out_bayer,
    int image_w, int tile_row_start,
    bool is_first_tile, bool is_last_tile) {

    const int32_t lut_rounding = (kLutShift > 0) ? (1 << (kLutShift - 1)) : 0;
    const int32_t midpoint_bias = 1 << (kLutShift + kLutBitWidth - 1);
    const int32_t lut_shift = kLutShift & 0x1f;

    auto lookup = [&](int32_t v) -> uint16_t {
        int idx = (v < 0) ? 0 : ((v >= kLutSize) ? kLutSize - 1 : v);
        int32_t val = (lut[idx] + lut_rounding) >> lut_shift;
        if (val < 0) val = 0;
        if (val > kClipMax) val = kClipMax;
        return static_cast<uint16_t>(val);
    };

    for (int r = 0; r < w_rows; r++) {
        int prev_r = (r == 0 && is_first_tile) ? 0 : r - 1;
        int next_r = (r == w_rows - 1 && is_last_tile) ? r : r + 1;

        int bayer_row_top = tile_row_start + 2 * r;
        int bayer_row_bot = tile_row_start + 2 * r + 1;

        uint16_t* top_row = (bayer_row_top < 0) ? nullptr : out_bayer + bayer_row_top * image_w;
        uint16_t* bot_row = (bayer_row_bot < 0) ? nullptr : out_bayer + bayer_row_bot * image_w;

        // Column c = 0
        {
            int c = 0;
            int cp = (w_cols == 1) ? 0 : 1;

            const int32_t* p3_row = p3 + r * stride24;
            const int32_t* p3_prev = p3 + prev_r * stride24;
            const int32_t* p2_row = p2 + r * stride24;
            const int32_t* p2_next = p2 + next_r * stride24;
            const int32_t* p4_row = p4 + r * stride13;
            const int32_t* p1_row = p1 + r * stride13;

            int32_t hh_c = p3_row[c];
            int32_t hh_cp = p3_row[cp];
            int32_t lh_c = p2_row[c];
            int32_t lh_next_c = p2_next[c];
            int32_t hl_c = p4_row[c];

            if (top_row) {
                int32_t sum_top0 = ((p3_prev[c] + lh_c + hh_c + lh_c) >> 2)
                                   + midpoint_bias + p1_row[c];
                top_row[2 * c] = lookup(sum_top0);
                int32_t sum_top1 = lh_c + midpoint_bias;
                top_row[2 * c + 1] = lookup(sum_top1);
            }

            if (bot_row) {
                int32_t sum_bot0 = hh_c + midpoint_bias;
                bot_row[2 * c] = lookup(sum_bot0);
                int32_t sum_bot1 = hl_c + midpoint_bias
                                   + ((hh_cp + hh_c + lh_c + lh_next_c) >> 2);
                bot_row[2 * c + 1] = lookup(sum_bot1);
            }
        }

        // Columns c = 1 .. w_cols - 1
        for (int c = 1; c < w_cols; c++) {
            int cp = (c == w_cols - 1) ? c : c + 1;

            const int32_t* p3_row = p3 + r * stride24;
            const int32_t* p3_prev = p3 + prev_r * stride24;
            const int32_t* p2_row = p2 + r * stride24;
            const int32_t* p2_next = p2 + next_r * stride24;
            const int32_t* p4_row = p4 + r * stride13;
            const int32_t* p1_row = p1 + r * stride13;

            int32_t hh_val_c = p3_row[c];
            int32_t hh_val_cp = p3_row[cp];
            int32_t hh_plus_lh_c = hh_val_c + p2_row[c];
            int32_t lh_next_c2 = p2_next[c];
            int32_t hl_val_c = p4_row[c];

            if (top_row) {
                int32_t sum_top0 = ((p2_row[c - 1] + p3_prev[c] + hh_plus_lh_c) >> 2)
                                   + midpoint_bias + p1_row[c];
                top_row[2 * c] = lookup(sum_top0);
                int32_t sum_top1 = p2_row[c] + midpoint_bias;
                top_row[2 * c + 1] = lookup(sum_top1);
            }

            if (bot_row) {
                int32_t sum_bot0 = hh_val_c + midpoint_bias;
                bot_row[2 * c] = lookup(sum_bot0);
                int32_t sum_bot1 = hl_val_c + midpoint_bias
                                   + ((hh_val_cp + hh_plus_lh_c + lh_next_c2) >> 2);
                bot_row[2 * c + 1] = lookup(sum_bot1);
            }
        }
    }
}

}  // namespace nikon_he
