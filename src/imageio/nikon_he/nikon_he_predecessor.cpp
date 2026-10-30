/* -*- C++ -*-
 * File: nikon_he_predecessor.cpp
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE cross-precinct GCLI predecessor state

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

#include "nikon_he_predecessor.h"
#include <algorithm>

namespace nikon_he {

void PrecinctPredecessorState::init(const SubbandConfig config[26]) {
    for (int band = 0; band < 26; ++band)
        gcli_[band].assign(config[band].ng, 0);
    precinct_index_ = 0;
}

const uint8_t* PrecinctPredecessorState::get_previous_gcli(int band) const {
    // LL is emitted twice per precinct: the second line predicts from the first,
    // and the first line of the next precinct predicts from this second line
    const int previous = band == 12 ? 23 : (band == 23 ? 12 : band);
    return gcli_[previous].data();
}

void PrecinctPredecessorState::save_gcli(int band, const uint8_t* values) {
    std::copy_n(values, gcli_[band].size(), gcli_[band].data());
}

void PrecinctPredecessorState::reset_gcli_state() {
    for (auto& band : gcli_)
        std::fill(band.begin(), band.end(), 0);
}

}  // namespace nikon_he
