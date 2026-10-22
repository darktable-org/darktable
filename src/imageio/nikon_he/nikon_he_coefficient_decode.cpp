/* -*- C++ -*-
 * File: nikon_he_coefficient_decode.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE coefficient magnitude/sign decode implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE coefficient decode — implementation.
//
// See nikon_he_coefficient_decode.h for the public API.

#include "nikon_he_coefficient_decode.h"
#include <cstring>

namespace nikon_he {

void unpack_coefficient_magnitudes(
    BitReader& data_reader,
    const uint8_t* gcli_values,
    int gtli,
    int num_groups,
    int32_t* coefficients_out) {

    // Zero the full output array.
    int total_coefficients = num_groups * 4;
    std::memset(coefficients_out, 0, static_cast<size_t>(total_coefficients) * sizeof(int32_t));

    for (int g = 0; g < num_groups; ++g) {
        int num_bitplanes = static_cast<int>(gcli_values[g]) - gtli;
        if (num_bitplanes <= 0) {
            continue;  // All four coefficients in this group are zero.
        }

        // Assemble magnitudes bit-plane by bit-plane, MSB first.
        // Each nibble encodes one bit-plane: bits 3,2,1,0 → coefficients 0,1,2,3.
        // GCLIs are bounded to 15 before this call, including the implicit low bits
        uint32_t m[4] = {0, 0, 0, 0};

        for (int bp = 0; bp < num_bitplanes; ++bp) {
            uint32_t nibble = data_reader.read_bits(4);

            m[0] = (m[0] << 1) | ((nibble >> 3) & 1);
            m[1] = (m[1] << 1) | ((nibble >> 2) & 1);
            m[2] = (m[2] << 1) | ((nibble >> 1) & 1);
            m[3] = (m[3] << 1) | ( nibble       & 1);
        }

        // Shift in the implicit-zero low bits (truncated at encode time).
        for (int i = 0; i < 4; ++i)
            coefficients_out[g * 4 + i] = static_cast<int32_t>(m[i] << gtli);
    }
}

void apply_sign_bits(
    BitReader& sign_reader,
    int32_t* coefficients,
    int coefficient_count) {

    for (int i = 0; i < coefficient_count; ++i) {
        if (coefficients[i] == 0) {
            continue;  // Zero coefficients consume no sign bit.
        }
        if (sign_reader.read_bits(1) == 1) {
            coefficients[i] = -coefficients[i];
        }
    }
}

}  // namespace nikon_he
