/* -*- C++ -*-
 * File: nikon_he_precinct_header.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE precinct header parser implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE precinct header parser — implementation.
//
// See nikon_he_precinct_header.h for the public API.

#include "nikon_he_precinct_header.h"
#include <cstring>

namespace nikon_he {

// 12-byte prefix and eight interleaved seven-byte packet headers
static constexpr size_t kMinHeaderPrefix = 68;

// Read a big-endian uint32_t from an unaligned byte pointer.
static inline uint32_t read_be(const uint8_t* p, int bytes) {
    uint32_t v = 0;
    for (int i = 0; i < bytes; i++) {
        v = (v << 8) | p[i];
    }
    return v;
}

bool parse_precinct_header(const uint8_t* data,
                           size_t data_size,
                           const int sig_bytes[kLineBlocksPerPrecinct],
                           PrecinctSizes& out) {
    if (data_size < kMinHeaderPrefix) {
        return false;
    }

    std::memset(&out, 0, sizeof(out));

    // Bytes 0-2: total_size_minus_12 (24-bit big-endian)
    out.total_size = read_be(data, 3);
    if (out.total_size != data_size - 12) return false;

    // Byte 3: Bp
    out.Bp = data[3];

    // Byte 4: Br
    out.Br = data[4];

    // bytes 5-11: 25 two-bit prediction modes, followed by six padding bits
    for (int i = 0; i < 7; i++) {
        uint8_t b = data[5 + i];
        for (int j = 0; j < 4; j++) {
            int idx = i * 4 + j;
            if (idx >= kDpbCount) break;
            out.Dpb[idx] = (b >> (6 - 2 * j)) & 0x03;
        }
    }

    // 8 LB mini-headers + substreams, INTERLEAVED.
    // Each LB block is:
    //   7 bytes mini-header (1 + 20 + 20 + 15 = 56 bits)
    //   f20 bytes sig substream
    //   f24 bytes gcli substream
    //   f28 bytes data substream
    //   f2c bytes sign substream
    // The next mini-header starts immediately after the previous LB's
    // sign substream.
    size_t cursor = 12;   // after prefix (3+1+1+7 = 12 bytes)
    for (int lb = 0; lb < kLineBlocksPerPrecinct; lb++) {
        if (cursor + 7 > data_size) return false;

        const uint8_t* base = data + cursor;
        uint64_t val = 0;
        for (int b = 0; b < 7; b++) {
            val = (val << 8) | base[b];
        }

        out.lb_f20_sign  [lb] = static_cast<uint8_t> ((val >> 55) & 1);
        out.lb_data_bytes[lb] = static_cast<uint32_t>((val >> 35) & 0xFFFFF);
        out.lb_gcli_bytes[lb] = static_cast<uint32_t>((val >> 15) & 0xFFFFF);
        out.lb_sign_bytes[lb] = static_cast<uint32_t>( val        & 0x7FFF);
        // A set flag selects raw GCLI packets, which carry no significance bytes.
        out.lb_sig_bytes [lb] = out.lb_f20_sign[lb] ? 0 : static_cast<uint32_t>(sig_bytes[lb]);

        cursor += 7;                                 // past mini-header
        out.lb_sig_offset[lb] = static_cast<uint32_t>(cursor);  // sig starts here

        // Skip this LB's 4 substreams before reading next mini-header.
        size_t payload = static_cast<size_t>(out.lb_sig_bytes[lb])
                       + out.lb_gcli_bytes[lb]
                       + out.lb_data_bytes[lb]
                       + out.lb_sign_bytes[lb];
        if (cursor + payload > data_size) return false;
        cursor += payload;
    }

    return true;
}

}  // namespace nikon_he
