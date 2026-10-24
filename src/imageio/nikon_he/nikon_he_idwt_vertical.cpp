/* -*- C++ -*-
 * File: nikon_he_idwt_vertical.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE vertical inverse DWT implementation

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

// Nikon HE vertical 5/3 IDWT — implementation.
//
// See nikon_he_idwt_vertical.h for the public API.
//
// State machine — for each call (per LB component), based on the CURRENT
// state, perform the loop body and transition to the OUT state. All inner
// loops iterate `i in [0, n)` and skip when `n == 0` (but state still
// advances). All shifts are arithmetic on signed int32.
//
//   In  | x0   | Loop body                                              | Out
//   ----|------|--------------------------------------------------------|----
//    0  | new  | x2[i] = x0[i]                                          | 1
//    1  | new  | x2[i] = (x0[i] * 4) - x2[i]                           | 4
//    2  | new  | x2[i] = x0[i] * 4                                     | 5
//    4  | new  | w = x2[i] - x0[i]; x3[i] = (w+1)>>2; x2[i] = x0[i]     | 7
//    5  | new  | w = x2[i] - 2*x0[i]; x3[i] = (w+1)>>2; x2[i] = x0[i]   | 7
//    7  | new  | tmp = x3[i]; x1[i] = tmp;
//             | x3[i] = tmp + 2*x2[i]; x2[i] = (x0[i]<<2) - x2[i]       | 8
//    7  | NULL | x1[i] = x3[i]                          (partial tail)  | 9
//    8  | new  | w = x2[i] - x0[i]; w = (w+1)>>2;
//             | x1[i] = (w + x3[i]) >> 1;
//             | x2[i] = x0[i]; x3[i] = w                                | 7
//    9  | NULL | x1[i] = x3[i] + x2[i]                                  | 11
//   11  | any  | (no write; state stays done)                           | 11

#include "nikon_he_idwt_vertical.h"
#include <cstring>

namespace nikon_he {

void ver_lift_lb_step(
    const int32_t* x0,
    int32_t* x1_out,
    int n,
    VerLiftStatePerLB& st) {

    int32_t* x2 = st.x2_carry;
    int32_t* x3 = st.x3_carry;

    // The tail's rows arrive as nullptr, which only the steady and flush states
    // take; in any other state the stream ended early, and the decode fails.
    if (x0 == nullptr && n > 0 && st.state != VerLiftState::kState7 &&
        st.state != VerLiftState::kState9 && st.state != VerLiftState::kState11) {
        st.failed = true;
        return;
    }

    // NOTE: even when n == 0 (memcpy LB), the state STILL advances per the
    // transition table. Loop bodies are no-ops in that case.

    switch (st.state) {
    case VerLiftState::kInit0:                  // path B entry
        for (int i = 0; i < n; ++i) {
            x2[i] = x0[i];
        }
        st.state = VerLiftState::kInit1;
        break;

    case VerLiftState::kInit1:                  // path B step 2
        for (int i = 0; i < n; ++i) {
            x2[i] = (x0[i] * 4) - x2[i];
        }
        st.state = VerLiftState::kInit4;
        break;

    case VerLiftState::kInit4:                  // path B step 3
        for (int i = 0; i < n; ++i) {
            int32_t w = x2[i] - x0[i];
            x3[i] = (w + 1) >> 2;               // arithmetic >> on signed
            x2[i] = x0[i];
        }
        st.state = VerLiftState::kState7;
        break;

    case VerLiftState::kInit2:                  // path A entry (tile 0)
        for (int i = 0; i < n; ++i) {
            x2[i] = x0[i] * 4;
        }
        st.state = VerLiftState::kState5;
        break;

    case VerLiftState::kState5:                 // first residual (path A)
        for (int i = 0; i < n; ++i) {
            int32_t w = x2[i] - 2 * x0[i];
            x3[i] = (w + 1) >> 2;
            x2[i] = x0[i];
        }
        st.state = VerLiftState::kState7;
        break;

    case VerLiftState::kState7:                 // steady-state "even tick"
        if (x0 == nullptr) {
            // Partial-tile tail variant
            for (int i = 0; i < n; ++i) {
                x1_out[i] = x3[i];
            }
            st.state = VerLiftState::kState9;
        } else {
            for (int i = 0; i < n; ++i) {
                int32_t tmp = x3[i];
                x1_out[i] = tmp;
                x3[i] = tmp + 2 * x2[i];
                x2[i] = (x0[i] * 4) - x2[i];
            }
            st.state = VerLiftState::kState8;
        }
        break;

    case VerLiftState::kState8:                 // steady-state "odd tick"
        // n.b. x0==NULL not normally hit at state 8.
        for (int i = 0; i < n; ++i) {
            int32_t w = x2[i] - x0[i];
            w = (w + 1) >> 2;
            x1_out[i] = (w + x3[i]) >> 1;
            x2[i] = x0[i];
            x3[i] = w;
        }
        st.state = VerLiftState::kState7;
        break;

    case VerLiftState::kState9:                 // tail flush
        for (int i = 0; i < n; ++i) {
            x1_out[i] = x3[i] + x2[i];
        }
        st.state = VerLiftState::kState11;
        break;

    case VerLiftState::kState11:                // done (no write)
        break;

    default:
        break;
    }
}

}  // namespace nikon_he
