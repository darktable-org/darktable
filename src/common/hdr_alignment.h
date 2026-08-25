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

/* Automatic registration of RAW CFA frames for the lighttable "merge HDR"
 * feature: SIFT feature initialization -> RANSAC homography, applied to
 * exposure brackets shot on a shaky tripod or handheld.
 *
 * Registration runs on the undemosaiced Bayer / X-Trans mosaic that the merge
 * job accumulates.  It therefore (1) builds a reduced-resolution, CFA-free luma
 * proxy for all feature work and (2) applies the resulting homography to the
 * full resolution mosaic with a CFA-aware (same-color) resampler that preserves
 * the mosaic phase.
 *
 * The implementation lives in hdr_alignment.cc: OpenCV 4's API is C++ only, so
 * the SIFT / FLANN / findHomography primitives force that translation unit to be
 * C++, and this header gives it C linkage so the merge job (and the unit tests)
 * can call it from C unchanged.  When darktable is built without OpenCV
 * (HAVE_OPENCV undefined) the public functions degrade to no-ops that report
 * "no alignment", and the merge behaves exactly as before.
 */

#pragma once

#include <glib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Outcome of aligning a single frame, for logging / diagnostics. */
typedef enum dt_hdr_align_status_t
{
  DT_HDR_ALIGN_OK = 0,    // a reliable warp was applied
  DT_HDR_ALIGN_IDENTITY,  // nothing reliable found, frame left unwarped
  DT_HDR_ALIGN_DISABLED   // built without OpenCV / alignment off
} dt_hdr_align_status_t;

typedef struct dt_hdr_align_result_t
{
  dt_hdr_align_status_t status;
  int feature_inliers;   // RANSAC inlier count from the feature stage
  double corner_drift;   // max corner displacement vs identity, in pixels
} dt_hdr_align_result_t;

/* Opaque per-merge alignment state.  Holds the cached reference proxy. */
typedef struct dt_hdr_align_t dt_hdr_align_t;

/* Runtime-tunable alignment parameters.  These mirror the compile-time defaults
 * (DT_HDR_PROXY_SCALE etc.) but let the merge job override them per run from the
 * user's preferences.  Pass NULL to dt_hdr_alignment_new() to use the defaults. */
typedef struct dt_hdr_align_params_t
{
  double proxy_scale;     // feature proxy size as a fraction of full res (≈0.5–1.0)
  double feature_gamma;   // display-gamma applied to the 8-bit SIFT proxy (1.0 = off)
  double clahe_clip;      // CLAHE clip limit before SIFT (0 = off; on aids extreme DR
                          //   but can cause false matches on repetitive textures)
  int sift_keypoints;     // per-frame SIFT keypoint budget after spatial balancing
  int debug_images;       // write per-frame alignment debug visuals (0 = off)
} dt_hdr_align_params_t;

/* Fill `p` with the built-in defaults (the DT_HDR_PROXY_* compile-time values).
 * Callers can then override individual fields from preferences. */
void dt_hdr_alignment_default_params(dt_hdr_align_params_t *p);

/* Create / destroy the alignment state for one HDR merge run.  `params` may be
 * NULL (use defaults); the values are clamped to sane ranges and copied.
 * Registration uses a projective (homography) motion model with an affine
 * fallback on weak support.
 *
 * Returns NULL on failure and, in particular, ALWAYS returns NULL when built
 * without OpenCV.  That is the switch that makes the feature inert: every other
 * entry point rejects a NULL state, so a caller that simply checks the return
 * value cannot reach any alignment work in a build that cannot align. */
dt_hdr_align_t *dt_hdr_alignment_new(const dt_hdr_align_params_t *params);
void dt_hdr_alignment_free(dt_hdr_align_t *a);

/* Cache the reference frame.  Builds the reduced-resolution 8-bit SIFT luma
 * proxy from the CFA mosaic and stores it on `a`.  `filters` and `xtrans`
 * describe the CFA (filters == 9u => X-Trans, else Bayer).
 * Returns TRUE on success. */
gboolean dt_hdr_alignment_set_reference(dt_hdr_align_t *a,
                                        const float *mosaic,
                                        int width,
                                        int height,
                                        uint32_t filters,
                                        const uint8_t (*xtrans)[6]);

/* Align one non-reference frame onto the cached reference.
 *
 * On success writes the warped mosaic into `out` (caller-allocated,
 * width*height floats) and returns TRUE.  On FALSE the caller must accumulate
 * the original `mosaic` -- `out` is NOT guaranteed to be populated, since a
 * reliable sub-pixel warp deliberately skips the redundant full-frame copy.
 * Read `mosaic`, never `out`, whenever FALSE is returned; `info->status` still
 * carries the OK / IDENTITY / DISABLED decision either way.
 *
 * `out` may not alias `mosaic`.  `info` may be NULL. */
gboolean dt_hdr_alignment_align_frame(dt_hdr_align_t *a,
                                      const float *mosaic,
                                      float *out,
                                      int width,
                                      int height,
                                      uint32_t filters,
                                      const uint8_t (*xtrans)[6],
                                      dt_hdr_align_result_t *info);

/* Probe a frame's feature richness for auto-reference selection: builds the luma
 * proxy and returns its SIFT keypoint count at the auto-reference probe
 * resolution.  Returns 0 if built without OpenCV or the proxy cannot be built. */
int dt_hdr_alignment_probe_features(const float *mosaic, int width, int height);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
