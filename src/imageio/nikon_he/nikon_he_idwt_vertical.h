/* -*- C++ -*-
 * File: nikon_he_idwt_vertical.h
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE vertical inverse DWT interface

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE vertical 5/3 IDWT state machine.
//
// After horizontal IDWT, each precinct produces 4 line-blocks of
// horizontally-reconstructed coefficients. The vertical IDWT combines
// these across precincts using a per-LB-component state machine.
//
// The state machine (per LB, per pass):
//   Path A (tile 0):  2 → 5 → 7 → 8 → (7→8)* → 9 → 11
//   Path B (tile >0): 0 → 1 → 4 → 7 → 8 → (7→8)* → 9 → 11
//
// Only states ≥ 7 produce x1 writes to the output buffer. The x1 write
// offset advances by 4 per ver_lift loop when pre-tick state > 5.
//


#ifndef LIBRAW_NIKON_HE_IDWT_VERTICAL_H
#define LIBRAW_NIKON_HE_IDWT_VERTICAL_H

#include "nikon_he_subband_config.h"
#include <cstdint>

namespace nikon_he {

// Number of LB components for vertical IDWT (LB 0, 1, 3 = lift; LB 2 = memcpy).
constexpr int kVerComponents = 4;

// State machine constants.
namespace VerLiftState {
    enum Value {
        // Path A initial states
        kInit2  = 2,   // tile 0 entry
        // Path B initial states
        kInit0  = 0,   // tile >0 entry
        kInit1  = 1,
        kInit4  = 4,
        // Common states
        kState5 = 5,
        kState7 = 7,   // first x1 write (state=7 writes x3 directly)
        kState8 = 8,   // main lift body (5/3 vertical lift, repeat)
        kState9 = 9,   // partial-tile tail flush
        kState11 = 11, // final flush
    };
}

// Per-LB carry buffers for the vertical IDWT.
// x2_carry and x3_carry hold intermediate lift state across precincts.
// State tracks the phase counter (2,5,7,8,9,11 for path A).
struct VerLiftStatePerLB {
    int32_t* x2_carry;   // size: lift_LB * 4 (or ng_LL * 4 for memcpy LB)
    int32_t* x3_carry;   // size: lift_LB * 4
    int state;           // phase counter
    bool failed;         // input ended in a state that needs more rows
};

// Run one vertical-lift pass for one LB component.
//
// Parameters:
//   x0       — horizontal lift output for this LB (or nullptr for tail flush)
//   x1_out   — output buffer (writes to x1_out[x1_write_offset + ...])
//   n        — number of samples (lift_LB*4 for lift LBs, 0 for memcpy LBs)
//   st       — per-LB state (updated in-place)
//
// The output x1 is written with stride = passA_stride (from layout_info).
// x1_write_offset is the per-loop write offset (0, 4, 8, ...).
//
void ver_lift_lb_step(
    const int32_t* x0,
    int32_t* x1_out,
    int n,
    VerLiftStatePerLB& st);

// Initialize vertical IDWT state for path A (tile 0).
// All 4 LBs start at state 2 — including the memcpy LB (LB 2). When n=0
// the state still advances per the transition table; the memcpy LB does
// no work but ticks 2→5→7→8 in lockstep with the other LBs.
inline void ver_lift_init_path_a(VerLiftStatePerLB st[4]) {
    for (int k = 0; k < 4; k++) {
        st[k].state = VerLiftState::kInit2;
    }
}

// Initialize vertical IDWT state for path B (tile > 0).
inline void ver_lift_init_path_b(VerLiftStatePerLB st[4]) {
    for (int k = 0; k < 4; k++) {
        st[k].state = VerLiftState::kInit0;
    }
}

// Check if a ver_lift loop should advance the x1 write offset.
// Returns true when the pre-tick state of LB 0 is > 5.
inline bool ver_lift_should_advance_offset(const VerLiftStatePerLB st[4]) {
    return st[0].state > 5;
}

}  // namespace nikon_he

#endif  // LIBRAW_NIKON_HE_IDWT_VERTICAL_H
