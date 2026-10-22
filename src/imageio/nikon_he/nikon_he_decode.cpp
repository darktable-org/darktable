/* -*- C++ -*-
 * File: nikon_he_decode.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE image-level decoder implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

#include "nikon_he_decode.h"
#include "nikon_he_picture_header.h"
#include "nikon_he_predecessor.h"
#include "nikon_he_precinct_decode.h"
#include "nikon_he_predict_lut.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace nikon_he {

HeDecodeResult decode_nikon_he_image(const uint8_t* strip, size_t strip_size,
                                     int image_width, int image_height,
                                     uint16_t* out_bayer, int output_stride) {
    HeDecodeResult result = {false, 0, 0};
    PictureHeader header;
    if (!parse_picture_header(strip, strip_size, header) ||
        !is_supported_picture_header(header, strip_size) ||
        image_width != header.hdr_width || image_height != header.hdr_height ||
        !out_bayer || output_stride < image_width)
        return result;

    SubbandConfig config[26];
    LayoutInfo layout;
    compute_subband_layout(image_width / 2, config, layout);
    const int stripe_ints = compute_buf_stripe_ints(&layout);
    const int kband = compute_kband(&layout);
    const int n_tiles = (image_height + 63) / 64;
    const int n_precincts = image_height / 4;
    const uint8_t* predict_lut = build_prediction_lookup_table();

    // each precinct codes four Bayer rows, with a slice header every 16 precincts
    std::vector<const uint8_t*> precincts(n_precincts);
    std::vector<size_t> sizes(n_precincts);
    size_t cursor = header.precinct_offset;
    for (int p = 0; p < n_precincts; ++p) {
        if (strip_size - cursor < 12) return result;
        const uint8_t* data = strip + cursor;
        const size_t size = (static_cast<size_t>(data[0]) << 16) |
                            (static_cast<size_t>(data[1]) << 8) | data[2];
        if (size < 56 || size > strip_size - cursor - 12) return result;
        precincts[p] = data;
        sizes[p] = size + 12;
        cursor += size + 12;
        if ((p + 1) % 16 == 0 && p + 1 < n_precincts) {
            if (strip_size - cursor < 6) return result;
            const uint8_t* slice = strip + cursor;
            if (slice[0] != 0xff || slice[1] != 0x20 ||
                slice[2] != 0 || slice[3] != 4 ||
                ((slice[4] << 8) | slice[5]) != (p + 1) / 16)
                return result;
            cursor += 6;
        }
    }
    if (strip_size - cursor != 2 || strip[cursor] != 0xff || strip[cursor + 1] != 0x11)
        return result;

    std::vector<int32_t> lut(kLutSize);
    for (int i = 0; i < kLutSize; ++i)
        lut[i] = 4 * transfer_value(header, i);

    // three contiguous tiles provide the preceding/following rows for each lift
    // decode T, reconstruct stage 1 of T-1, then emit the Bayer rows of T-2
    const size_t tile_ints = static_cast<size_t>(kStripesPerTile) * stripe_ints;
    std::vector<int32_t> coefficients(3 * tile_ints);
    std::vector<int32_t> reconstructed(3 * tile_ints);
    std::vector<int32_t> overflow(2 * stripe_ints);
    for (int next = 0; next < n_tiles + 2; ++next) {
        std::memmove(coefficients.data(), coefficients.data() + tile_ints,
                     2 * tile_ints * sizeof(int32_t));
        std::memmove(reconstructed.data(), reconstructed.data() + tile_ints,
                     2 * tile_ints * sizeof(int32_t));
        std::fill(coefficients.begin() + 2 * tile_ints, coefficients.end(), 0);
        std::fill(reconstructed.begin() + 2 * tile_ints, reconstructed.end(), 0);

        if (next < n_tiles) {
            PrecinctPredecessorState prediction;
            prediction.init(config);
            const int first = next * 16;
            const int count = std::min(kPrecinctsPerTile, n_precincts - first);
            const auto tile = decode_tile(precincts.data() + first, sizes.data() + first,
                                          header, config, prediction, predict_lut,
                                          coefficients.data() + 2 * tile_ints,
                                          overflow.data(), next == 0, count);
            if (!tile.success) return result;
            ++result.tiles_decoded;
            result.total_precincts += tile.precincts_decoded;
        }

        const int stage1 = next - 1;
        if (stage1 >= 0 && stage1 < n_tiles) {
            const int rows = std::min(32, image_height / 2 - stage1 * 32);
            const int32_t* tile = coefficients.data() + tile_ints;
            int32_t* output = reconstructed.data() + 2 * tile_ints;
            step1_merge_4_to_2(tile + kband, tile, tile + 3 * kband, tile + 2 * kband,
                               rows, image_width / 2, stripe_ints, stripe_ints,
                               output, output + kband, stripe_ints,
                               stage1 == 0, stage1 == n_tiles - 1);
        }

        const int stage2 = next - 2;
        if (stage2 >= 0) {
            const int rows = std::min(32, image_height / 2 - stage2 * 32);
            const int32_t* tile = coefficients.data();
            const int32_t* stage1_tile = reconstructed.data() + tile_ints;
            step2_bayer_rows(tile + kband, stage1_tile, stage1_tile + kband,
                             tile + 2 * kband, rows, image_width / 2,
                             stripe_ints, stripe_ints, lut.data(), out_bayer,
                             output_stride, stage2 * 64,
                             stage2 == 0, stage2 == n_tiles - 1);
        }
    }
    result.success = true;
    return result;
}

}  // namespace nikon_he
