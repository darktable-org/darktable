/* -*- C++ -*-
 * File: nikon_he_precinct_header.h
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE precinct header parser interface

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE precinct header parser.
//
// Each precinct in a Z9 HE image starts with a compact binary header
// that describes the byte layout of the subsequent entropy-coded data.
// The header is read MSB-first (big-endian) from the raw precinct byte
// stream.
//


#ifndef LIBRAW_NIKON_HE_PRECINCT_HEADER_H
#define LIBRAW_NIKON_HE_PRECINCT_HEADER_H

#include <cstddef>
#include <cstdint>


namespace nikon_he {

// Number of line-blocks per precinct pass.
constexpr int kLineBlocksPerPrecinct = 8;

// one two-bit prediction mode per WGT band, padded to seven bytes
constexpr int kDpbCount = 25;

// Result of parsing a precinct header.
struct PrecinctSizes {
    // Total size of the precinct data in bytes (as stored in the header).
    uint32_t total_size;

    // Precinct bit-depth parameters.
    uint8_t Bp;
    uint8_t Br;

    // prediction modes in WGT order, shared by both LL lines
    uint8_t Dpb[kDpbCount];

    // Per-LB substream byte counts.
    uint32_t lb_sig_bytes[kLineBlocksPerPrecinct];   // LayoutInfo::sig_bytes, or 0 for raw GCLI
    uint32_t lb_data_bytes[kLineBlocksPerPrecinct];  // f28: data substream
    uint32_t lb_gcli_bytes[kLineBlocksPerPrecinct];  // f24: GCLI+significance substreams
    uint32_t lb_sign_bytes[kLineBlocksPerPrecinct];  // f2c: sign substream
    uint8_t  lb_f20_sign[kLineBlocksPerPrecinct];    // selects raw GCLI packets

    // Per-LB offset from the START of precinct_data to the LB's SIG substream
    // (= first byte after its 7-byte mini-header). The data/gcli/sign offsets
    // follow contiguously after.
    uint32_t lb_sig_offset[kLineBlocksPerPrecinct];
};

// Parse a precinct header from raw bytes (MSB-first big-endian).
//
// Parameters:
//   data        — pointer to the start of the precinct data
//   data_size   — size of the buffer (must be ≥ 68 bytes)
//   sig_bytes   — each LB's significance bytes (LayoutInfo::sig_bytes)
//   out         — output PrecinctSizes structure (always written on success)
//
// Returns true on success, false if the buffer is too short.
//
bool parse_precinct_header(const uint8_t* data,
                           size_t data_size,
                           const int sig_bytes[kLineBlocksPerPrecinct],
                           PrecinctSizes& out);

}  // namespace nikon_he

#endif  // LIBRAW_NIKON_HE_PRECINCT_HEADER_H
