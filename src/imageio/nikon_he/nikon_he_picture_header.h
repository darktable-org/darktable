/* -*- C++ -*-
 * File: nikon_he_picture_header.h
 *
   Nikon HE / HE* JPEG XS picture-header parser

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon adds a vendor-specific PIH tail and per-component decomposition fields

#ifndef LIBRAW_NIKON_HE_PICTURE_HEADER_H
#define LIBRAW_NIKON_HE_PICTURE_HEADER_H

#include <cstddef>
#include <cstdint>

namespace nikon_he {

constexpr int kNikonHeWgtBands = 25;

struct PictureHeader {
    bool valid;

    size_t precinct_offset;

    uint32_t Lcod;
    uint16_t hdr_width;
    uint16_t hdr_height;
    uint16_t precinct_width;
    uint16_t Hsl;
    uint8_t  comps_num;
    uint8_t  coeff_group_size;
    uint8_t  significance_group_size;
    uint8_t  Bw;
    uint8_t  profile[4];
    uint8_t  components[4][3];

    int      nbands;
    uint8_t  gain[kNikonHeWgtBands];
    uint8_t  priority[kNikonHeWgtBands];

    // Nikon's 13-byte extension past the standard 24-byte PIH payload, which holds
    // the transfer curve's parameters.
    bool     has_vendor_tail;
    uint8_t  vendor_tail[13];
};

bool parse_picture_header(const uint8_t* strip, size_t strip_size,
                          PictureHeader& out);

// The decoder implements one fixed 4-component, 25-WGT-band Nikon profile.
bool is_supported_picture_header(const PictureHeader& ph, size_t strip_size);

// evaluate the vendor transfer with exact integer rounding, returning 14-bit raw
uint16_t transfer_value(const PictureHeader& ph, int32_t index);

// T[p,b] = clamp(Qp - G[b] - (P[b] < Rp), 0, 15)
inline int gtli_from_weights(const PictureHeader& ph, int band, int Qp, int Rp) {
    if (band < 0 || band >= ph.nbands)
        return 0;
    int v = Qp - (int)ph.gain[band] - ((int)ph.priority[band] < Rp ? 1 : 0);
    if (v < 0) v = 0;
    if (v > 15) v = 15;
    return v;
}

}  // namespace nikon_he

#endif  // LIBRAW_NIKON_HE_PICTURE_HEADER_H
