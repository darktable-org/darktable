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
 * feature.  This is a C port of the HDR use-case (`--preset hdr`) of the
 * reference `align_and_blend.py`: SIFT feature initialization -> RANSAC
 * homography, applied to exposure brackets shot on a shaky tripod or handheld.
 *
 * Unlike the Python tool, which operates on demosaiced RGB, this code operates
 * on the undemosaiced Bayer / X-Trans mosaic that the merge job accumulates.
 * It therefore (1) builds a reduced-resolution, CFA-free luma proxy for all
 * feature work and (2) applies the resulting homography to the full resolution
 * mosaic with a CFA-aware (same-color) resampler that preserves the mosaic
 * phase.  See dev-doc/HDR_Alignment_Design.md for the full design.
 *
 * The OpenCV-dependent primitives (SIFT, FLANN, findHomography) live behind a
 * narrow C-ABI seam implemented in hdr_alignment_cv.cc, because OpenCV 4's API
 * is C++ only.  When darktable is
 * built without OpenCV (HAVE_OPENCV undefined) the public functions degrade to
 * no-ops that report "no alignment", and the merge behaves exactly as before.
 */

#pragma once

#include <glib.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Motion model used for registration.  The HDR preset uses HOMOGRAPHY; AFFINE is
 * exposed for completeness / a possible UI choice. */
typedef enum dt_hdr_align_warp_mode_t
{
  DT_HDR_WARP_HOMOGRAPHY = 0,
  DT_HDR_WARP_AFFINE = 1
} dt_hdr_align_warp_mode_t;

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
 * NULL (use defaults); the values are clamped to sane ranges and copied. */
dt_hdr_align_t *dt_hdr_alignment_new(dt_hdr_align_warp_mode_t mode,
                                     const dt_hdr_align_params_t *params);
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
 *  - On success: writes the warped mosaic into `out` (caller-allocated,
 *    width*height floats) and returns TRUE.
 *  - On failure / no reliable warp: copies `mosaic` into `out` unchanged and
 *    returns FALSE, so the caller can accumulate the unaligned frame (never
 *    worse than the current, alignment-free behavior).
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
 * resolution.  Returns 0 if built without OpenCV or the proxy cannot be built.
 * Mirrors _count_lowres_sift_features() that drives `--auto-reference`. */
int dt_hdr_alignment_probe_features(const float *mosaic, int width, int height);

/* ------------------------------------------------------------------------- *
 *  OpenCV backend seam (implemented in hdr_alignment_cv.cc).
 *
 *  These are the only entry points that touch OpenCV.  All image data crosses
 *  as plain pointers; the homography crosses as a row-major double[9].  Kept in
 *  this header (rather than an internal one) so both the C and C++ translation
 *  units agree on the ABI.  Not part of the public darktable API.
 * ------------------------------------------------------------------------- */

typedef struct dt_hdr_cv_feature_stats_t
{
  int kp_template;      // keypoints kept in the reference proxy (after scale floor)
  int kp_image;         // keypoints kept in the moving proxy (after scale floor)
  int kp_template_raw;  // keypoints before the scale floor (reference)
  int kp_image_raw;     // keypoints before the scale floor (moving)
  int ratio_matches;    // matches passing the Lowe ratio test
  int good_matches;     // mutual-consistent matches fed to RANSAC
  int inliers;          // RANSAC inliers supporting the returned homography
  int used_affine;      // 1 if the affine fallback produced the result
  // inlier reprojection error (pixels) in proxy coords; < 0 if unavailable
  double reproj_mean;
  double reproj_median;
  double reproj_max;
} dt_hdr_cv_feature_stats_t;

/* Precompute and cache the reference frame's SIFT features (CLAHE -> detect ->
 * describe -> scale floor -> spatial balance) from its 8-bit luma proxy, so the
 * reference is detected ONCE per merge instead of being re-detected for every
 * moving frame.  Returns an opaque handle (free with dt_hdr_cv_features_destroy)
 * or NULL on failure.  `balance_target` / `clahe_clip` are as for
 * dt_hdr_cv_feature_homography(). */
void *dt_hdr_cv_features_create(const uint8_t *proxy,
                                int width,
                                int height,
                                int balance_target,
                                double clahe_clip);
void dt_hdr_cv_features_destroy(void *features);

/* Estimate the reference->image homography between the cached reference features
 * (`ref_features`, from dt_hdr_cv_features_create) and a moving 8-bit luma proxy
 * `img`, using SIFT + FLANN (Lowe ratio + mutual NN) + RANSAC findHomography,
 * with an estimateAffine2D fallback on weak homography support.  Only the moving
 * frame is detected here; the reference comes from the cache.  Mirrors
 * estimate_initial_warp_feature_ransac() for the HDR path.
 *
 * Writes the 3x3 row-major homography into H and returns the inlier count
 * (0 => estimation failed or ref_features is NULL; H is left as identity).
 * `balance_target` caps the moving keypoint set before matching (<= 0 disables
 * balancing); `clahe_clip` is the pre-SIFT CLAHE clip limit (<= 0 disables it)
 * and should match the value the reference cache was built with.
 *
 * `debug_dir` (NULL/"" disables) is the directory the per-frame debug visuals
 * are written to, and `frame_index` numbers them; both are owned by the caller
 * (the per-merge alignment state), so no global debug state is kept here and
 * concurrent merges do not interfere. */
int dt_hdr_cv_feature_homography(const void *ref_features,
                                 const uint8_t *img,
                                 int width,
                                 int height,
                                 int balance_target,
                                 double clahe_clip,
                                 const char *debug_dir,
                                 int frame_index,
                                 double H[9],
                                 dt_hdr_cv_feature_stats_t *stats);

/* Count SIFT keypoints on a low-resolution copy of `luma` (longest side scaled
 * to <= probe_dim).  Used by the optional auto-reference pre-pass to pick the
 * richest-feature frame.  Mirrors _count_lowres_sift_features(). */
int dt_hdr_cv_count_features(const uint8_t *luma, int width, int height, int probe_dim);

#ifdef __cplusplus
}
#endif

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
