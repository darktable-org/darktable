/* -*- C++ -*-
 * File: nikon_he_coefficient_decode.h
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE coefficient magnitude/sign decode interface

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE coefficient decode.
//
// After GCLI decode, the data substream carries coefficient magnitudes
// as interleaved bit-planes (one nibble = one bit-plane for all 4
// coefficients in a group), and the sign substream carries one bit per
// non-zero coefficient.
//


#ifndef LIBRAW_NIKON_HE_COEFFICIENT_DECODE_H
#define LIBRAW_NIKON_HE_COEFFICIENT_DECODE_H

#include "nikon_he_bit_reader.h"
#include <cstddef>
#include <cstdint>

namespace nikon_he {

// Unpack coefficient magnitudes from the data substream.
//
// For each group g with num_bitplanes = gcli_values[g] - gtli:
//   - If num_bitplanes ≤ 0, all 4 coefficients are zero.
//   - Otherwise, read num_bitplanes nibbles (4 bits each) MSB-first.
//     Each nibble gives one bit for each of the 4 coefficients:
//       bit 3 → coefficient 0
//       bit 2 → coefficient 1
//       bit 1 → coefficient 2
//       bit 0 → coefficient 3
//   - After assembly, shift left by gtli to restore the implicit low bits.
//
// Parameters:
//   data_reader      — bit reader positioned at the data substream
//   gcli_values      — per-group GCLI values (length ≥ num_groups)
//   gtli             — this sub-band's GTLI threshold
//   num_groups       — number of coefficient groups (each = 4 coefficients)
//   coefficients_out — output buffer of at least num_groups * 4 int32_t
//
void unpack_coefficient_magnitudes(
    BitReader& data_reader,
    const uint8_t* gcli_values,
    int gtli,
    int num_groups,
    int32_t* coefficients_out);

// Apply sign bits from the sign substream.
//
// For each non-zero coefficient in `coefficients` (length = coefficient_count),
// read one bit from sign_reader: 1 means negate, 0 means keep positive.
// Zero coefficients consume no sign bit.
//
void apply_sign_bits(
    BitReader& sign_reader,
    int32_t* coefficients,
    int coefficient_count);

}  // namespace nikon_he

#endif  // LIBRAW_NIKON_HE_COEFFICIENT_DECODE_H
