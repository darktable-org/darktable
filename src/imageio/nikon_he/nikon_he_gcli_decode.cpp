/* -*- C++ -*-
 * File: nikon_he_gcli_decode.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE GCLI (group code-length info) decode implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE GCLI decode — implementation.
//
// Decodes per-group GCLI (Greatest Coded Line Index) values from the
// significance and GCLI substreams. The sig substream gives one bit per
// block of 8 groups; the GCLI substream carries per-group unary deltas.
//
// Mode 0x71 (zero prediction):
//   sig=1 → all 8 GCLIs = gtli  (predict(gtli, prev=0, u=0) = gtli)
//   sig=0 → gcli[i] = gtli + unary_code  (predict(gtli, 0, u) = gtli + u)
//
// Mode 0x73 (vertical prediction):
//   sig=1 → gcli[i] = predict(gtli, prev_band[i], u=0) = max(prev_band[i], gtli)
//   sig=0 → gcli[i] = predict(gtli, prev_band[i], u)
//


#include "nikon_he_gcli_decode.h"
#include <algorithm>

namespace nikon_he {

bool decode_gcli_values(
    BitReader& sig_reader,
    BitReader& gcli_reader,
    int mode,
    int num_groups,
    int gtli,
    const uint8_t* predict_lut,
    const uint8_t* previous_gcli,
    uint8_t* gcli_output) {

    if ((mode != 0x71 && mode != 0x73) || gtli < 0 || gtli > kMaxGcli)
        return false;

    // Number of significance blocks (one sig bit per 8 GCLI groups).
    int num_sig_blocks = (num_groups + 7) / 8;

    for (int block = 0; block < num_sig_blocks; ++block) {
        int base = block * 8;
        int block_size = std::min(8, num_groups - base);

        uint32_t sig_bit = sig_reader.read_bits(1);

        if (sig_bit == 1) {
            // "All-baseline" block: every GCLI equals the predicted value
            // with unary_code=0 (no per-element refinement from GCLI substream).
            if (mode == 0x71) {
                // Zero-prediction: predict(gtli, 0, 0) = gtli.
                for (int i = 0; i < block_size; ++i) {
                    gcli_output[base + i] = static_cast<uint8_t>(gtli);
                }
            } else {
                // Vertical prediction: each element's baseline depends on
                // the corresponding previous-band GCLI.
                // predict(gtli, prev, 0) = max(prev, gtli).
                for (int i = 0; i < block_size; ++i) {
                    int prev = previous_gcli[base + i];
                    if (prev > kMaxGcli) return false;
                    gcli_output[base + i] = static_cast<uint8_t>(std::max(prev, gtli));
                }
            }
        } else {
            // Per-element decode: each GCLI has its own unary delta in the
            // GCLI substream.
            if (mode == 0x71) {
                // Zero-prediction: gcli = gtli + unary_code.
                for (int i = 0; i < block_size; ++i) {
                    uint32_t u = gcli_reader.read_unary();
                    if (u > static_cast<uint32_t>(kMaxGcli - gtli)) return false;
                    gcli_output[base + i] = static_cast<uint8_t>(gtli + u);
                }
            } else {
                // Vertical prediction via lookup table.
                for (int i = 0; i < block_size; ++i) {
                    uint32_t u = gcli_reader.read_unary();
                    int prev = previous_gcli[base + i];
                    uint8_t gcli = lookup_prediction(predict_lut, gtli, prev, u > 31 ? 32 : static_cast<int>(u));
                    if (gcli == kPredictionLutSentinel || gcli > kMaxGcli) return false;
                    gcli_output[base + i] = gcli;
                }
            }
        }
    }
    return true;
}

}  // namespace nikon_he
