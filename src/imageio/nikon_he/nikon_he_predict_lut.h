/* -*- C++ -*-
 * File: nikon_he_predict_lut.h
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE prediction lookup table interface

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE GCLI prediction lookup table.
//
// The prediction function `predict(gtli, previous_gcli, unary_code)` is
// a deterministic mapping used during GCLI (Greatest Coded Line Index)
// decode for modes 0x71 (zero-prediction) and 0x73 (vertical prediction).
//
// The full table is 8192 entries (16 × 16 × 32) indexed as:
//   index = (gtli << 9) | (previous_gcli << 5) | unary_code
//
// The table is computed once at decoder init and cached.
//


#ifndef LIBRAW_NIKON_HE_PREDICT_LUT_H
#define LIBRAW_NIKON_HE_PREDICT_LUT_H

#include <cstddef>
#include <cstdint>

namespace nikon_he {

// 8192 = 16 gtli levels × 16 previous_gcli levels × 32 unary_code values.
constexpr size_t kPredictionLutSize = 8192;

// Sentinel value for unreachable table entries.
constexpr uint8_t kPredictionLutSentinel = 0xFF;

// Build the prediction lookup table. The returned pointer is valid for the
// lifetime of the program (thread-safe static initialization).
const uint8_t* build_prediction_lookup_table();

// Convenience inline lookup. Returns the predicted GCLI value, or the
// sentinel (0xFF) for unreachable combinations.
inline uint8_t lookup_prediction(const uint8_t* lut,
                                  int gtli,
                                  int previous_gcli,
                                  int unary_code) {
    // The table covers GTLIs and previous GCLIs of 0 to 15 and unary codes of 0 to 31.
    if (gtli < 0 || gtli > 15 || previous_gcli < 0 || previous_gcli > 15 ||
        unary_code < 0 || unary_code > 31)
        return kPredictionLutSentinel;
    int index = (gtli << 9) | (previous_gcli << 5) | unary_code;
    return lut[index];
}

}  // namespace nikon_he

#endif  // LIBRAW_NIKON_HE_PREDICT_LUT_H
