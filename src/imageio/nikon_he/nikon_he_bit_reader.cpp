/* -*- C++ -*-
 * File: nikon_he_bit_reader.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE bitstream reader implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE bitstream reader — implementation.
// See nikon_he_bit_reader.h for the public API.
//
// Register convention: pending bits are left-justified (MSB-aligned) within
// a 64-bit register. The next bit to be consumed is at bit 63. Shifting the
// register left by N consumes N bits. A refill adds 32 fresh bits below the
// existing pending bits, keeping the MSB-justified invariant.

#include "nikon_he_bit_reader.h"

namespace nikon_he {

void BitReader::reset(const uint8_t* data, size_t length) {
    base_ = data;
    cursor_ = data;
    end_ = data + length;
    register_ = 0;
    bits_available_ = 0;
    failed_ = false;
}

void BitReader::refill() {
    // Read one 32-bit big-endian word. If fewer than 4 bytes remain,
    // read what we can and zero-pad the rest.
    uint32_t word = 0;
    size_t remaining = static_cast<size_t>(end_ - cursor_);

    if (remaining >= 4) {
        word = (static_cast<uint32_t>(cursor_[0]) << 24)
             | (static_cast<uint32_t>(cursor_[1]) << 16)
             | (static_cast<uint32_t>(cursor_[2]) << 8)
             |  static_cast<uint32_t>(cursor_[3]);
        cursor_ += 4;
    } else if (remaining > 0) {
        for (size_t i = 0; i < remaining; ++i) {
            word = (word << 8) | cursor_[i];
        }
        word <<= (4 - remaining) * 8;
        cursor_ = end_;
    } else {
        return;  // source exhausted, nothing to add
    }

    // Slide the new word into the register below existing bits.
    // register_ holds bits_available_ valid bits at the top.
    // New word goes in just below them.
    int shift = 32 - bits_available_;
    register_ |= static_cast<uint64_t>(word) << shift;
    bits_available_ += (remaining >= 4) ? 32 : static_cast<int>(remaining * 8);
}

uint32_t BitReader::read_bits(int count) {
    while (bits_available_ < count) {
        if (cursor_ >= end_) {
            failed_ = true;
            // Source exhausted: return whatever bits remain, zero-padded.
            uint32_t result = static_cast<uint32_t>(register_ >> (64 - count));
            register_ = 0;
            bits_available_ = 0;
            return result;
        }
        refill();
    }
    uint32_t result = static_cast<uint32_t>(register_ >> (64 - count));
    register_ <<= count;
    bits_available_ -= count;
    return result;
}

uint32_t BitReader::read_unary() {
    uint32_t count = 0;
    for (;;) {
        if (bits_available_ <= 0) {
            if (cursor_ >= end_) {
                failed_ = true;
                return count;  // unterminated run at EOF
            }
            refill();
        }
        // Check the MSB of the register.
        bool is_one = (register_ >> 63) & 1ULL;
        register_ <<= 1;
        bits_available_--;
        if (!is_one) {
            return count;
        }
        count++;
        if (count > 31) {
            failed_ = true;
            return count;
        }
    }
}

void BitReader::align_to_byte() {
    int fractional = bits_available_ & 7;
    if (fractional == 0) return;
    register_ <<= fractional;
    bits_available_ -= fractional;
}

size_t BitReader::bytes_read() const {
    size_t consumed = static_cast<size_t>(cursor_ - base_);
    size_t held_in_register = static_cast<size_t>(bits_available_ >> 3);
    return consumed - held_in_register;
}

}  // namespace nikon_he
