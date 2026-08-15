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

/* HDR exposure-bracket registration: SIFT feature initialization -> RANSAC
 * homography, applied to the raw CFA mosaic the merge job accumulates.
 *
 * This translation unit is C++ only because OpenCV 4 dropped its C API; the
 * public interface in hdr_alignment.h carries C linkage, so control_jobs.c calls
 * it as ordinary C.  It owns the whole pipeline:
 *
 *   - CFA mosaic  ->  reduced-resolution, CFA-free luma proxy
 *   - percentile normalization (1st/99th percentile stretch + clip)
 *   - SIFT detect/describe, FLANN matching, RANSAC homography  (OpenCV)
 *   - proxy-coordinate -> full-resolution homography rescale
 *   - CFA-aware (same-color) resampling of the full-resolution mosaic
 *   - warp sanity / corner-drift reliability gates
 *   - per-frame orchestration (set_reference / align_frame)
 *
 * Everything OpenCV-dependent sits behind HAVE_OPENCV; without it the public
 * functions degrade to no-ops that report "no alignment" and the merge behaves
 * exactly as before.  No OpenCV type appears in the public header.
 */

#include "common/hdr_alignment.h"

#include "common/darktable.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <type_traits>

#ifdef HAVE_OPENCV
#include <opencv2/core.hpp> // also brings in CV_VERSION_MAJOR
#include <opencv2/imgproc.hpp>
#if CV_VERSION_MAJOR >= 5
// OpenCV 5 renamed features2d to features and split calib3d into
// calib/geometry/stereo; SIFT/FLANN moved to the former, findHomography and
// estimateAffine2D to the latter.  The 4.x names still exist as compatibility
// headers, but using the current ones keeps us off the deprecation path.
#include <opencv2/features.hpp>
#include <opencv2/geometry.hpp>
#else
#include <opencv2/features2d.hpp>
#include <opencv2/calib3d.hpp>
#endif
#include <opencv2/flann.hpp> // KDTreeIndexParams / SearchParams

#include <algorithm>
#include <cstdio> // fopen/fwrite (Netpbm debug dump)
#include <vector>
#endif // HAVE_OPENCV

#ifdef HAVE_OPENCV
/* CFA color lookup.  Reproduced from develop/imageop_math.h (FC / FCNxtrans) so
 * this lean common file does not have to pull in the OpenCL / imageop headers
 * that imageop_math.h depends on.  Only the OpenCV-backed alignment path
 * resamples the mosaic, so these stay inside the guard to avoid an unused
 * function warning in builds without OpenCV. */
static inline int _fc(const std::size_t row, const std::size_t col, const std::uint32_t filters)
{
  return filters >> (((row << 1 & 14) + (col & 1)) << 1) & 3;
}

static inline int _fcol(const int row, const int col, const std::uint32_t filters,
                        const std::uint8_t (*const xtrans)[6])
{
  if (filters == 9u)
    // +600 (a multiple of the 6x6 X-Trans period) keeps the index non-negative.
    return xtrans[(row + 600) % 6][(col + 600) % 6];
  return _fc(static_cast<std::size_t>(row), static_cast<std::size_t>(col), filters);
}
#endif // HAVE_OPENCV

/* ------------------------------------------------------------------------- *
 *  Tuning constants.
 * ------------------------------------------------------------------------- */

// percentile-stretch bounds for the 8-bit feature proxy
#define DT_HDR_PERCENTILE_LOW 1.0
#define DT_HDR_PERCENTILE_HIGH 99.0
#define DT_HDR_SMALL_EPS 1e-6

// Default per-frame SIFT keypoint budget after spatial balancing.  Seeds the
// runtime parameter and is the fallback the detector uses for a non-positive
// budget.
#define DT_HDR_SIFT_KEYPOINTS 5000

// Feature-proxy downscale relative to the full-resolution mosaic (the legacy
// 2x2-block reduction was 0.5x).  A higher scale keeps more distinctive detail,
// so more keypoints survive and the matcher aliases less on periodic structure,
// at ~(scale/0.5)^2 SIFT cost.  This matters because the *linear* raw is far
// less feature-rich than what SIFT is normally fed: 4242 keypoints on one real
// frame vs 43626 for a tone-mapped rendition of it.
// Build with -DDT_HDR_PROXY_SCALE=0.5 for the legacy speed.
#ifndef DT_HDR_PROXY_SCALE
#define DT_HDR_PROXY_SCALE 0.625
#endif

// Display-gamma encoding of the 8-bit SIFT proxy, applied after the percentile
// stretch and before CLAHE, so the linear raw looks like the tone-mapped input
// SIFT is tuned for.  Since the stretch already covers [1,99], this is mostly a
// redistribution on top of it: it lifts keypoints on mid-key scenes but can cost
// a few on noisy deep-shadow frames.  Hence a knob; 1.0 disables it.
#ifndef DT_HDR_PROXY_FEATURE_GAMMA
#define DT_HDR_PROXY_FEATURE_GAMMA 2.2
#endif

// CLAHE clip limit applied to the 8-bit SIFT proxy before detection.  0 disables
// it.  Off by default: the display-gamma proxy already provides the tonal
// encoding SIFT wants, while CLAHE on repetitive textures changes descriptor
// signatures between exposures and manufactures false matches (the
// period-aliasing failure mode).  Raise it (e.g. 2.0) for genuinely
// feature-starved / extreme-DR brackets.
#ifndef DT_HDR_CLAHE_CLIP
#define DT_HDR_CLAHE_CLIP 0.0
#endif

// Longest-side resolution of the auto-reference SIFT probe.
#define DT_HDR_AUTO_REFERENCE_PROBE_DIM 1500

// Minimum RANSAC inlier count for the feature-init warp to be trusted by the
// orchestration.  Independent of kHomographyMinInliers, which decides inside the
// detector whether to try the affine fallback; the two happen to coincide today.
#define DT_HDR_FEATURE_MIN_INLIERS 50

// Warp sanity bounds.
#define DT_HDR_WARP_MAX_TRANSLATION_DIAG_FRAC 0.30
#define DT_HDR_WARP_SCALE_MIN 0.5
#define DT_HDR_WARP_SCALE_MAX 2.0

// Below this full-resolution corner motion the warp is treated as a no-op and
// the mosaic is copied through unresampled (avoids needlessly softening a
// frame that did not actually move).
#define DT_HDR_NOOP_MAX_CORNER_PX 0.5

// Half-width (px) of the separable same-color resampling tent.  For a Bayer
// period-2 sublattice this reduces to exact bilinear; for X-Trans it is a
// smooth same-color weighted average.
#define DT_HDR_CFA_TENT_RADIUS 2

// Accepted ranges for the runtime-tunable parameters.  These MUST stay in sync
// with the <type min .. max> bounds advertised for the matching
// plugins/lighttable/hdr_merge_* keys in data/darktableconfig.xml.in; the
// clamps in dt_hdr_alignment_new() are the safety net behind that UI.
#define DT_HDR_PROXY_SCALE_MIN 0.25
#define DT_HDR_PROXY_SCALE_MAX 1.0
#define DT_HDR_FEATURE_GAMMA_MIN 1.0
#define DT_HDR_FEATURE_GAMMA_MAX 6.0
#define DT_HDR_CLAHE_CLIP_MIN 0.0
#define DT_HDR_CLAHE_CLIP_MAX 16.0
#define DT_HDR_SIFT_KEYPOINTS_MIN 500
#define DT_HDR_SIFT_KEYPOINTS_MAX 20000

/* ------------------------------------------------------------------------- *
 *  Feature-matching constants and the OpenCV feature detector.  Everything to
 *  the matching #endif needs OpenCV and stays out of the public header.
 * ------------------------------------------------------------------------- */
#ifdef HAVE_OPENCV

/* Reference features, detected once per merge in set_reference and matched
 * against by every moving frame (so the reference SIFT pass is not repeated
 * N-1 times).  Keeps the CLAHE'd image too, for the debug visuals.
 *
 * Named namespace, not the anonymous one below: dt_hdr_align_t is declared in
 * the public header, so a member of internal-linkage type would trip
 * -Werror=subobject-linkage. */
namespace dt_hdr_align
{
  struct RefFeatures
  {
    cv::Mat image;                // CLAHE'd 8-bit reference proxy (debug visuals)
    std::vector<cv::KeyPoint> kp; // balanced reference keypoints
    cv::Mat des;                  // matching descriptor rows
    int kp_raw = 0;               // detections before the scale floor
    int kp_floor = 0;             // kept after the scale floor (balance input)
    // FLANN index over `des`, trained once.  Every moving frame's image->template
    // match reuses this instead of rebuilding the reference KD-tree N-1 times.
    cv::Ptr<cv::FlannBasedMatcher> matcher;
  };
} // namespace dt_hdr_align

using dt_hdr_align::RefFeatures;

namespace
{

  struct GFreeDeleter
  {
    void operator()(gchar *p) const { g_free(p); }
  };

  struct DtFreeAlignedDeleter
  {
    void operator()(void *p) const { dt_free_align(p); }
  };

  template <typename T>
  using AlignedBuf = std::unique_ptr<T[], DtFreeAlignedDeleter>;

  // Row-major 3x3 homography.  A std::array rather than double[9] so it can be
  // copied and assigned as a value and passed by reference without decaying to
  // a pointer.
  using Homography = std::array<double, 9>;

  // The no-op identity homography every path starts from.
  constexpr Homography kIdentityH = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};

  // SIFT detector (cv::SIFT::create).  nfeatures = 0 leaves the detector uncapped:
  // the per-frame budget is applied afterwards by the spatial balancing, which
  // keeps keypoints spread over the frame instead of taking the strongest N.
  // These match OpenCV's own defaults; passing them explicitly pins the detector
  // against a default changing under us (OpenCV 4 vs 5).
  constexpr int kSiftMaxFeatures = 0;
  constexpr int kSiftOctaveLayers = 3;
  constexpr double kSiftContrastThreshold = 0.04;
  constexpr double kSiftEdgeThreshold = 10.0;
  constexpr double kSiftSigma = 1.6;
  constexpr double kSiftMinScalePx = 6.0;

  // FLANN KD-tree matcher: randomized trees to build, and leaf checks per query.
  // Both are above OpenCV's defaults (4 trees / 32 checks): descriptor matching
  // quality decides the whole registration, and this stage is not the bottleneck.
  constexpr int kFlannKdTrees = 5;
  constexpr int kFlannSearchChecks = 50;

  constexpr double kRatioThreshold = 0.75;
  constexpr int kMinFeatureKeypoints = 25;
  constexpr int kMinGoodMatches = 25;
  constexpr int kMaxMatchesForRansac = 1800;

  constexpr double kRansacReprojThreshold = 2.5;
  constexpr int kRansacMaxIters = 5000;
  constexpr double kRansacConfidence = 0.995;
  constexpr int kHomographyMinInliers = 50;
  constexpr double kHomographyMinInlierRatio = 0.40;
  constexpr int kSpatialGrid = 6;                   // rows/cols of the match-spread grid
  constexpr int kClusterDegradeMaxCells = 2;        // inlier cells below which we degrade
  constexpr double kClusterTranslationMaxMad = 5.0; // max inlier displacement MAD (px)

  /* Per-frame feature-stage metrics, for the diagnostic log. */
  struct FeatureStats
  {
    int kp_template = 0;           // keypoints kept in the reference proxy (after scale floor)
    int kp_image = 0;              // keypoints kept in the moving proxy (after scale floor)
    int kp_template_raw = 0;       // keypoints before the scale floor (reference)
    int kp_image_raw = 0;          // keypoints before the scale floor (moving)
    int ratio_matches = 0;         // matches passing the Lowe ratio test
    int good_matches = 0;          // mutual-consistent matches fed to RANSAC
    int inliers = 0;               // RANSAC inliers supporting the returned homography
    bool used_affine = false;      // the affine fallback produced the result
    bool used_translation = false; // the result was refit to a pure translation
                                   //   (cluster-degradation); wins over used_affine
    // inlier reprojection error (pixels) in proxy coords; < 0 if unavailable
    double reproj_mean = -1.0;
    double reproj_median = -1.0;
    double reproj_max = -1.0;
  };

  // Wrap a borrowed 8-bit buffer as a single-channel cv::Mat header (no copy).
  cv::Mat wrapU8(const std::uint8_t *p, int w, int h)
  {
    return cv::Mat(h, w, CV_8U, const_cast<std::uint8_t *>(p));
  }

  // Row-major CV_32F 3x3 -> Homography.
  void fromMat3x3(const cv::Mat &m, Homography &H)
  {
    cv::Mat md;
    m.convertTo(md, CV_64F);
    for (int i = 0; i < 9; i++)
      H[i] = md.at<double>(i / 3, i % 3);
  }

  cv::Ptr<cv::SIFT> makeSift()
  {
    return cv::SIFT::create(kSiftMaxFeatures, kSiftOctaveLayers, kSiftContrastThreshold,
                            kSiftEdgeThreshold, kSiftSigma);
  }

  // Optional local-contrast enhancement on the 8-bit feature image (clip <= 0
  // disables it).  Off by default: CLAHE recovers features in extreme dynamic
  // range, but on *repetitive* textures it shifts descriptor signatures between
  // exposures and makes the matcher consense on a period-aliased shift.  Left as
  // a knob for genuinely feature-starved / extreme-DR brackets.
  void applyClahe(cv::Mat &img, double clip)
  {
    if (clip <= 0.0)
      return;
    cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(clip, cv::Size(8, 8));
    clahe->apply(img, img);
  }

  // Indices of keypoints at or above the scale floor (kSiftMinScalePx): the
  // smallest octaves are descriptor-ambiguous noise on low-contrast scenes.
  // Returns an index list (rather than mutating in place) so the caller can apply
  // the same selection to the parallel descriptor matrix via gatherKpDesc().
  std::vector<int> scaleFloorKeep(const std::vector<cv::KeyPoint> &kps)
  {
    std::vector<int> keep;
    keep.reserve(kps.size());
    for (int idx = 0; idx < static_cast<int>(kps.size()); idx++)
      if (kSiftMinScalePx <= 0.0 || kps[idx].size >= kSiftMinScalePx)
        keep.push_back(idx);
    return keep;
  }

  // Reorder keypoints and (when present) their descriptor rows by `keep`, in
  // keep-order, so the two stay aligned after any filter/balance step.  des may be
  // empty (no descriptors computed yet), in which case only kps is reordered.
  void gatherKpDesc(std::vector<cv::KeyPoint> &kps, cv::Mat &des, const std::vector<int> &keep)
  {
    std::vector<cv::KeyPoint> nk;
    nk.reserve(keep.size());
    const bool has_desc = !des.empty() && des.rows == static_cast<int>(kps.size());
    cv::Mat nd;
    if (has_desc)
      nd.create(static_cast<int>(keep.size()), des.cols, des.type());
    for (std::size_t k = 0; k < keep.size(); k++)
    {
      nk.push_back(kps[keep[k]]);
      if (has_desc)
        des.row(keep[k]).copyTo(nd.row(static_cast<int>(k)));
    }
    kps.swap(nk);
    if (has_desc)
      des = nd;
  }

  // Spatially balance keypoints down to `target`, keeping the strongest (by SIFT
  // response) in each cell of a grid of ~target cells, round-robined so coverage
  // stays even when cells are sparse.  Two effects matter here: it stops a
  // feature-dense reference (12888 vs 5075 on one real pair) from flooding the
  // matcher with near-duplicates that pass the ratio test at the *wrong* instance
  // of a periodic structure, and it makes the two keypoint budgets symmetric,
  // which mutual-NN matching assumes.  Returns kept indices for gatherKpDesc().
  std::vector<int> spatialBalanceKeep(const std::vector<cv::KeyPoint> &kps, int width, int height, int target)
  {
    std::vector<int> all(kps.size());
    for (int idx = 0; idx < static_cast<int>(kps.size()); idx++)
      all[idx] = idx;
    if (target <= 0 || static_cast<int>(kps.size()) <= target)
      return all;
    const double aspect = static_cast<double>(std::max(1, width)) / static_cast<double>(std::max(1, height));
    int cols = std::max(1, static_cast<int>(std::lround(std::sqrt(static_cast<double>(target) * aspect))));
    int rows = std::max(1, static_cast<int>(std::lround(static_cast<double>(target) / cols)));
    std::vector<std::vector<int>> cells(rows * cols);
    for (int idx = 0; idx < static_cast<int>(kps.size()); idx++)
    {
      int c = static_cast<int>(kps[idx].pt.x / std::max(1, width) * cols);
      int r = static_cast<int>(kps[idx].pt.y / std::max(1, height) * rows);
      c = std::clamp(c, 0, cols - 1);
      r = std::clamp(r, 0, rows - 1);
      cells[r * cols + c].push_back(idx);
    }
    for (auto &cell : cells)
      std::sort(cell.begin(), cell.end(),
                [&](int a, int b)
                { return kps[a].response > kps[b].response; });
    std::vector<int> out;
    out.reserve(target);
    std::size_t maxlen = 0;
    for (const auto &cell : cells)
      maxlen = std::max(maxlen, cell.size());
    for (std::size_t k = 0; k < maxlen && static_cast<int>(out.size()) < target; k++)
      for (const auto &cell : cells)
        if (k < cell.size())
        {
          out.push_back(cell[k]);
          if (static_cast<int>(out.size()) >= target)
            break;
        }
    return out;
  }

  // Inlier reprojection-error stats (mean/median/max, in pixels) of warp H mapping
  // template `src` -> image `dst`, restricted to RANSAC inliers.  Reported by the
  // "reproj mean/median/max" log line.
  // `ransac_mask` is the N x 1 CV_8U per-correspondence mask from findHomography /
  // estimateAffine2D -- one row per match, NOT an image.
  void reprojStats(const std::vector<cv::Point2f> &src, const std::vector<cv::Point2f> &dst,
                   const cv::Mat &ransac_mask, const cv::Mat &H, FeatureStats *stats)
  {
    if (!stats)
      return;
    stats->reproj_mean = stats->reproj_median = stats->reproj_max = -1.0;
    if (H.empty() || src.empty())
      return;
    std::vector<cv::Point2f> proj;
    cv::perspectiveTransform(src, proj, H);
    std::vector<double> errs;
    errs.reserve(src.size());
    for (std::size_t k = 0; k < src.size(); k++)
    {
      if (!ransac_mask.empty() && ransac_mask.at<uchar>(static_cast<int>(k)) == 0)
        continue;
      const double dx = static_cast<double>(proj[k].x) - dst[k].x;
      const double dy = static_cast<double>(proj[k].y) - dst[k].y;
      errs.push_back(std::sqrt(dx * dx + dy * dy));
    }
    if (errs.empty())
      return;
    double sum = 0.0, mx = 0.0;
    for (double e : errs)
    {
      sum += e;
      mx = std::max(mx, e);
    }
    std::sort(errs.begin(), errs.end());
    stats->reproj_mean = sum / static_cast<double>(errs.size());
    stats->reproj_median = errs[errs.size() / 2];
    stats->reproj_max = mx;
  }

  // Subsample matches to an even spatial distribution over a kSpatialGrid^2 grid of
  // the image, so RANSAC is constrained by correspondences from the whole frame
  // instead of one dense, well-exposed region.
  std::vector<cv::DMatch> spatialSubsample(const std::vector<cv::DMatch> &matches,
                                           const std::vector<cv::KeyPoint> &kp_img,
                                           int width, int height, int target)
  {
    if (static_cast<int>(matches.size()) <= target)
      return matches;
    const int gr = kSpatialGrid, gc = kSpatialGrid;
    std::vector<std::vector<int>> cells(gr * gc);
    for (int idx = 0; idx < static_cast<int>(matches.size()); idx++)
    {
      const cv::Point2f &p = kp_img[matches[idx].queryIdx].pt;
      int r = static_cast<int>(p.y / std::max(1, height) * gr);
      int c = static_cast<int>(p.x / std::max(1, width) * gc);
      r = std::clamp(r, 0, gr - 1);
      c = std::clamp(c, 0, gc - 1);
      cells[r * gc + c].push_back(idx);
    }
    for (auto &cell : cells)
      std::sort(cell.begin(), cell.end(),
                [&](int a, int b)
                { return matches[a].distance < matches[b].distance; });
    std::vector<cv::DMatch> out;
    out.reserve(target);
    std::size_t maxlen = 0;
    for (const auto &cell : cells)
      maxlen = std::max(maxlen, cell.size());
    for (std::size_t k = 0; k < maxlen && static_cast<int>(out.size()) < target; k++)
      for (const auto &cell : cells)
      {
        if (k < cell.size())
        {
          out.push_back(matches[cell[k]]);
          if (static_cast<int>(out.size()) >= target)
            break;
        }
      }
    return out;
  }

  // When RANSAC inliers cluster into <= kClusterDegradeMaxCells grid cells, an 8-DOF
  // homography overfits scale/shear/perspective to a tiny region and extrapolates
  // wildly.  If the inlier displacements are consistent (low MAD), refit as a pure
  // translation from their median.
  bool degradeClusteredToTranslation(const std::vector<cv::Point2f> &src,
                                     const std::vector<cv::Point2f> &dst,
                                     const cv::Mat &ransac_mask, int width, int height,
                                     Homography &H)
  {
    const int gr = kSpatialGrid, gc = kSpatialGrid;
    bool occ[kSpatialGrid * kSpatialGrid] = {false};
    int n_cells = 0;
    std::vector<double> dxs, dys;
    for (std::size_t k = 0; k < src.size(); k++)
    {
      if (!ransac_mask.empty() && ransac_mask.at<uchar>(static_cast<int>(k)) == 0)
        continue;
      int r = static_cast<int>(src[k].y / std::max(1, height) * gr);
      int c = static_cast<int>(src[k].x / std::max(1, width) * gc);
      r = std::clamp(r, 0, gr - 1);
      c = std::clamp(c, 0, gc - 1);
      if (!occ[r * gc + c])
      {
        occ[r * gc + c] = true;
        n_cells++;
      }
      dxs.push_back(static_cast<double>(dst[k].x) - src[k].x);
      dys.push_back(static_cast<double>(dst[k].y) - src[k].y);
    }
    if (static_cast<int>(dxs.size()) < 4 || n_cells > kClusterDegradeMaxCells)
      return false;

    std::vector<double> sx = dxs, sy = dys;
    std::sort(sx.begin(), sx.end());
    std::sort(sy.begin(), sy.end());
    const double tx = sx[sx.size() / 2], ty = sy[sy.size() / 2];
    std::vector<double> ax, ay;
    ax.reserve(dxs.size());
    ay.reserve(dys.size());
    for (double v : dxs)
      ax.push_back(std::fabs(v - tx));
    for (double v : dys)
      ay.push_back(std::fabs(v - ty));
    std::sort(ax.begin(), ax.end());
    std::sort(ay.begin(), ay.end());
    if (std::max(ax[ax.size() / 2], ay[ay.size() / 2]) > kClusterTranslationMaxMad)
      return false;

    H = Homography{1.0, 0.0, tx, //
                   0.0, 1.0, ty, //
                   0.0, 0.0, 1.0};
    return true;
  }

  /* ------------------------------------------------------------------------- *
   *  Optional debug-image dump.
   *
   *  When the alignment state carries a directory (from the preference or the
   *  DT_HDR_DEBUG_IMAGE_DIR override), each aligned (moving) frame writes a
   *  numbered set of Netpbm images (PGM/PPM) to it: the CLAHE'd SIFT input, the
   *  detected keypoints, and the colour-coded match visualisation (green = inlier,
   *  red = outlier).  Entirely diagnostic: no effect unless a directory is set.
   * ------------------------------------------------------------------------- */

  // Longest side of a dumped image; large proxies are scaled down to stay viewable
  // and to keep the (uncompressed) files reasonable.
  constexpr int kDebugMaxSide = 1600;

  // Write `img` (8-bit 1- or 3-channel, or float/other normalised to [0,255]) as a
  // Netpbm image -- PGM (P5) for grayscale, PPM (P6) for colour -- scaling it down
  // if larger than kDebugMaxSide.  Netpbm needs no image library (keeps OpenCV to
  // its minimal module set); the files open in any common image viewer.  Never
  // throws into the caller.
  void writeDebugImage(const std::string &dir, int frame, const char *name, const cv::Mat &img)
  {
    try
    {
      if (img.empty())
        return;
      cv::Mat u8;
      if (img.depth() == CV_8U)
        u8 = img;
      else
      {
        cv::normalize(img, u8, 0, 255, cv::NORM_MINMAX);
        u8.convertTo(u8, img.channels() == 3 ? CV_8UC3 : CV_8U);
      }
      const int side = std::max(u8.cols, u8.rows);
      if (side > kDebugMaxSide)
      {
        const double f = static_cast<double>(kDebugMaxSide) / side;
        cv::resize(u8, u8, cv::Size(), f, f, cv::INTER_AREA);
      }
      const int ch = u8.channels();
      char path[1024];
      snprintf(path, sizeof(path), "%s/hdr_frame%02d_%s.%s", dir.c_str(), frame, name,
               ch == 3 ? "ppm" : "pgm");
      FILE *fp = fopen(path, "wb");
      if (!fp)
        return;
      fprintf(fp, "P%c\n%d %d\n255\n", ch == 3 ? '6' : '5', u8.cols, u8.rows);
      // OpenCV is BGR, Netpbm wants RGB.  Write row by row: a Mat is not
      // necessarily continuous, so its rows may carry padding.
      cv::Mat out;
      if (ch == 3)
        cv::cvtColor(u8, out, cv::COLOR_BGR2RGB);
      else
        out = u8;
      for (int y = 0; y < out.rows; y++)
        fwrite(out.ptr<unsigned char>(y), 1, static_cast<std::size_t>(out.cols) * ch, fp);
      fclose(fp);
      dt_print(DT_DEBUG_HDR_MERGE, "  debug image: %s", path);
    }
    catch (const cv::Exception &e)
    {
      dt_print(DT_DEBUG_HDR_MERGE, "  debug image '%s' failed: %s", name, e.what());
    }
  }

  // Single SIFT pass on one 8-bit proxy.  detectAndCompute builds the Gaussian
  // pyramid once where a separate detect + compute would build it twice; the
  // descriptors wasted on later-pruned keypoints cost far less than that second
  // pyramid.  The scale floor and spatial balance then prune keypoints and their
  // descriptor rows together.  `out_img` receives the SIFT input, for the debug
  // visuals.
  void detectDescribe(const cv::Mat &proxy, int balance_target, double clahe_clip,
                      std::vector<cv::KeyPoint> &kp, cv::Mat &des, cv::Mat &out_img, int &raw,
                      int &after_floor)
  {
    out_img = proxy.clone(); // clone: CLAHE writes in place
    applyClahe(out_img, clahe_clip);
    cv::Ptr<cv::SIFT> sift = makeSift();
    sift->detectAndCompute(out_img, cv::noArray(), kp, des);
    raw = static_cast<int>(kp.size());
    gatherKpDesc(kp, des, scaleFloorKeep(kp));
    after_floor = static_cast<int>(kp.size());
    // Spatially balance to a common budget so a feature-dense frame cannot flood
    // the matcher with aliased candidates.
    gatherKpDesc(kp, des, spatialBalanceKeep(kp, proxy.cols, proxy.rows, balance_target));
  }

  // Build a FLANN KD-tree matcher (5 randomized trees, 50 checks) -- the same
  // parameters used for the per-frame matching.  Factored out so the reference
  // index and the per-frame moving index are built identically.
  cv::Ptr<cv::FlannBasedMatcher> makeFlann()
  {
    return cv::makePtr<cv::FlannBasedMatcher>(
        cv::makePtr<cv::flann::KDTreeIndexParams>(kFlannKdTrees),
        cv::makePtr<cv::flann::SearchParams>(kFlannSearchChecks));
  }

  // Detect + cache the reference frame's SIFT features (CLAHE -> detect ->
  // describe -> scale floor -> spatial balance) from its 8-bit luma proxy, so the
  // reference is detected ONCE per merge instead of being re-detected for every
  // moving frame.  Returns nullptr on failure, in which case the caller falls
  // through and leaves every frame unaligned.
  std::unique_ptr<RefFeatures> detectReference(const cv::Mat &proxy, int balance_target,
                                               double clahe_clip)
  {
    if (balance_target <= 0)
      balance_target = DT_HDR_SIFT_KEYPOINTS;
    try
    {
      auto f = std::make_unique<RefFeatures>();
      detectDescribe(proxy, balance_target, clahe_clip, f->kp, f->des, f->image, f->kp_raw,
                     f->kp_floor);
      // Train the reference-side FLANN index once; every moving frame reuses it.
      if (!f->des.empty())
      {
        f->matcher = makeFlann();
        f->matcher->add(std::vector<cv::Mat>{f->des});
        f->matcher->train();
      }
      return f;
    }
    catch (const std::exception &e)
    {
      dt_print(DT_DEBUG_HDR_MERGE, "HDR ref feature precompute raised: %s", e.what());
      return nullptr;
    }
  }

  /* Estimate the reference->image homography from the cached reference features
   * and a moving 8-bit luma proxy: SIFT + FLANN (Lowe ratio + mutual NN) + RANSAC
   * findHomography, with an estimateAffine2D fallback on weak support.  Only the
   * moving frame is detected here.
   *
   * Writes the row-major 3x3 into H and returns the inlier count (0 => failed, H
   * left as identity).  `clahe_clip` must match what the reference cache was built
   * with.  `debug_dir` (empty disables) and `frame_index` come from the per-merge
   * state, so concurrent merges do not interfere. */
  int featureHomography(const RefFeatures *ref, const cv::Mat &img, int balance_target,
                        double clahe_clip, const std::string &debug_dir, int frame_index,
                        Homography &H, FeatureStats *stats)
  {
    if (balance_target <= 0)
      balance_target = DT_HDR_SIFT_KEYPOINTS;
    H = kIdentityH;
    if (stats)
      *stats = FeatureStats{};

    if (!ref || ref->kp.empty() || ref->des.empty())
      return 0;

    try
    {
      // The reference (template) comes from the per-merge cache; only the moving
      // frame is detected here.  Bind t / kp_t / des_t to the cache so the matching
      // and debug code below is identical to the symmetric (uncached) version.
      const cv::Mat &t = ref->image;
      const std::vector<cv::KeyPoint> &kp_t = ref->kp;
      const cv::Mat &des_t = ref->des;

      cv::Mat mov_img, des_i;
      std::vector<cv::KeyPoint> kp_i;
      int ki_raw = 0, ki_floor = 0;
      detectDescribe(img, balance_target, clahe_clip, kp_i, des_i, mov_img, ki_raw,
                     ki_floor);

      if (stats)
      {
        stats->kp_template_raw = ref->kp_raw;
        stats->kp_image_raw = ki_raw;
      }
      dt_print(DT_DEBUG_HDR_MERGE,
               "SIFT scale floor: kept template %d/%d, image %d/%d keypoints with kp.size >= %.1fpx",
               ref->kp_floor, ref->kp_raw, ki_floor, ki_raw, kSiftMinScalePx);
      if (static_cast<int>(kp_i.size()) < ki_floor)
        dt_print(DT_DEBUG_HDR_MERGE,
                 "SIFT spatial balance: image %d->%d keypoints", ki_floor, static_cast<int>(kp_i.size()));

      if (stats)
      {
        stats->kp_template = static_cast<int>(kp_t.size());
        stats->kp_image = static_cast<int>(kp_i.size());
      }
      if (des_t.empty() || des_i.empty() || static_cast<int>(kp_t.size()) < kMinFeatureKeypoints || static_cast<int>(kp_i.size()) < kMinFeatureKeypoints)
      {
        dt_print(DT_DEBUG_HDR_MERGE,
                 "SIFT init: insufficient features (need >= %d), using identity initial warp",
                 kMinFeatureKeypoints);
        return 0;
      }

      // FLANN kNN + Lowe ratio in both directions, keeping the mutually-best
      // correspondences.  `it` (image->template) reuses ref->matcher, so the
      // reference KD-tree is built once per merge; `ti` builds a fresh index over
      // the moving descriptors every frame (unavoidable).  The two are
      // independent, so they run as two parallel sections.
      std::vector<std::vector<cv::DMatch>> knn_it, knn_ti;
      bool match_failed = false;
      std::string match_err;
      auto capture_err = [&](const cv::Exception &e)
      {
#ifdef _OPENMP
#pragma omp critical(hdr_match_err)
#endif
        {
          match_failed = true;
          match_err = e.what();
        }
      };

#ifdef _OPENMP
#pragma omp parallel sections num_threads(2)
#endif
      {
#ifdef _OPENMP
#pragma omp section
#endif
        {
          try
          {
            // Reuse the trained reference index; fall back to a fresh one only if
            // the cache is somehow absent (defensive -- des_t is non-empty here).
            if (ref->matcher)
              ref->matcher->knnMatch(des_i, knn_it, 2);
            else
              makeFlann()->knnMatch(des_i, des_t, knn_it, 2);
          }
          catch (const cv::Exception &e)
          {
            capture_err(e);
          }
        }
#ifdef _OPENMP
#pragma omp section
#endif
        {
          try
          {
            makeFlann()->knnMatch(des_t, des_i, knn_ti, 2);
          }
          catch (const cv::Exception &e)
          {
            capture_err(e);
          }
        }
      }
      if (match_failed)
      {
        dt_print(DT_DEBUG_HDR_MERGE, "SIFT init match raised: %s", match_err.c_str());
        return 0;
      }

      // Reverse best (template kp -> image kp) after the ratio test.
      std::vector<int> rev_best(kp_t.size(), -1);
      for (const auto &m : knn_ti)
        if (m.size() >= 2 && m[0].distance < kRatioThreshold * m[1].distance)
          rev_best[m[0].queryIdx] = m[0].trainIdx;

      int ratio_count = 0;
      std::vector<cv::DMatch> good;
      good.reserve(knn_it.size());
      for (const auto &m : knn_it)
      {
        if (m.size() < 2)
          continue;
        if (m[0].distance >= kRatioThreshold * m[1].distance)
          continue; // Lowe ratio
        ratio_count++;
        // Mutual-consistency: the template kp must point back to this image kp.
        if (rev_best[m[0].trainIdx] != m[0].queryIdx)
          continue;
        good.push_back(m[0]);
      }
      if (stats)
      {
        stats->ratio_matches = ratio_count;
        stats->good_matches = static_cast<int>(good.size());
      }
      if (static_cast<int>(good.size()) > 0)
        dt_print(DT_DEBUG_HDR_MERGE,
                 "SIFT init: %d/%d ratio-test matches are mutual-consistent",
                 static_cast<int>(good.size()), ratio_count);
      if (static_cast<int>(good.size()) < kMinGoodMatches)
      {
        dt_print(DT_DEBUG_HDR_MERGE,
                 "SIFT init: only %d good matches, using identity initial warp",
                 static_cast<int>(good.size()));
        return 0;
      }

      // Subsample matches to an even spatial distribution, so RANSAC is constrained
      // by correspondences across the whole frame rather than one dense region.
      {
        const int before = static_cast<int>(good.size());
        good = spatialSubsample(good, kp_i, img.cols, img.rows, kMaxMatchesForRansac);
        if (static_cast<int>(good.size()) < before)
          dt_print(DT_DEBUG_HDR_MERGE,
                   "SIFT init: spatially-subsampled to %d matches (%dx%d grid) for RANSAC",
                   static_cast<int>(good.size()), kSpatialGrid, kSpatialGrid);
      }

      // src = template coords, dst = image coords  =>  warp maps template->image,
      // consistent with WARP_INVERSE_MAP sampling.
      std::vector<cv::Point2f> src, dst;
      src.reserve(good.size());
      dst.reserve(good.size());
      for (const auto &m : good)
      {
        src.push_back(kp_t[m.trainIdx].pt);
        dst.push_back(kp_i[m.queryIdx].pt);
      }

      cv::Mat inliers;
      cv::Mat Hh = cv::findHomography(src, dst, cv::RANSAC, kRansacReprojThreshold, inliers,
                                      kRansacMaxIters, kRansacConfidence);
      int n_in = inliers.empty() ? 0 : cv::countNonZero(inliers);
      const int homog_inliers = n_in;
      const double homog_ratio = good.empty() ? 0.0 : static_cast<double>(n_in) / static_cast<double>(good.size());
      const bool weak = Hh.empty() || n_in < kHomographyMinInliers || homog_ratio < kHomographyMinInlierRatio;

      // Choose the model: homography, or estimateAffine2D fallback on weak support.
      cv::Mat chosen, chosen_inl;
      bool used_affine = false;
      if (weak)
      {
        cv::Mat ainl;
        cv::Mat A = cv::estimateAffine2D(src, dst, ainl, cv::RANSAC, kRansacReprojThreshold,
                                         kRansacMaxIters, kRansacConfidence);
        if (!A.empty())
        {
          chosen = cv::Mat::eye(3, 3, CV_64F);
          A.copyTo(chosen(cv::Rect(0, 0, 3, 2)));
          chosen_inl = ainl;
          used_affine = true;
          n_in = ainl.empty() ? 0 : cv::countNonZero(ainl);
          dt_print(DT_DEBUG_HDR_MERGE,
                   "SIFT init: weak homography support (%d/%d inliers, %.0f%%); using affine fallback",
                   homog_inliers, static_cast<int>(good.size()), homog_ratio * 100.0);
        }
      }
      if (chosen.empty())
      {
        if (Hh.empty())
          return 0;
        chosen = Hh;
        chosen_inl = inliers;
      }
      fromMat3x3(chosen, H);

      // Cluster-degradation: a homography (or affine) fit to inliers crammed into a
      // couple of grid cells overfits scale/shear and extrapolates wildly across the
      // frame; if the inlier displacements agree, refit as a robust translation.
      Homography Htrans;
      cv::Mat reproj_H = chosen;
      bool used_translation = false;
      if (degradeClusteredToTranslation(src, dst, chosen_inl, img.cols, img.rows, Htrans))
      {
        H = Htrans;
        used_translation = true;
        reproj_H = cv::Mat::eye(3, 3, CV_64F);
        reproj_H.at<double>(0, 2) = Htrans[2];
        reproj_H.at<double>(1, 2) = Htrans[5];
        dt_print(DT_DEBUG_HDR_MERGE,
                 "SIFT init: inliers cluster in <= %d cells; refitting as translation-only "
                 "tx=%.2f ty=%.2f",
                 kClusterDegradeMaxCells, Htrans[2], Htrans[5]);
      }

      if (stats)
      {
        stats->inliers = n_in;
        stats->used_affine = used_affine;
        stats->used_translation = used_translation;
      }
      reprojStats(src, dst, chosen_inl, reproj_H, stats);

      // Optional debug visuals: the (CLAHE'd) SIFT input, the kept keypoints, and
      // the colour-coded matches -- the quickest way to see *why* the match set
      // consensed where it did (e.g. a periodic structure matching one period off).
      // `debug_dir` / `frame_index` are supplied by the (per-merge) caller.
      if (!debug_dir.empty())
      {
        const int fr = frame_index;
        // The reference (template) visuals are identical for every moving frame,
        // so write them once (on the first aligned frame) instead of per frame.
        if (frame_index <= 1)
        {
          writeDebugImage(debug_dir, fr, "1_template_sift_input", t);
          cv::Mat kt_vis;
          cv::drawKeypoints(t, kp_t, kt_vis, cv::Scalar::all(-1),
                            cv::DrawMatchesFlags::DRAW_RICH_KEYPOINTS);
          writeDebugImage(debug_dir, fr, "3_template_keypoints", kt_vis);
        }
        writeDebugImage(debug_dir, fr, "2_image_sift_input", mov_img);
        cv::Mat ki_vis;
        cv::drawKeypoints(mov_img, kp_i, ki_vis, cv::Scalar::all(-1),
                          cv::DrawMatchesFlags::DRAW_RICH_KEYPOINTS);
        writeDebugImage(debug_dir, fr, "4_image_keypoints", ki_vis);
        // All matches, colour-coded: red = RANSAC outlier, green = inlier.  `good`
        // matches have queryIdx -> kp_i (image), trainIdx -> kp_t (template), and
        // chosen_inl is the per-match inlier mask over `good`.  Draw the outliers
        // first, then the inliers over the top so the consensus stands out.
        std::vector<char> inlier_mask(good.size(), 0), outlier_mask(good.size(), 1);
        for (std::size_t k = 0; k < good.size(); k++)
        {
          const bool in = !chosen_inl.empty() && chosen_inl.at<uchar>(static_cast<int>(k));
          inlier_mask[k] = in ? 1 : 0;
          outlier_mask[k] = in ? 0 : 1;
        }
        const cv::Scalar red(0, 0, 255), green(0, 255, 0); // BGR
        cv::Mat match_vis;
        cv::drawMatches(mov_img, kp_i, t, kp_t, good, match_vis, red, red, outlier_mask,
                        cv::DrawMatchesFlags::NOT_DRAW_SINGLE_POINTS);
        cv::drawMatches(mov_img, kp_i, t, kp_t, good, match_vis, green, green, inlier_mask,
                        cv::DrawMatchesFlags::DRAW_OVER_OUTIMG | cv::DrawMatchesFlags::NOT_DRAW_SINGLE_POINTS);
        writeDebugImage(debug_dir, fr, "5_all_matches_green_inlier_red_outlier", match_vis);
      }
      return n_in;
    }
    catch (const cv::Exception &e)
    {
      dt_print(DT_DEBUG_HDR_MERGE, "SIFT init raised: %s", e.what());
      return 0;
    }
  }

  // Count SIFT keypoints on a low-resolution copy of `luma` (longest side scaled to
  // <= probe_dim).  Used by the optional auto-reference pre-pass to pick the
  // richest-feature frame.
  // Takes the proxy as a cv::Mat: a reference cannot be null, and the extent
  // travels with the data instead of in separate width/height arguments that
  // could disagree with the buffer.  Callers wrap their buffer with wrapU8().
  int countFeatures(const cv::Mat &luma, int probe_dim)
  {
    try
    {
      cv::Mat resized;
      const int max_side = std::max(luma.cols, luma.rows);
      if (probe_dim > 0 && max_side > probe_dim)
      {
        const double s = static_cast<double>(probe_dim) / static_cast<double>(max_side);
        cv::resize(luma, resized, cv::Size(), s, s, cv::INTER_AREA);
      }
      // The probe ranks the gamma-encoded proxies as detection sees them (CLAHE
      // off by default); the ranking only needs to be self-consistent.  Detect
      // directly on the wrapped (or resized) buffer -- SIFT does not modify its
      // input, so no defensive copy is needed.
      const cv::Mat &probe = resized.empty() ? luma : resized;

      cv::Ptr<cv::SIFT> sift = makeSift();
      std::vector<cv::KeyPoint> kps;
      sift->detect(probe, kps);
      return static_cast<int>(kps.size());
    }
    catch (const cv::Exception &)
    {
      return 0;
    }
  }

} // namespace

#endif // HAVE_OPENCV

/* ------------------------------------------------------------------------- *
 *  Alignment state.
 * ------------------------------------------------------------------------- */

struct dt_hdr_align_t
{
  // Runtime-tunable parameters (proxy scale, feature gamma, CLAHE clip, SIFT
  // budget), seeded from the compile-time defaults and overridable per run.
  dt_hdr_align_params_t params = {};

  // Per-merge debug-image policy, resolved once in dt_hdr_alignment_new() (env
  // override or the preference).  Empty => debug images off.  `debug_frame` is
  // bumped for each aligned moving frame so its visuals share a number.  Owned
  // here (not a global) so concurrent HDR merges cannot interfere.
  std::string debug_dir;
  int debug_frame = 0;

#ifdef HAVE_OPENCV
  // Cached reference SIFT features, computed once from the reference proxy in
  // set_reference and matched against by every moving frame -- so the reference
  // is detected once per merge, not N-1 times.  Null if the precompute failed.
  std::unique_ptr<RefFeatures> ref_features;
#endif
  int pw = 0, ph = 0; // proxy dimensions

  // Reference full-resolution geometry / CFA description.
  int width = 0, height = 0;
  std::uint32_t filters = 0;
  std::uint8_t xtrans[6][6] = {};

  gboolean have_reference = FALSE;
};

/* ------------------------------------------------------------------------- *
 *  Small 3x3 (row-major) homography helpers.
 *
 *  These (and the gate / CFA-warp / logging helpers further down) are only used
 *  on the alignment path, which needs OpenCV; guard them so a build without
 *  OpenCV does not trip -Werror=unused-function on the plain static helpers.
 * ------------------------------------------------------------------------- */
#ifdef HAVE_OPENCV

// Perspective-divide guard for w = H[6]x + H[7]y + H[8].  Below this the warp is
// degenerate at that point and the reciprocal saturates instead of exploding;
// the ceiling is 1/kPerspectiveWMin so the mapping stays continuous across the
// cut.  Deliberately far smaller than DT_HDR_SMALL_EPS: this is a "genuinely
// zero in double" test, not a tolerance.
constexpr double kPerspectiveWMin = 1e-12;
constexpr double kPerspectiveInvWMax = 1.0 / kPerspectiveWMin;

// Map a point through a row-major homography.
[[nodiscard]] static inline std::pair<double, double> _h_apply(const Homography &H, double x,
                                                               double y)
{
  const double w = H[6] * x + H[7] * y + H[8];
  const double iw = (std::fabs(w) < kPerspectiveWMin) ? kPerspectiveInvWMax : 1.0 / w;
  return {(H[0] * x + H[1] * y + H[2]) * iw, (H[3] * x + H[4] * y + H[5]) * iw};
}

// Proxy -> full-resolution coordinates: H_full = S * H_proxy * S^-1 with
// S = diag(sx, sy, 1), sx/sy being the full-res / proxy size ratios.  Those are
// nearly equal but kept independent so an off-square rounding of the proxy dims
// cannot skew the warp.  Expanding S*H*S^-1 gives the factors below.
[[nodiscard]] static Homography _h_scaled_proxy_to_full(const Homography &H, double sx,
                                                        double sy)
{
  return {H[0], H[1] * sx / sy, H[2] * sx, // a, b, translation x
          H[3] * sy / sx, H[4], H[5] * sy, // d, e, translation y
          H[6] / sx, H[7] / sy, H[8]};     // perspective x, y
}

/* ------------------------------------------------------------------------- *
 *  Percentile bounds of the proxy (the 1st / 99th order statistics).
 *
 *  These only gate an 8-bit normalization, so exact order statistics are
 *  unnecessary: a uniform histogram over [min,max] is accurate enough, and is
 *  O(n) and fully parallel where a sort would be the serial cost of this path.
 * ------------------------------------------------------------------------- */
static void _percentile_bounds(const float *src, std::size_t n, double low, double high,
                               float *p_low, float *p_high)
{
  constexpr int kHdrPercentileBins{4096};
  *p_low = 0.0f;
  *p_high = 1.0f;
  if (n == 0)
    return;

  // Pass 1: data range (parallel min/max reduction).
  float lo = src[0], hi = src[0];
  DT_OMP_FOR(reduction(min : lo) reduction(max : hi))
  for (std::size_t i = 0; i < n; i++)
  {
    const float v = src[i];
    if (v < lo)
      lo = v;
    if (v > hi)
      hi = v;
  }
  if (!(hi > lo)) // flat (or single-valued) image: nothing to stretch
  {
    *p_low = lo;
    *p_high = hi;
    return;
  }

  // Pass 2: uniform histogram over [lo,hi] (parallel array reduction).
  std::uint32_t hist[kHdrPercentileBins] = {0};
  const double sbin = static_cast<double>(kHdrPercentileBins) / (static_cast<double>(hi) - static_cast<double>(lo));
  DT_OMP_FOR(reduction(+ : hist[:kHdrPercentileBins]))
  for (std::size_t i = 0; i < n; i++)
  {
    int b = static_cast<int>((static_cast<double>(src[i]) - static_cast<double>(lo)) * sbin);
    if (b < 0)
      b = 0;
    else if (b >= kHdrPercentileBins)
      b = kHdrPercentileBins - 1;
    hist[b]++;
  }

  // Walk the cumulative counts to the requested order statistics (nearest-rank),
  // reporting each bin's center as its representative value.
  const double bw = (static_cast<double>(hi) - static_cast<double>(lo)) / static_cast<double>(kHdrPercentileBins);
  const std::uint64_t rank_lo = static_cast<std::uint64_t>(low / 100.0 * static_cast<double>(n - 1) + 0.5);
  const std::uint64_t rank_hi = static_cast<std::uint64_t>(high / 100.0 * static_cast<double>(n - 1) + 0.5);
  std::uint64_t cum = 0;
  gboolean got_lo = FALSE;
  *p_low = lo;
  *p_high = hi;
  for (int b = 0; b < kHdrPercentileBins; b++)
  {
    const std::uint64_t next = cum + hist[b];
    const float center = static_cast<float>(static_cast<double>(lo) + (static_cast<double>(b) + 0.5) * bw);
    if (!got_lo && next > rank_lo)
    {
      *p_low = center;
      got_lo = TRUE;
    }
    if (next > rank_hi)
    {
      *p_high = center;
      break;
    }
    cum = next;
  }
}

/* ------------------------------------------------------------------------- *
 *  CFA mosaic -> CFA-free luma proxy at a configurable scale.
 *
 *  Luma comes from a stride-1 2x2 window: *any* 2x2 patch of a Bayer mosaic,
 *  whatever its phase, holds exactly one R, one B and two G, so 0.25*sum is
 *  CFA-free luma (R + 2G + B)/4 at full resolution -- no interpolation, no
 *  colour bias.  (An X-Trans 2x2 is not tile-aligned, but the average still
 *  strongly attenuates the CFA modulation.)  Area-averaging that down to the
 *  target size decouples the proxy scale from the 0.5x a 2x2-block reduction
 *  would be locked to; see DT_HDR_PROXY_SCALE.
 * ------------------------------------------------------------------------- */

// CFA-free luma at a stride-1 2x2 window with top-left (sx, sy), edge-clamped.
static inline float _luma2x2(const float *m, int width, int height, int sx, int sy)
{
  if (sx < 0)
    sx = 0;
  else if (sx > width - 2)
    sx = width - 2;
  if (sy < 0)
    sy = 0;
  else if (sy > height - 2)
    sy = height - 2;
  const float a = m[static_cast<std::size_t>(sy) * width + sx];
  const float b = m[static_cast<std::size_t>(sy) * width + sx + 1];
  const float c = m[static_cast<std::size_t>(sy + 1) * width + sx];
  const float d = m[static_cast<std::size_t>(sy + 1) * width + sx + 1];
  return 0.25f * (a + b + c + d);
}

static AlignedBuf<float> _build_proxy(const float *mosaic, int width, int height, double scale,
                                      int *pw_out, int *ph_out)
{
  if (width < 4 || height < 4)
    return nullptr;
  int pw = static_cast<int>(std::lround(static_cast<double>(width) * scale));
  int ph = static_cast<int>(std::lround(static_cast<double>(height) * scale));
  if (pw < 8 || ph < 8)
    return nullptr;
  if (pw > width)
    pw = width;
  if (ph > height)
    ph = height;

  AlignedBuf<float> proxy(dt_alloc_align_float(static_cast<std::size_t>(pw) * ph));
  if (!proxy)
    return nullptr;

  // Source mosaic pixels per proxy pixel (>= ~1; equals 1/scale).
  const double fx = static_cast<double>(width) / static_cast<double>(pw);
  const double fy = static_cast<double>(height) / static_cast<double>(ph);

  // DT_OMP_FOR is default(firstprivate), so the region takes a borrowed raw
  // pointer -- the owning AlignedBuf stays out here.
  float *const proxy_px = proxy.get();

  DT_OMP_FOR()
  for (int ty = 0; ty < ph; ty++)
  {
    int y0 = static_cast<int>(ty * fy);
    int y1 = static_cast<int>((ty + 1) * fy);
    if (y1 <= y0)
      y1 = y0 + 1;
    if (y1 > height - 1)
      y1 = height - 1;
    if (y0 > height - 2)
      y0 = height - 2;
    for (int tx = 0; tx < pw; tx++)
    {
      int x0 = static_cast<int>(tx * fx);
      int x1 = static_cast<int>((tx + 1) * fx);
      if (x1 <= x0)
        x1 = x0 + 1;
      if (x1 > width - 1)
        x1 = width - 1;
      if (x0 > width - 2)
        x0 = width - 2;
      float sum = 0.0f;
      int cnt = 0;
      for (int sy = y0; sy < y1; sy++)
        for (int sx = x0; sx < x1; sx++)
        {
          sum += _luma2x2(mosaic, width, height, sx, sy);
          cnt++;
        }
      proxy_px[static_cast<std::size_t>(ty) * pw + tx] = (cnt > 0) ? sum / static_cast<float>(cnt) : 0.0f;
    }
  }

  *pw_out = pw;
  *ph_out = ph;
  return proxy;
}

// Build the 8-bit SIFT proxy: percentile-stretch, display-gamma encode, scale to
// [0,255].  The gamma lifts shadow detail into SIFT's operating range (see
// DT_HDR_PROXY_FEATURE_GAMMA); CLAHE, if on, follows in detectDescribe().
// Stretch and gamma are fused into one parallel pass, with the gamma read from a
// LUT so the hot loop is a clamp plus a table lookup rather than a powf().
static AlignedBuf<std::uint8_t> _proxy_to_u8(const float *proxy_f, int pw, int ph, double gamma)
{
  constexpr int kHdrGammaLutSize{4096};
  const std::size_t n = static_cast<std::size_t>(pw) * ph;
  AlignedBuf<std::uint8_t> out(dt_alloc_align_uint8(n));
  if (!out)
    return nullptr;

  float p_low, p_high;
  _percentile_bounds(proxy_f, n, DT_HDR_PERCENTILE_LOW, DT_HDR_PERCENTILE_HIGH, &p_low, &p_high);
  const float scale = 1.0f / ((p_high - p_low) + static_cast<float>(DT_HDR_SMALL_EPS));

  // Precompute the display-gamma response over the normalized [0,1] domain.
  const float inv_gamma = 1.0f / static_cast<float>(gamma > 0.0 ? gamma : 1.0);
  std::uint8_t lut[kHdrGammaLutSize];
  for (int k = 0; k < kHdrGammaLutSize; k++)
  {
    const float u = static_cast<float>(k) / static_cast<float>(kHdrGammaLutSize - 1);
    const float v = std::pow(u, inv_gamma) * 255.0f;
    lut[k] = static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f));
  }

  std::uint8_t *const out_px = out.get();

  DT_OMP_FOR()
  for (std::size_t i = 0; i < n; i++)
  {
    float nv = (proxy_f[i] - p_low) * scale; // percentile stretch
    nv = std::clamp(nv, 0.0f, 1.0f);         // clip to [0,1]
    const int idx = static_cast<int>(nv * static_cast<float>(kHdrGammaLutSize - 1) + 0.5f);
    out_px[i] = lut[idx]; // gamma encode + quantize
  }
  return out;
}

/* ------------------------------------------------------------------------- *
 *  Reliability gates.
 * ------------------------------------------------------------------------- */

static gboolean _warp_is_sane(const Homography &H, int w, int h)
{
  const double diag = std::hypot(static_cast<double>(w), static_cast<double>(h));
  // Translation as a fraction of the image diagonal.
  if (std::hypot(H[2], H[5]) > DT_HDR_WARP_MAX_TRANSLATION_DIAG_FRAC * diag)
    return FALSE;
  // Scale = column norms of the upper-left 2x2.
  const double s1 = std::hypot(H[0], H[3]);
  const double s2 = std::hypot(H[1], H[4]);
  if (s1 < DT_HDR_WARP_SCALE_MIN || s1 > DT_HDR_WARP_SCALE_MAX)
    return FALSE;
  if (s2 < DT_HDR_WARP_SCALE_MIN || s2 > DT_HDR_WARP_SCALE_MAX)
    return FALSE;
  return TRUE;
}

// Per-corner displacement between two warps; fills the 4 distances (px) and
// returns the maximum.  Median is derived by the caller.
static double _corner_drift(const Homography &Ha, const Homography &Hb, int w, int h,
                            double dist[4])
{
  const double cx[4] = {0.0, static_cast<double>(w), 0.0, static_cast<double>(w)};
  const double cy[4] = {0.0, 0.0, static_cast<double>(h), static_cast<double>(h)};
  double maxd = 0.0;
  for (int i = 0; i < 4; i++)
  {
    const auto [ax, ay] = _h_apply(Ha, cx[i], cy[i]);
    const auto [bx, by] = _h_apply(Hb, cx[i], cy[i]);
    dist[i] = std::hypot(ax - bx, ay - by);
    if (dist[i] > maxd)
      maxd = dist[i];
  }
  return maxd;
}

/* ------------------------------------------------------------------------- *
 *  CFA-aware (same-color) resampling.
 *
 *  Fill output photosite (x, y) -- CFA color _fcol(y, x) -- by mapping it
 *  through the full-resolution homography and interpolating ONLY from moving-
 *  mosaic photosites of that same color, with a separable tent.  Always reading
 *  color c into a cell declared color c preserves the reference frame's mosaic
 *  phase exactly (both frames share the camera's CFA layout).
 * ------------------------------------------------------------------------- */

static inline float _sample_cfa_same_color(const float *mosaic, int w, int h,
                                           std::uint32_t filters, const std::uint8_t (*xtrans)[6],
                                           int c, double sx, double sy)
{
  // Clamp the sampling center to the valid region (clamp-to-edge border).
  if (sx < 0.0)
    sx = 0.0;
  if (sy < 0.0)
    sy = 0.0;
  if (sx > static_cast<double>(w - 1))
    sx = static_cast<double>(w - 1);
  if (sy > static_cast<double>(h - 1))
    sy = static_cast<double>(h - 1);

  const int x0 = static_cast<int>(std::floor(sx)) - DT_HDR_CFA_TENT_RADIUS;
  const int x1 = static_cast<int>(std::floor(sx)) + DT_HDR_CFA_TENT_RADIUS;
  const int y0 = static_cast<int>(std::floor(sy)) - DT_HDR_CFA_TENT_RADIUS;
  const int y1 = static_cast<int>(std::floor(sy)) + DT_HDR_CFA_TENT_RADIUS;
  const double inv_r = 1.0 / static_cast<double>(DT_HDR_CFA_TENT_RADIUS);

  double acc = 0.0;
  double wsum = 0.0;
  for (int j = y0; j <= y1; j++)
  {
    if (j < 0 || j >= h)
      continue;
    const double wy = 1.0 - std::fabs(sy - static_cast<double>(j)) * inv_r;
    if (wy <= 0.0)
      continue;
    for (int i = x0; i <= x1; i++)
    {
      if (i < 0 || i >= w)
        continue;
      if (_fcol(j, i, filters, xtrans) != c)
        continue;
      const double wx = 1.0 - std::fabs(sx - static_cast<double>(i)) * inv_r;
      if (wx <= 0.0)
        continue;
      const double wgt = wx * wy;
      acc += wgt * static_cast<double>(mosaic[static_cast<std::size_t>(j) * w + i]);
      wsum += wgt;
    }
  }
  if (wsum > 0.0)
    return static_cast<float>(acc / wsum);

  // Degenerate fallback: no same-color tap had positive weight, which happens
  // only at extreme X-Trans borders where the clamped window can hold no colour
  // c at all.  Search outward in rings for the nearest one rather than write a
  // different colour into a cell declared c (that would corrupt the phase).
  // The X-Trans period is 6, so this always resolves; bounded anyway.
  int bx = static_cast<int>(sx + 0.5);
  int by = static_cast<int>(sy + 0.5);
  if (bx < 0)
    bx = 0;
  else if (bx > w - 1)
    bx = w - 1;
  if (by < 0)
    by = 0;
  else if (by > h - 1)
    by = h - 1;
  for (int rad = 0; rad <= 8; rad++)
  {
    float best = 0.0f;
    double best_d2 = 0.0;
    gboolean found = FALSE;
    for (int j = by - rad; j <= by + rad; j++)
    {
      if (j < 0 || j >= h)
        continue;
      for (int i = bx - rad; i <= bx + rad; i++)
      {
        if (i < 0 || i >= w)
          continue;
        // ring only: skip interior cells already searched at a smaller radius
        if (rad > 0 && abs(i - bx) != rad && abs(j - by) != rad)
          continue;
        if (_fcol(j, i, filters, xtrans) != c)
          continue;
        const double d2 = (sx - i) * (sx - i) + (sy - j) * (sy - j);
        if (!found || d2 < best_d2)
        {
          best_d2 = d2;
          best = mosaic[static_cast<std::size_t>(j) * w + i];
          found = TRUE;
        }
      }
    }
    if (found)
      return best;
  }
  // Should be unreachable for a valid CFA; keep a defined result just in case.
  return mosaic[static_cast<std::size_t>(by) * w + bx];
}

// Bayer fast path for the same-color sampler.  A Bayer CFA is period-2, so the
// color-c photosites of a row sit on one column parity (R/B occupy alternate
// rows; green every row, flipping parity).  Taking that parity from the 2x2 cell
// and stepping by 2 visits exactly the same taps with no per-tap _fcol() test --
// bit-for-bit identical to the general sampler, at ~1/3 the inner iterations.
static inline float _sample_bayer_same_color(const float *mosaic, int w, int h,
                                             std::uint32_t filters, int c, double sx, double sy)
{
  if (sx < 0.0)
    sx = 0.0;
  if (sy < 0.0)
    sy = 0.0;
  if (sx > static_cast<double>(w - 1))
    sx = static_cast<double>(w - 1);
  if (sy > static_cast<double>(h - 1))
    sy = static_cast<double>(h - 1);

  const int x0 = static_cast<int>(std::floor(sx)) - DT_HDR_CFA_TENT_RADIUS;
  const int x1 = static_cast<int>(std::floor(sx)) + DT_HDR_CFA_TENT_RADIUS;
  const int y0 = static_cast<int>(std::floor(sy)) - DT_HDR_CFA_TENT_RADIUS;
  const int y1 = static_cast<int>(std::floor(sy)) + DT_HDR_CFA_TENT_RADIUS;
  const double inv_r = 1.0 / static_cast<double>(DT_HDR_CFA_TENT_RADIUS);

  double acc = 0.0;
  double wsum = 0.0;
  for (int j = y0; j <= y1; j++)
  {
    if (j < 0 || j >= h)
      continue;
    const double wy = 1.0 - std::fabs(sy - static_cast<double>(j)) * inv_r;
    if (wy <= 0.0)
      continue;

    // Column parity carrying color c in this row (& 1 is well-defined for the
    // negative window edge in two's complement: it yields the 0/1 parity).
    const int row_par = j & 1;
    int col_par;
    if (_fc(static_cast<std::size_t>(row_par), 0, filters) == c)
      col_par = 0;
    else if (_fc(static_cast<std::size_t>(row_par), 1, filters) == c)
      col_par = 1;
    else
      continue; // this row carries no color c (e.g. an R/B row of the wrong parity)

    int istart = x0;
    if ((istart & 1) != col_par)
      istart++;
    for (int i = istart; i <= x1; i += 2)
    {
      if (i < 0 || i >= w)
        continue;
      const double wx = 1.0 - std::fabs(sx - static_cast<double>(i)) * inv_r;
      if (wx <= 0.0)
        continue;
      const double wgt = wx * wy;
      acc += wgt * static_cast<double>(mosaic[static_cast<std::size_t>(j) * w + i]);
      wsum += wgt;
    }
  }
  if (wsum > 0.0)
    return static_cast<float>(acc / wsum);

  const int ix = static_cast<int>(sx + 0.5);
  const int iy = static_cast<int>(sy + 0.5);
  return mosaic[static_cast<std::size_t>(iy) * w + ix];
}

static void _warp_mosaic_cfa(const float *mosaic, float *out, int width, int height,
                             std::uint32_t filters, const std::uint8_t (*xtrans)[6], const Homography &H)
{
  // Hoist the CFA dispatch out of the per-pixel loop: Bayer uses the period-2
  // fast path, X-Trans the general same-color sampler.
  if (filters != 9u)
  {
    DT_OMP_FOR(collapse(2))
    for (int y = 0; y < height; y++)
      for (int x = 0; x < width; x++)
      {
        const int c = _fcol(y, x, filters, xtrans);
        const auto [sx, sy] = _h_apply(H, static_cast<double>(x), static_cast<double>(y));
        out[static_cast<std::size_t>(y) * width + x] =
            _sample_bayer_same_color(mosaic, width, height, filters, c, sx, sy);
      }
  }
  else
  {
    DT_OMP_FOR(collapse(2))
    for (int y = 0; y < height; y++)
      for (int x = 0; x < width; x++)
      {
        const int c = _fcol(y, x, filters, xtrans);
        const auto [sx, sy] = _h_apply(H, static_cast<double>(x), static_cast<double>(y));
        out[static_cast<std::size_t>(y) * width + x] =
            _sample_cfa_same_color(mosaic, width, height, filters, xtrans, c, sx, sy);
      }
  }
}

/* ------------------------------------------------------------------------- *
 *  Diagnostic logging (-d hdr_merge).
 * ------------------------------------------------------------------------- */

// Log a 3x3 homography (row-major) and its translation / rotation / scale /
// shear decomposition, so a merge run can be inspected with `-d hdr_merge`.
static void _log_warp(const char *title, const Homography &H)
{
  dt_print(DT_DEBUG_HDR_MERGE, "  %s:", title);
  dt_print(DT_DEBUG_HDR_MERGE, "    [ %.6f %.6f %.6f ]", H[0], H[1], H[2]);
  dt_print(DT_DEBUG_HDR_MERGE, "    [ %.6f %.6f %.6f ]", H[3], H[4], H[5]);
  dt_print(DT_DEBUG_HDR_MERGE, "    [ %.6f %.6f %.6f ]", H[6], H[7], H[8]);

  // Decompose the (homography-normalized) linear part via QR: L = Q*R, with Q a
  // pure rotation and R upper-triangular [[sx, shear], [0, sy]].
  double h22 = H[8];
  if (std::fabs(h22) < DT_HDR_SMALL_EPS)
    h22 = (h22 >= 0.0 ? DT_HDR_SMALL_EPS : -DT_HDR_SMALL_EPS);
  const double tx = H[2] / h22, ty = H[5] / h22;
  const double px = H[6] / h22, py = H[7] / h22;
  const double a = H[0] / h22, b = H[1] / h22, c = H[3] / h22, d = H[4] / h22;
  const double r11 = std::hypot(a, c);
  double rot_deg = 0.0, scale_x = r11, scale_y = 0.0, shear = 0.0;
  if (r11 > DT_HDR_SMALL_EPS)
  {
    const double r12 = (a * b + c * d) / r11;
    const double c2x = b - r12 * (a / r11);
    const double c2y = d - r12 * (c / r11);
    rot_deg = std::atan2(c, a) * (180.0 / M_PI); // atan2(Q[1,0], Q[0,0])
    scale_x = r11;
    scale_y = std::hypot(c2x, c2y);
    shear = r12;
  }
  dt_print(DT_DEBUG_HDR_MERGE, "  Decomposition:");
  dt_print(DT_DEBUG_HDR_MERGE, "    translation_x: %.2f", tx);
  dt_print(DT_DEBUG_HDR_MERGE, "    translation_y: %.2f", ty);
  dt_print(DT_DEBUG_HDR_MERGE, "    perspective_x: %.2f", px);
  dt_print(DT_DEBUG_HDR_MERGE, "    perspective_y: %.2f", py);
  dt_print(DT_DEBUG_HDR_MERGE, "    rotation_deg: %.2f deg", rot_deg);
  dt_print(DT_DEBUG_HDR_MERGE, "    scale_x: %.2f", scale_x);
  dt_print(DT_DEBUG_HDR_MERGE, "    scale_y: %.2f", scale_y);
  dt_print(DT_DEBUG_HDR_MERGE, "    shear: %.2f", shear);
}

// Log the SIFT+RANSAC initialization metrics block.
static void _log_feature_stats(const FeatureStats *s)
{
  dt_print(DT_DEBUG_HDR_MERGE, "  SIFT+RANSAC initialization metrics:");
  dt_print(DT_DEBUG_HDR_MERGE, "    keypoints (template/image): %d/%d", s->kp_template, s->kp_image);
  dt_print(DT_DEBUG_HDR_MERGE, "    good matches: %d", s->good_matches);
  if (s->good_matches > 0)
    dt_print(DT_DEBUG_HDR_MERGE, "    mutual-consistent matches: %d", s->good_matches);
  if (s->good_matches > 0)
    dt_print(DT_DEBUG_HDR_MERGE, "    RANSAC inliers: %d/%d (%.0f%%)",
             s->inliers, s->good_matches, 100.0 * s->inliers / s->good_matches);
  else
    dt_print(DT_DEBUG_HDR_MERGE, "    RANSAC inliers: %d", s->inliers);
  if (s->reproj_mean >= 0.0)
    dt_print(DT_DEBUG_HDR_MERGE, "    reproj mean/median/max: %.2f / %.2f / %.2f px",
             s->reproj_mean, s->reproj_median, s->reproj_max);
  else
    dt_print(DT_DEBUG_HDR_MERGE, "    reproj mean/median/max: n/a");
  dt_print(DT_DEBUG_HDR_MERGE, "    transform model: %s",
           s->used_translation ? "translation (cluster-degraded)"
                               : (s->used_affine ? "affine-fallback" : "homography"));
}

// Where the debug images would go: the env override wins over the preference.
// Returns an empty string when neither asks for them.
static std::string _debug_dir_candidate()
{
  if (const char *env = std::getenv("DT_HDR_DEBUG_IMAGE_DIR"); env && env[0])
    return env;

  const std::unique_ptr<gchar, GFreeDeleter> fallback(
      g_build_filename(g_get_tmp_dir(), "darktable_hdr_align_debug", nullptr));
  return fallback.get();
}

// Resolve the per-merge debug-image directory and make sure it exists.  Returns
// an empty string when debug images are off / the directory cannot be created.
static std::string _resolve_debug_dir(bool debug_images)
{
  const std::string debug_dir = _debug_dir_candidate();
  if (!debug_images || debug_dir.empty())
    return {};

  // Ensure the directory exists (the env override may name a path that does not
  // yet exist); otherwise we would silently fail to write every dump.
  if (g_mkdir_with_parents(debug_dir.c_str(), 0755) != 0)
  {
    dt_print(DT_DEBUG_HDR_MERGE,
             "  [hdr merge] could not create debug image directory '%s'; disabling debug images",
             debug_dir.c_str());
    return {};
  }
  dt_print(DT_DEBUG_HDR_MERGE, "  debug images -> %s", debug_dir.c_str());
  return debug_dir;
}

#endif // HAVE_OPENCV

/* ------------------------------------------------------------------------- *
 *  Public API.  Everything above needs OpenCV; without it these six functions
 *  are the entire translation unit, and they all decline to do anything.
 * ------------------------------------------------------------------------- */

void dt_hdr_alignment_default_params(dt_hdr_align_params_t *p)
{
  if (!p)
    return;
  p->proxy_scale = DT_HDR_PROXY_SCALE;
  p->feature_gamma = DT_HDR_PROXY_FEATURE_GAMMA;
  p->clahe_clip = DT_HDR_CLAHE_CLIP;
  p->sift_keypoints = DT_HDR_SIFT_KEYPOINTS;
  p->debug_images = 0;
}

dt_hdr_align_t *dt_hdr_alignment_new(const dt_hdr_align_params_t *params)
{
#ifndef HAVE_OPENCV
  // Without OpenCV there is no registration to do, so refuse to create the state
  // at all.  Every other entry point rejects a NULL state, which makes the whole
  // feature inert here without relying on callers to guard their call sites.
  std::ignore = params;
  return NULL;
#else
  // Callers are C, so no exception may escape this frame.  std::nothrow only
  // covers the allocation, so keep the constructor non-throwing too: a member
  // that allocates on default construction would throw straight past our C callers.
  static_assert(std::is_nothrow_default_constructible_v<dt_hdr_align_t>,
                "dt_hdr_align_t must be nothrow-default-constructible: "
                "dt_hdr_alignment_new() may not throw into its C callers");
  dt_hdr_align_t *a = new (std::nothrow) dt_hdr_align_t();
  if (!a)
    return NULL;
  dt_hdr_alignment_default_params(&a->params);
  if (params)
  {
    // Clamp user-supplied parameters to the ranges advertised by the matching
    // preferences (see the DT_HDR_*_MIN/MAX defines, kept in sync with
    // data/darktableconfig.xml.in).
    a->params.proxy_scale = std::clamp(params->proxy_scale,
                                       DT_HDR_PROXY_SCALE_MIN, DT_HDR_PROXY_SCALE_MAX);
    a->params.feature_gamma = std::clamp(params->feature_gamma,
                                         DT_HDR_FEATURE_GAMMA_MIN, DT_HDR_FEATURE_GAMMA_MAX);
    a->params.clahe_clip = std::clamp(params->clahe_clip,
                                      DT_HDR_CLAHE_CLIP_MIN, DT_HDR_CLAHE_CLIP_MAX);
    a->params.sift_keypoints = std::clamp(params->sift_keypoints,
                                          DT_HDR_SIFT_KEYPOINTS_MIN, DT_HDR_SIFT_KEYPOINTS_MAX);
    a->params.debug_images = params->debug_images ? 1 : 0;
  }
  try
  {
    a->debug_dir = _resolve_debug_dir(a->params.debug_images != 0);
  }
  catch (const std::exception &)
  {
    // out of memory building the path: carry on with debug images off
    a->debug_dir.clear();
  }
  return a;
#endif
}

void dt_hdr_alignment_free(dt_hdr_align_t *a)
{
  delete a;
}

gboolean dt_hdr_alignment_set_reference(dt_hdr_align_t *a,
                                        const float *mosaic,
                                        int width,
                                        int height,
                                        std::uint32_t filters,
                                        const std::uint8_t (*xtrans)[6])
{
  if (!a || !mosaic)
    return FALSE;

#ifndef HAVE_OPENCV
  // Unreachable (dt_hdr_alignment_new() returns NULL here), but keep the guard
  // so a future caller cannot pay for the full-resolution proxy build that
  // nothing would go on to use.
  std::ignore = width;
  std::ignore = height;
  std::ignore = filters;
  std::ignore = xtrans;
  return FALSE;
#else
  int pw = 0, ph = 0;
  AlignedBuf<float> proxy_f = _build_proxy(mosaic, width, height, a->params.proxy_scale, &pw, &ph);
  if (!proxy_f)
    return FALSE;

  // Build the 8-bit SIFT proxy; the float luma is no longer needed afterwards.
  AlignedBuf<std::uint8_t> proxy_u8 = _proxy_to_u8(proxy_f.get(), pw, ph, a->params.feature_gamma);
  proxy_f.reset(); // released here, not at scope exit: it is the larger buffer
  if (!proxy_u8)
    return FALSE;

  a->pw = pw;
  a->ph = ph;
  a->width = width;
  a->height = height;
  a->filters = filters;
  if (xtrans)
    memcpy(a->xtrans, xtrans, sizeof(a->xtrans));
  else
    memset(a->xtrans, 0, sizeof(a->xtrans));

  // Precompute (and cache) the reference's SIFT features once; every moving frame
  // then matches against this instead of re-detecting the reference.  On failure
  // ref_features stays null and frames fall through unaligned (graceful).
  a->ref_features = detectReference(wrapU8(proxy_u8.get(), pw, ph), a->params.sift_keypoints,
                                    a->params.clahe_clip);
  proxy_u8.reset();

  a->have_reference = TRUE;
  dt_print(DT_DEBUG_HDR_MERGE,
           "  reference proxy: %dx%d mosaic -> %dx%d luma (%s)",
           width, height, pw, ph, filters == 9u ? "X-Trans" : "Bayer");
  return TRUE;
#endif
}

int dt_hdr_alignment_probe_features(const float *mosaic, int width, int height)
{
#ifndef HAVE_OPENCV
  std::ignore = mosaic;
  std::ignore = width;
  std::ignore = height;
  return 0;
#else
  if (!mosaic)
    return 0;
  // The probe ranks frames before any state exists, so it uses the default gamma
  // (the ranking only needs to be self-consistent).  SIFT runs at the probe
  // resolution (longest side <= DT_HDR_AUTO_REFERENCE_PROBE_DIM), so build the
  // proxy directly at that scale instead of building the full DT_HDR_PROXY_SCALE
  // proxy and downscaling it -- for a large raw that avoids building (and
  // normalizing) a proxy several times bigger than the probe ever uses.
  const int max_side = (width > height) ? width : height;
  double probe_scale = static_cast<double>(DT_HDR_AUTO_REFERENCE_PROBE_DIM) / static_cast<double>(max_side > 0 ? max_side : 1);
  if (probe_scale > DT_HDR_PROXY_SCALE)
    probe_scale = DT_HDR_PROXY_SCALE;
  int pw = 0, ph = 0;
  AlignedBuf<float> proxy = _build_proxy(mosaic, width, height, probe_scale, &pw, &ph);
  if (!proxy)
    return 0;
  AlignedBuf<std::uint8_t> u8 = _proxy_to_u8(proxy.get(), pw, ph, DT_HDR_PROXY_FEATURE_GAMMA);
  proxy.reset();
  if (!u8)
    return 0;
  // Pass the probe dim through as a safety cap; the proxy is already at or below
  // it, so this normally does not resize again.
  return countFeatures(wrapU8(u8.get(), pw, ph), DT_HDR_AUTO_REFERENCE_PROBE_DIM);
#endif
}

gboolean dt_hdr_alignment_align_frame(dt_hdr_align_t *a,
                                      const float *mosaic,
                                      float *out,
                                      int width,
                                      int height,
                                      std::uint32_t filters,
                                      const std::uint8_t (*xtrans)[6],
                                      dt_hdr_align_result_t *info)
{
  if (info)
  {
    info->status = DT_HDR_ALIGN_IDENTITY;
    info->feature_inliers = 0;
    info->corner_drift = 0.0;
  }

  // Default behavior on any early-out: pass the frame through unchanged so the
  // caller accumulates the unaligned mosaic (current darktable behavior).
  if (!a || !a->have_reference || !mosaic || !out || out == mosaic)
  {
    if (out && mosaic && out != mosaic)
      memcpy(out, mosaic, static_cast<std::size_t>(width) * height * sizeof(float));
    return FALSE;
  }

  // Frames must share geometry with the reference (the merge already enforces
  // identical size / orientation upstream).
  if (width != a->width || height != a->height)
  {
    memcpy(out, mosaic, static_cast<std::size_t>(width) * height * sizeof(float));
    return FALSE;
  }

#ifndef HAVE_OPENCV
  // Built without OpenCV: registration is unavailable; accumulate unaligned.
  // (filters / xtrans are only consumed by the CFA-aware warp below.)
  std::ignore = filters;
  std::ignore = xtrans;
  if (info)
    info->status = DT_HDR_ALIGN_DISABLED;
  memcpy(out, mosaic, static_cast<std::size_t>(width) * height * sizeof(float));
  return FALSE;
#else
  const int pw = a->pw;
  const int ph = a->ph;

  // No reference feature cache (precompute failed / too few features): there is
  // nothing to align against, so pass the frame through unaligned.
  if (!a->ref_features)
  {
    memcpy(out, mosaic, static_cast<std::size_t>(width) * height * sizeof(float));
    return FALSE;
  }

  // Build the moving frame's proxies.
  int mpw = 0, mph = 0;
  AlignedBuf<float> mov_f = _build_proxy(mosaic, width, height, a->params.proxy_scale, &mpw, &mph);
  if (!mov_f || mpw != pw || mph != ph)
  {
    memcpy(out, mosaic, static_cast<std::size_t>(width) * height * sizeof(float));
    return FALSE;
  }
  AlignedBuf<std::uint8_t> mov_u8 = _proxy_to_u8(mov_f.get(), pw, ph, a->params.feature_gamma);
  mov_f.reset(); // float proxy is only an intermediate for the u8 proxy
  if (!mov_u8)
  {
    memcpy(out, mosaic, static_cast<std::size_t>(width) * height * sizeof(float));
    return FALSE;
  }

  // The per-merge debug-image directory (env override or preference) was
  // resolved once in dt_hdr_alignment_new(); number each aligned moving frame so
  // its visuals share an index.  Both are per-merge state -- no globals -- so
  // concurrent merges stay independent.
  const int frame_index = ++a->debug_frame;

  // --- Stage 1: feature initialization (SIFT + RANSAC homography) ----------
  Homography H_feature;
  H_feature = kIdentityH;
  FeatureStats fstats;
  const int feature_inliers = featureHomography(
      a->ref_features.get(), wrapU8(mov_u8.get(), pw, ph), a->params.sift_keypoints,
      a->params.clahe_clip, a->debug_dir, frame_index, H_feature, &fstats);
  const gboolean feature_ok = feature_inliers >= DT_HDR_FEATURE_MIN_INLIERS;
  // (the per-stage SIFT / match / inlier lines are logged by featureHomography;
  //  the structured metrics block is emitted below.)

  mov_u8.reset(); // done with the proxies before the full-resolution warp

  // --- Stage 2: choose and apply the warp ----------------------------------
  // The SIFT feature-init homography is the final warp when it is reliable and
  // geometrically sane; otherwise the frame is accumulated unaligned (never
  // worse than the legacy, alignment-free merge).
  Homography H_final;
  dt_hdr_align_status_t status;
  if (feature_ok && _warp_is_sane(H_feature, pw, ph))
  {
    H_final = H_feature;
    status = DT_HDR_ALIGN_OK;
  }
  else
  {
    H_final = kIdentityH;
    status = DT_HDR_ALIGN_IDENTITY;
  }

  // Rescale proxy -> full-resolution coordinates.
  H_final = _h_scaled_proxy_to_full(H_final, static_cast<double>(width) / pw,
                                    static_cast<double>(height) / ph);

  // Report the full-resolution corner motion vs. identity.
  double dist_id[4];
  const double corner_motion = _corner_drift(kIdentityH, H_final, width, height, dist_id);

  if (info)
  {
    info->status = status;
    info->feature_inliers = feature_inliers;
    info->corner_drift = corner_motion;
  }

  // --- Structured log ------------------------------------------------------
  // Warps are reported in full-resolution coordinates, i.e. as they are actually
  // applied to the mosaic, not in the proxy coordinates they were estimated in.
  _log_feature_stats(&fstats);
  if (status != DT_HDR_ALIGN_IDENTITY)
    _log_warp("Final warp matrix (SIFT feature-init)", H_final);
  else
    dt_print(DT_DEBUG_HDR_MERGE,
             "  feature init unreliable (%d inliers): frame left unaligned",
             feature_inliers);

  if (status == DT_HDR_ALIGN_IDENTITY || corner_motion < DT_HDR_NOOP_MAX_CORNER_PX)
  {
    // No reliable warp, or motion below the resample threshold: the caller's
    // original `mosaic` is already the correct data to accumulate (an identity /
    // sub-pixel warp we deliberately do not resample -- see DT_HDR_NOOP_MAX_...).
    // Return FALSE so the caller uses its own source buffer and skip the
    // full-frame copy into `out` (a static frame is otherwise a pure ~w*h*4-byte
    // memcpy per frame).  `info->status` still reports the OK/IDENTITY decision.
    return FALSE;
  }

  _warp_mosaic_cfa(mosaic, out, width, height, filters, xtrans, H_final);
  return TRUE;
#endif // HAVE_OPENCV
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
