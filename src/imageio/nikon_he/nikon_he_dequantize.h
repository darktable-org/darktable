/* -*- C++ -*-
 * File: nikon_he_dequantize.h
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE coefficient dequantization interface

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE dequantization.
//
// The encoder truncates the bottom `gtli` bits of every coefficient.
// The decoder reconstructs those bits using a deadzone-midpoint formula,
// scaling each coefficient by a 16-entry constant table indexed by the
// number of bit-planes actually encoded.
//
// The image decoder uses the same uniform dequantization for every band.
//
// Nikon HE constants:
//   w4_shift = 4   (image-wide tone shift)
// The "encoded_gtli" and "target_gtli" parameters of the formula are
// BOTH the sub-band's own gtli (from picture weights and precinct Bp/Br).
//


#ifndef LIBRAW_NIKON_HE_DEQUANTIZE_H
#define LIBRAW_NIKON_HE_DEQUANTIZE_H

#include <cstdint>

namespace nikon_he {

// Deadzone-midpoint scaling constants. Indexed by (bit_plane_count - 1),
// i.e. bc[w2 - 1] where w2 = gcli - encoded_gtli.
// bc[15] = 0 is a sentinel (16 bit-planes → unreachable in this format).
constexpr int32_t kMidpointScaleTable[16] = {
    87381, 74898, 69905, 67650, 66576, 66052, 65793, 65664,
    65600, 65568, 65552, 65544, 65540, 65538, 65537, 0,
};

// Nikon HE image-wide tone shift.
constexpr int kW4Shift = 4;

// dequantize one coefficient in any band
//   coefficient — signed raw coefficient from entropy decode
//   gcli        — this group's GCLI value
//   gtli        — this sub-band's GTLI (from picture weights and precinct Bp/Br)
int32_t dequantize_coefficient(int32_t coefficient,
                               int gcli,
                               int gtli);

// Batch dequantize an array of coefficients for one sub-band.
void dequantize_coefficient_array(
    int32_t* coefficients,
    int coefficient_count,
    const uint8_t* gcli_values,
    int num_groups,
    int target_gtli);

}  // namespace nikon_he

#endif  // LIBRAW_NIKON_HE_DEQUANTIZE_H
