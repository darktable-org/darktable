/*
    This file is part of darktable,
    Copyright (C) 2016-2022 darktable developers.

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

// WARNING: do not #include anything in here!

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "Unfortunately we only work on litte-endian systems."
#endif

#if (defined(__amd64__) || defined(__amd64) || defined(__x86_64__) || defined(__x86_64))
#define DT_SUPPORTED_X86 1
#else
#define DT_SUPPORTED_X86 0
#endif

#if defined(__aarch64__) && (defined(__ARM_64BIT_STATE) && defined(__ARM_ARCH) && defined(__ARM_ARCH_8A) || defined(__APPLE__) || defined(__MINGW64__))
#define DT_SUPPORTED_ARMv8A 1
#else
#define DT_SUPPORTED_ARMv8A 0
#endif

#if defined(__PPC64__)
#define DT_SUPPORTED_PPC64 1
#else
#define DT_SUPPORTED_PPC64 0
#endif

#if DT_SUPPORTED_X86 && DT_SUPPORTED_ARMv8A
#error "Looks like hardware platform detection macros are broken?"
#endif

#if !DT_SUPPORTED_X86 && !DT_SUPPORTED_ARMv8A && !DT_SUPPORTED_PPC64
#error "Unfortunately we only work on amd64, ARMv8-A and PPC64 (64-bit little-endian only)."
#endif

#undef DT_SUPPORTED_PPC64
#undef DT_SUPPORTED_ARMv8A
#undef DT_SUPPORTED_X86

#if !defined(__SSE2__) || !defined(__SSE__)
#pragma message "Building without SSE2 is highly experimental."
#pragma message "Expect a LOT of functionality to be broken. You have been warned."
#endif

// double check for 32-bit architecture
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ < 8
#error "Unfortunately we only work on the 64-bit architectures amd64, ARMv8-A and PPC64."
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
