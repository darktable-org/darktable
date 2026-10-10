/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#pragma once

#include <lcms2.h>
#include <math.h>

// the caller owns the profile; its encoding must match the Wayland image description
cmsHPROFILE dt_display_transport_create_profile(void);
cmsHPROFILE dt_display_ui_create_profile(void);

// linear BT.709 samples for the gamma22 UI surface, clamped before quantization
static inline float dt_display_ui_encode(const float linear)
{
  if(linear <= 0.0f) return 0.0f;
  if(linear >= 1.0f) return 1.0f;
  return powf(linear, 1.0f / 2.2f);
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
