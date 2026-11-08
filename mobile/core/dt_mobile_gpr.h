/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "dt_mobile.h"
#if defined(__GNUC__)
#define DT_MOBILE_GPR_API __attribute__((visibility("default")))
#else
#define DT_MOBILE_GPR_API
#endif
#ifdef __cplusplus
extern "C" {
#endif

/* Decode VC-5 to full resolution Bayer DNG; never render an RGB preview.
 * The caller owns the returned private temporary file until cleanup. Source is
 * opened read-only. Android should supply its private cache directory. */
DT_MOBILE_GPR_API dt_mobile_status dt_mobile_gpr_prepare(const char *source_path, const char *cache_directory,
                                      char **out_path, char *error, size_t error_size);
DT_MOBILE_GPR_API void dt_mobile_gpr_cleanup(char *path);

#ifdef __cplusplus
}
#endif
