/* -*- C++ -*-
 * File: nikon_he_picture_header.cpp
 *
   Nikon HE / HE* JPEG XS picture-header parser

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

#include "nikon_he_picture_header.h"

#include <cstring>
#include <algorithm>

namespace nikon_he {

namespace {

const uint16_t kSOC = 0xff10;
const uint16_t kEOC = 0xff11;
const uint16_t kPIH = 0xff12;
const uint16_t kCDT = 0xff13;
const uint16_t kWGT = 0xff14;
const uint16_t kSLH = 0xff20;
const uint16_t kCAP = 0xff50;

inline uint16_t be16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

}  // namespace

bool parse_picture_header(const uint8_t* strip, size_t strip_size,
                          PictureHeader& out) {
    std::memset(&out, 0, sizeof(out));
    if (!strip || strip_size < 8)
        return false;
    if (be16(strip) != kSOC)
        return false;

    size_t i = 2;
    bool saw_cap = false, saw_pih = false, saw_cdt = false, saw_wgt = false;

    while (i + 2 <= strip_size) {
        const uint16_t marker = be16(strip + i);
        if ((marker & 0xff00) != 0xff00)
            return false;

        if (marker == kEOC)
            return false;
        if (i + 4 > strip_size)
            return false;
        const uint16_t lseg = be16(strip + i + 2);
        if (lseg < 2 || i + 2 + (size_t)lseg > strip_size)
            return false;
        const uint8_t* body = strip + i + 4;
        const size_t body_len = (size_t)lseg - 2;

        switch (marker) {
        case kCAP:
            if (saw_cap || saw_pih || body_len != 32 ||
                std::memcmp(body, "CONTACT_INTOPIX_", 16) != 0)
                return false;
            saw_cap = true;
            break;
        case kPIH: {
            if (!saw_cap || saw_pih || body_len != 37 || be32(body + 4) != 0)
                return false;
            out.Lcod                    = be32(body + 0);
            out.hdr_width               = be16(body + 8);
            out.hdr_height              = be16(body + 10);
            out.precinct_width          = be16(body + 12);
            out.Hsl                     = be16(body + 14);
            out.comps_num               = body[16];
            out.coeff_group_size        = body[17];
            out.significance_group_size = body[18];
            out.Bw                      = body[19];
            std::memcpy(out.profile, body + 20, sizeof(out.profile));
            if (body_len >= 24 + sizeof(out.vendor_tail)) {
                std::memcpy(out.vendor_tail, body + 24, sizeof(out.vendor_tail));
                out.has_vendor_tail = true;
            }
            saw_pih = true;
            break;
        }
        case kCDT:
            if (!saw_pih || saw_cdt || body_len != sizeof(out.components))
                return false;
            std::memcpy(out.components, body, sizeof(out.components));
            saw_cdt = true;
            break;
        case kWGT: {
            if (!saw_cdt || saw_wgt || (body_len & 1u))
                return false;
            int n = (int)(body_len / 2);
            if (n != kNikonHeWgtBands)
                return false;
            for (int b = 0; b < n; b++) {
                out.gain[b]     = body[2 * b];
                out.priority[b] = body[2 * b + 1];
            }
            out.nbands = n;
            saw_wgt = true;
            break;
        }
        case kSLH:
            if (!saw_wgt || body_len != 2 || be16(body) != 0)
                return false;
            break;
        default:
            return false;
        }

        i += 2 + (size_t)lseg;

        if (marker == kSLH) {
            out.precinct_offset = i;
            out.valid = saw_cap && saw_pih && saw_cdt && saw_wgt;
            return out.valid;
        }
    }
    return false;
}

bool is_supported_picture_header(const PictureHeader& ph, size_t strip_size) {
    if (!ph.valid || ph.nbands != kNikonHeWgtBands) return false;
    // Fq=4, Br=4; progression=1, color transform=0; NLx=5, NLy=1;
    // uniform quantization, separate signs, significance run mode, short packets
    constexpr uint8_t profile[] = {0x44, 0x10, 0x51, 0x14};
    if (std::memcmp(ph.profile, profile, sizeof(profile)) != 0)
        return false;
    for (int component = 0; component < 4; ++component) {
        // 14-bit planes at 1:1 sampling, with the third plane untransformed
        const auto* c = ph.components[component];
        if (c[0] != 14 || c[1] != 0x11 || c[2] != (component == 2 ? 0 : 0x51))
            return false;
    }
    uint32_t priorities = 0;
    for (int band = 0; band < ph.nbands; ++band) {
        if (ph.gain[band] > 15 || ph.priority[band] >= kNikonHeWgtBands)
            return false;
        const uint32_t bit = 1u << ph.priority[band];
        if (priorities & bit) return false;
        priorities |= bit;
    }
    // only A, B and C below have known variable semantics in the vendor tail
    constexpr uint8_t tail[] = {0x50, 0x88, 0x70, 0x80, 0, 0x15, 0x23, 0xd0, 0, 0, 0, 0, 7};
    constexpr uint8_t mask[] = {0xff, 0xff, 0xff, 0xf0, 0, 0xff, 0xff, 0xfc, 0, 0, 0, 0, 0xff};
    for (size_t i = 0; i < sizeof(tail); ++i)
        if ((ph.vendor_tail[i] & mask[i]) != tail[i]) return false;
    return ph.valid && ph.Lcod == strip_size &&
           ph.hdr_width >= 64 && ph.hdr_width <= 6064 && ph.hdr_width % 8 == 0 &&
           ph.hdr_height >= 64 && ph.hdr_height <= 4040 && ph.hdr_height % 4 == 0 &&
           ph.precinct_width == 0 && ph.Hsl == 16 &&
           ph.comps_num == 4 && ph.coeff_group_size == 4 &&
           ph.significance_group_size == 8 && ph.Bw == 18 &&
           ph.has_vendor_tail;
}

uint16_t transfer_value(const PictureHeader& ph, int32_t index) {
    const uint8_t* tail = ph.vendor_tail;
    const int64_t b = ((tail[3] & 15) << 8) | tail[4];
    const int64_t a = ((tail[7] & 3) << 16) | (tail[8] << 8) | tail[9];
    const int64_t c = (tail[10] << 8) | tail[11];
    const int64_t delta = std::clamp(index, 0, 65535) - c;
    const int64_t scaled = (delta * a + 32768) >> 16;
    const int64_t squared = delta < 0 ? -delta * delta : scaled * scaled;
    return static_cast<uint16_t>(std::clamp(b + ((squared + 163840) >> 18),
                                          int64_t(0), int64_t(16383)));
}

}  // namespace nikon_he
