/* -*- C++ -*-
 * File: nikon_he_predict_lut.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE prediction lookup table builder

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE GCLI prediction lookup table — implementation.
//
// The table maps (gtli, previous_gcli, unary_code) → predicted_gcli.
//
// Key concepts:
//   m_top     = max(previous_gcli, gtli)      — baseline prediction
//   threshold = m_top - gtli                   — zigzag range
//   The unary code u encodes a delta from m_top:
//     u=0        → delta=0
//     u=1..2*thr → zigzag: -1, +1, -2, +2, ...
//     u > 2*thr  → one-sided escape: u - threshold

#include "nikon_he_predict_lut.h"
#include <algorithm>
#include <array>

namespace nikon_he {

// Built once; a function-local static's initialisation is thread-safe, so decodes on
// several threads can share it.
static std::array<uint8_t, kPredictionLutSize> make_prediction_lookup_table() {
    std::array<uint8_t, kPredictionLutSize> lut{};
    for (int gtli = 0; gtli < 16; ++gtli) {
        for (int previous_gcli = 0; previous_gcli < 16; ++previous_gcli) {
            int m_top = std::max(previous_gcli, gtli);
            int threshold = m_top - gtli;
            int max_stored = 15 + threshold;

            for (int unary_code = 0; unary_code < 32; ++unary_code) {
                int delta;

                if (unary_code == 0) {
                    delta = 0;
                } else if (unary_code <= 2 * threshold) {
                    // Zigzag pattern: odd → negative, even → positive.
                    //   u=1 → -1, u=2 → +1, u=3 → -2, u=4 → +2, ...
                    if (unary_code & 1) {
                        delta = -((unary_code + 1) / 2);
                    } else {
                        delta = unary_code / 2;
                    }
                } else {
                    // Escape beyond the zigzag range: monotonically increasing.
                    delta = unary_code - threshold;
                }

                int gcli = m_top + delta;
                int index = (gtli << 9) | (previous_gcli << 5) | unary_code;

                if (gcli > max_stored) {
                    lut[index] = kPredictionLutSentinel;
                } else {
                    lut[index] = static_cast<uint8_t>(gcli);
                }
            }
        }
    }

    return lut;
}

const uint8_t* build_prediction_lookup_table() {
    static const std::array<uint8_t, kPredictionLutSize> lut = make_prediction_lookup_table();
    return lut.data();
}

}  // namespace nikon_he
