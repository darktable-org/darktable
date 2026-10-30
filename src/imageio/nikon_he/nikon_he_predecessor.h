/* -*- C++ -*-
 * File: nikon_he_predecessor.h
 * Copyright (C) 2026 Dmitri Sotnikov
 *
   Nikon HE cross-precinct GCLI predecessor state interface

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

#ifndef LIBRAW_NIKON_HE_PREDECESSOR_H
#define LIBRAW_NIKON_HE_PREDECESSOR_H

#include "nikon_he_subband_config.h"
#include <array>
#include <cstdint>
#include <vector>

namespace nikon_he {

class PrecinctPredecessorState {
public:
    void init(const SubbandConfig config[26]);
    const uint8_t* get_previous_gcli(int band) const;
    void save_gcli(int band, const uint8_t* values);
    void reset_gcli_state();
    void advance_precinct() { ++precinct_index_; }
    int precinct_index() const { return precinct_index_; }

private:
    std::array<std::vector<uint8_t>, 26> gcli_;
    int precinct_index_ = 0;
};

}  // namespace nikon_he

#endif  // LIBRAW_NIKON_HE_PREDECESSOR_H
