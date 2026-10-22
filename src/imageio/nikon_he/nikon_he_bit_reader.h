/* -*- C++ -*-
 * File: nikon_he_bit_reader.h
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE bitstream reader interface

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE bitstream reader.
//
// All bit-level reads are MSB-first big-endian. This reader consumes 32-bit
// big-endian words from the byte buffer and shifts them into a 64-bit
// register, MSB-justified. Reads past the end mark the stream as failed.
//


#ifndef LIBRAW_NIKON_HE_BIT_READER_H
#define LIBRAW_NIKON_HE_BIT_READER_H

#include <cstddef>
#include <cstdint>

namespace nikon_he {

class BitReader {
public:
    BitReader() = default;

    // Initialize with a byte buffer. The reader does NOT own the buffer;
    // the caller must ensure it remains valid during the reader's lifetime.
    BitReader(const uint8_t* data, size_t length) { reset(data, length); }

    void reset(const uint8_t* data, size_t length);

    // Read `count` bits MSB-first. count must be in [1, 32].
    // Past-end reads return available bits zero-padded on the LSB side.
    uint32_t read_bits(int count);

    // Read a plain unary code: count leading 1-bits, then consume the
    // terminating 0-bit. Runs above 31 or without a terminator mark failure.
    uint32_t read_unary();

    // Discard any partial byte so the next read is byte-aligned.
    // Equivalent to: bits_left &= ~7; reg <<= (bits_left & 7).
    void align_to_byte();

    // Total bytes consumed from the input buffer (excluding fractional
    // bits still pending in the register).
    size_t bytes_read() const;

    // True when all source bytes have been consumed. The register may
    // still contain zero-padded bits from a partial final word.
    bool exhausted() const { return cursor_ >= end_; }
    bool failed() const { return failed_; }

private:
    void refill();

    const uint8_t* base_ = nullptr;
    const uint8_t* cursor_ = nullptr;
    const uint8_t* end_ = nullptr;
    uint64_t register_ = 0;
    int bits_available_ = 0;
    bool failed_ = false;
};

}  // namespace nikon_he

#endif  // LIBRAW_NIKON_HE_BIT_READER_H
