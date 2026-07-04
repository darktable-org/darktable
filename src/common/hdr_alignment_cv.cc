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

/* OpenCV backend for HDR frame registration (the C-ABI seam declared in
 * hdr_alignment.h).  This is the only translation unit that touches OpenCV.
 * It concentrates the feature-based primitives that the HDR path of
 * align_and_blend.py relies on:
 *
 *   dt_hdr_cv_feature_homography  <-  estimate_initial_warp_feature_ransac()
 *   dt_hdr_cv_count_features      <-  _count_lowres_sift_features()
 *
 * All image data crosses the seam as plain pointers operating on the reduced-
 * resolution luma proxy built by hdr_alignment.c; the homography crosses as a
 * row-major double[9].  No OpenCV type appears in any public header.
 *
 * Built only when darktable is configured with OpenCV (HAVE_OPENCV).
 */

#include "common/hdr_alignment.h"

#ifdef HAVE_OPENCV

#include "common/darktable.h"   // dt_print / DT_DEBUG_HDR_MERGE

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/calib3d.hpp>
#include <opencv2/flann.hpp>            // KDTreeIndexParams / SearchParams

#include <algorithm>
#include <cmath>
#include <cstdio>                       // fopen/fwrite (Netpbm debug dump)
#include <cstdlib>                      // getenv
#include <string>
#include <vector>

namespace
{

// --- Constants ported from the HDR path of align_and_blend.py --------------
constexpr double kSiftContrastThreshold = 0.04;
constexpr double kSiftMinScalePx = 6.0;
constexpr double kRatioThreshold = 0.75;
constexpr int kMinFeatureKeypoints = 25;
constexpr int kMinGoodMatches = 25;
constexpr int kMaxMatchesForRansac = 1800;

constexpr double kRansacReprojThreshold = 2.5;
constexpr int kRansacMaxIters = 5000;
constexpr double kRansacConfidence = 0.995;
constexpr int kHomographyMinInliers = 50;
constexpr double kHomographyMinInlierRatio = 0.40;
constexpr int kSpatialGrid = 6;                      // FEATURE_SPATIAL_GRID_ROWS/COLS
constexpr int kClusterDegradeMaxCells = 2;           // INLIER_CLUSTER_DEGRADE_MAX_CELLS
constexpr double kClusterTranslationMaxMad = 5.0;    // INLIER_CLUSTER_TRANSLATION_MAX_MAD_PX
constexpr int kSiftSpatialBalanceTarget = 5000;      // SIFT_SPATIAL_BALANCE_TARGET

// Wrap a borrowed 8-bit buffer as a single-channel cv::Mat header (no copy).
cv::Mat wrapU8(const uint8_t *p, int w, int h)
{
  return cv::Mat(h, w, CV_8U, const_cast<uint8_t *>(p));
}

// Row-major CV_32F 3x3 -> double[9].
void fromMat3x3(const cv::Mat &m, double H[9])
{
  cv::Mat md;
  m.convertTo(md, CV_64F);
  for(int i = 0; i < 9; i++) H[i] = md.at<double>(i / 3, i % 3);
}

cv::Ptr<cv::SIFT> makeSift()
{
  return cv::SIFT::create(/*nfeatures=*/0, /*nOctaveLayers=*/3,
                          kSiftContrastThreshold, /*edgeThreshold=*/10.0,
                          /*sigma=*/1.6);
}

// Optional local-contrast enhancement (CLAHE) on the 8-bit feature image, with
// clip limit `clip` (<= 0 disables it).  CLAHE recovers features in extreme
// dynamic range, but -- as align_and_blend.py warns and defaults off
// (SIFT_USE_CLAHE = False) -- on *repetitive* textures it changes descriptor
// signatures between frames and manufactures false matches, so on a periodic
// scene it makes the matcher consense on a period-aliased shift.  We therefore
// leave it off by default and rely on the display-gamma proxy (which already
// mimics the prototype's tone-mapped JPEG input); it stays available as a knob
// for genuinely feature-starved / extreme-DR brackets.
void applyClahe(cv::Mat &img, double clip)
{
  if(clip <= 0.0) return;
  cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(clip, cv::Size(8, 8));
  clahe->apply(img, img);
}

// Indices of keypoints at or above the scale floor (SIFT_MIN_SCALE_PX): the
// smallest octaves are descriptor-ambiguous noise on low-contrast scenes.
// Returns an index list (rather than mutating in place) so the caller can apply
// the same selection to the parallel descriptor matrix via gatherKpDesc().
std::vector<int> scaleFloorKeep(const std::vector<cv::KeyPoint> &kps)
{
  std::vector<int> keep;
  keep.reserve(kps.size());
  for(int idx = 0; idx < (int)kps.size(); idx++)
    if(kSiftMinScalePx <= 0.0 || kps[idx].size >= kSiftMinScalePx) keep.push_back(idx);
  return keep;
}

// Reorder keypoints and (when present) their descriptor rows by `keep`, in
// keep-order, so the two stay aligned after any filter/balance step.  des may be
// empty (no descriptors computed yet), in which case only kps is reordered.
void gatherKpDesc(std::vector<cv::KeyPoint> &kps, cv::Mat &des, const std::vector<int> &keep)
{
  std::vector<cv::KeyPoint> nk;
  nk.reserve(keep.size());
  const bool has_desc = !des.empty() && des.rows == (int)kps.size();
  cv::Mat nd;
  if(has_desc) nd.create((int)keep.size(), des.cols, des.type());
  for(size_t k = 0; k < keep.size(); k++)
  {
    nk.push_back(kps[keep[k]]);
    if(has_desc) des.row(keep[k]).copyTo(nd.row((int)k));
  }
  kps.swap(nk);
  if(has_desc) des = nd;
}

// Spatially balance keypoints down to `target`, keeping the strongest (by SIFT
// response) in each cell of a frame-spanning grid.  Mirrors align_and_blend.py's
// "SIFT spatial balance" step.  Two effects matter for the HDR raw path:
//   (1) it caps a feature-dense reference (we have seen 12888 vs 5075 between the
//       two frames) so the descriptor matcher is not flooded with near-duplicate
//       candidates that pass the ratio test at the *wrong* instance of a periodic
//       structure -- the failure mode where RANSAC locks onto a translation that
//       is one structure-period short of the true (large) camera motion;
//   (2) it makes the template/image keypoint budgets symmetric, which is what
//       mutual-nearest-neighbour matching assumes.
// The grid has ~target cells (≈ 1 kept keypoint per cell), proportioned to the
// image aspect ratio, and we round-robin the per-cell response-sorted lists so
// coverage stays uniform if some cells are sparse.  Returns the kept indices
// (into kps) for gatherKpDesc() to apply to keypoints and descriptors together.
std::vector<int> spatialBalanceKeep(const std::vector<cv::KeyPoint> &kps, int width, int height, int target)
{
  std::vector<int> all(kps.size());
  for(int idx = 0; idx < (int)kps.size(); idx++) all[idx] = idx;
  if(target <= 0 || (int)kps.size() <= target) return all;
  const double aspect = (double)std::max(1, width) / (double)std::max(1, height);
  int cols = std::max(1, (int)std::lround(std::sqrt((double)target * aspect)));
  int rows = std::max(1, (int)std::lround((double)target / cols));
  std::vector<std::vector<int>> cells(rows * cols);
  for(int idx = 0; idx < (int)kps.size(); idx++)
  {
    int c = (int)(kps[idx].pt.x / std::max(1, width) * cols);
    int r = (int)(kps[idx].pt.y / std::max(1, height) * rows);
    c = std::min(std::max(c, 0), cols - 1);
    r = std::min(std::max(r, 0), rows - 1);
    cells[r * cols + c].push_back(idx);
  }
  for(auto &cell : cells)
    std::sort(cell.begin(), cell.end(),
              [&](int a, int b) { return kps[a].response > kps[b].response; });
  std::vector<int> out;
  out.reserve(target);
  size_t maxlen = 0;
  for(const auto &cell : cells) maxlen = std::max(maxlen, cell.size());
  for(size_t k = 0; k < maxlen && (int)out.size() < target; k++)
    for(const auto &cell : cells)
      if(k < cell.size())
      {
        out.push_back(cell[k]);
        if((int)out.size() >= target) break;
      }
  return out;
}

// Inlier reprojection-error stats (mean/median/max, in pixels) of warp H mapping
// template `src` -> image `dst`, restricted to RANSAC inliers.  Mirrors the
// "reproj mean/median/max" line of log_feature_init_stats().
void reprojStats(const std::vector<cv::Point2f> &src, const std::vector<cv::Point2f> &dst,
                 const cv::Mat &inliers, const cv::Mat &H, dt_hdr_cv_feature_stats_t *stats)
{
  if(!stats) return;
  stats->reproj_mean = stats->reproj_median = stats->reproj_max = -1.0;
  if(H.empty() || src.empty()) return;
  std::vector<cv::Point2f> proj;
  cv::perspectiveTransform(src, proj, H);
  std::vector<double> errs;
  errs.reserve(src.size());
  for(size_t k = 0; k < src.size(); k++)
  {
    if(!inliers.empty() && inliers.at<uchar>((int)k) == 0) continue;
    const double dx = (double)proj[k].x - dst[k].x;
    const double dy = (double)proj[k].y - dst[k].y;
    errs.push_back(std::sqrt(dx * dx + dy * dy));
  }
  if(errs.empty()) return;
  double sum = 0.0, mx = 0.0;
  for(double e : errs) { sum += e; mx = std::max(mx, e); }
  std::sort(errs.begin(), errs.end());
  stats->reproj_mean = sum / (double)errs.size();
  stats->reproj_median = errs[errs.size() / 2];
  stats->reproj_max = mx;
}

// Subsample matches to an even spatial distribution over a kSpatialGrid^2 grid of
// the image, so RANSAC is constrained by correspondences from the whole frame
// instead of one dense, well-exposed region.  Mirrors _spatially_uniform_subsample().
std::vector<cv::DMatch> spatialSubsample(const std::vector<cv::DMatch> &matches,
                                         const std::vector<cv::KeyPoint> &kp_img,
                                         int width, int height, int target)
{
  if((int)matches.size() <= target) return matches;
  const int gr = kSpatialGrid, gc = kSpatialGrid;
  std::vector<std::vector<int>> cells(gr * gc);
  for(int idx = 0; idx < (int)matches.size(); idx++)
  {
    const cv::Point2f &p = kp_img[matches[idx].queryIdx].pt;
    int r = (int)(p.y / std::max(1, height) * gr);
    int c = (int)(p.x / std::max(1, width) * gc);
    r = std::min(std::max(r, 0), gr - 1);
    c = std::min(std::max(c, 0), gc - 1);
    cells[r * gc + c].push_back(idx);
  }
  for(auto &cell : cells)
    std::sort(cell.begin(), cell.end(),
              [&](int a, int b) { return matches[a].distance < matches[b].distance; });
  std::vector<cv::DMatch> out;
  out.reserve(target);
  size_t maxlen = 0;
  for(const auto &cell : cells) maxlen = std::max(maxlen, cell.size());
  for(size_t k = 0; k < maxlen && (int)out.size() < target; k++)
    for(const auto &cell : cells)
    {
      if(k < cell.size())
      {
        out.push_back(matches[cell[k]]);
        if((int)out.size() >= target) break;
      }
    }
  return out;
}

// When RANSAC inliers cluster into <= kClusterDegradeMaxCells grid cells, an 8-DOF
// homography overfits scale/shear/perspective to a tiny region and extrapolates
// wildly.  If the inlier displacements are consistent (low MAD), refit as a pure
// translation from their median.  Mirrors the INLIER_CLUSTER_DEGRADE_* logic.
bool degradeClusteredToTranslation(const std::vector<cv::Point2f> &src,
                                   const std::vector<cv::Point2f> &dst,
                                   const cv::Mat &inliers, int width, int height,
                                   double H[9])
{
  const int gr = kSpatialGrid, gc = kSpatialGrid;
  bool occ[kSpatialGrid * kSpatialGrid] = { false };
  int n_cells = 0;
  std::vector<double> dxs, dys;
  for(size_t k = 0; k < src.size(); k++)
  {
    if(!inliers.empty() && inliers.at<uchar>((int)k) == 0) continue;
    int r = (int)(src[k].y / std::max(1, height) * gr);
    int c = (int)(src[k].x / std::max(1, width) * gc);
    r = std::min(std::max(r, 0), gr - 1);
    c = std::min(std::max(c, 0), gc - 1);
    if(!occ[r * gc + c]) { occ[r * gc + c] = true; n_cells++; }
    dxs.push_back((double)dst[k].x - src[k].x);
    dys.push_back((double)dst[k].y - src[k].y);
  }
  if((int)dxs.size() < 4 || n_cells > kClusterDegradeMaxCells) return false;

  std::vector<double> sx = dxs, sy = dys;
  std::sort(sx.begin(), sx.end());
  std::sort(sy.begin(), sy.end());
  const double tx = sx[sx.size() / 2], ty = sy[sy.size() / 2];
  std::vector<double> ax, ay;
  ax.reserve(dxs.size());
  ay.reserve(dys.size());
  for(double v : dxs) ax.push_back(std::fabs(v - tx));
  for(double v : dys) ay.push_back(std::fabs(v - ty));
  std::sort(ax.begin(), ax.end());
  std::sort(ay.begin(), ay.end());
  if(std::max(ax[ax.size() / 2], ay[ay.size() / 2]) > kClusterTranslationMaxMad) return false;

  H[0] = 1; H[1] = 0; H[2] = tx;
  H[3] = 0; H[4] = 1; H[5] = ty;
  H[6] = 0; H[7] = 0; H[8] = 1;
  return true;
}

/* ------------------------------------------------------------------------- *
 *  Optional debug-image dump.
 *
 *  When the caller passes a directory (resolved in the C layer from the
 *  preference or the DT_HDR_DEBUG_IMAGE_DIR override), each aligned (moving)
 *  frame writes a numbered set of Netpbm images (PGM/PPM) to it: the CLAHE'd
 *  SIFT input, the detected keypoints, and the colour-coded match visualisation
 *  (green = inlier, red = outlier).  Mirrors align_and_blend.py's "Saved feature
 *  debug visuals".  Entirely diagnostic: no effect unless a directory is given.
 * ------------------------------------------------------------------------- */

// Longest side of a dumped image; large proxies are scaled down to stay viewable
// and to keep the (uncompressed) files reasonable.
constexpr int kDebugMaxSide = 1600;

// Write `img` (8-bit 1- or 3-channel, or float/other normalised to [0,255]) as a
// Netpbm image -- PGM (P5) for grayscale, PPM (P6) for colour -- scaling it down
// if larger than kDebugMaxSide.  Netpbm needs no image library (keeps OpenCV to
// its minimal module set); the files open in any common image viewer.  Never
// throws into the caller.
void writeDebugImage(const char *dir, int frame, const char *name, const cv::Mat &img)
{
  try
  {
    if(img.empty()) return;
    cv::Mat u8;
    if(img.depth() == CV_8U)
      u8 = img;
    else
    {
      cv::normalize(img, u8, 0, 255, cv::NORM_MINMAX);
      u8.convertTo(u8, img.channels() == 3 ? CV_8UC3 : CV_8U);
    }
    const int side = std::max(u8.cols, u8.rows);
    if(side > kDebugMaxSide)
    {
      const double f = (double)kDebugMaxSide / side;
      cv::resize(u8, u8, cv::Size(), f, f, cv::INTER_AREA);
    }
    const int ch = u8.channels();
    char path[1024];
    snprintf(path, sizeof(path), "%s/hdr_frame%02d_%s.%s", dir, frame, name,
             ch == 3 ? "ppm" : "pgm");
    FILE *fp = fopen(path, "wb");
    if(!fp) return;
    fprintf(fp, "P%c\n%d %d\n255\n", ch == 3 ? '6' : '5', u8.cols, u8.rows);
    if(ch == 3)
    {
      // OpenCV is BGR; Netpbm wants RGB, so swap per pixel, one row at a time.
      std::vector<unsigned char> row((size_t)u8.cols * 3);
      for(int y = 0; y < u8.rows; y++)
      {
        const unsigned char *p = u8.ptr<unsigned char>(y);
        for(int x = 0; x < u8.cols; x++)
        {
          row[x * 3 + 0] = p[x * 3 + 2];
          row[x * 3 + 1] = p[x * 3 + 1];
          row[x * 3 + 2] = p[x * 3 + 0];
        }
        fwrite(row.data(), 1, row.size(), fp);
      }
    }
    else
    {
      for(int y = 0; y < u8.rows; y++)
        fwrite(u8.ptr<unsigned char>(y), 1, (size_t)u8.cols, fp);
    }
    fclose(fp);
    dt_print(DT_DEBUG_HDR_MERGE, "  debug image: %s", path);
  }
  catch(const cv::Exception &e)
  {
    dt_print(DT_DEBUG_HDR_MERGE, "  debug image '%s' failed: %s", name, e.what());
  }
}

// Single SIFT pass on one 8-bit proxy: detectAndCompute builds the Gaussian
// pyramid once (a separate detect + compute would build it twice), then the
// scale floor and spatial balance prune keypoints and their descriptor rows
// together via gatherKpDesc.  On the raw HDR proxy the detection count is modest,
// so the descriptors computed for later-pruned keypoints cost little next to the
// saved second pyramid; with very high detection counts (e.g. CLAHE on) that
// overhead grows but the single pyramid still dominates.  `out_img` receives the
// CLAHE'd image (the SIFT input) for the optional debug visuals.
void detectDescribe(const uint8_t *proxy, int width, int height, int balance_target,
                    double clahe_clip, std::vector<cv::KeyPoint> &kp, cv::Mat &des,
                    cv::Mat &out_img, int &raw, int &after_floor)
{
  out_img = wrapU8(proxy, width, height).clone();  // clone: CLAHE writes in place
  applyClahe(out_img, clahe_clip);
  cv::Ptr<cv::SIFT> sift = makeSift();
  sift->detectAndCompute(out_img, cv::noArray(), kp, des);
  raw = (int)kp.size();
  gatherKpDesc(kp, des, scaleFloorKeep(kp));
  after_floor = (int)kp.size();
  // Spatially balance to a common budget so a feature-dense frame cannot flood
  // the matcher with aliased candidates.  Mirrors align_and_blend.py.
  gatherKpDesc(kp, des, spatialBalanceKeep(kp, width, height, balance_target));
}

// Cached reference features: detected once per merge (dt_hdr_cv_features_create)
// and matched against by every moving frame, so the reference SIFT pass is not
// repeated for all N-1 moving frames.  Holds the CLAHE'd reference image too, for
// the optional debug visuals.
struct RefFeatures
{
  cv::Mat image;                 // CLAHE'd 8-bit reference proxy (debug visuals)
  std::vector<cv::KeyPoint> kp;  // balanced reference keypoints
  cv::Mat des;                   // matching descriptor rows
  int kp_raw = 0;                // detections before the scale floor
  int kp_floor = 0;              // kept after the scale floor (balance input)
};

} // namespace

extern "C" {

void *dt_hdr_cv_features_create(const uint8_t *proxy, int width, int height,
                                int balance_target, double clahe_clip)
{
  if(balance_target <= 0) balance_target = kSiftSpatialBalanceTarget;
  try
  {
    RefFeatures *f = new RefFeatures();
    detectDescribe(proxy, width, height, balance_target, clahe_clip,
                   f->kp, f->des, f->image, f->kp_raw, f->kp_floor);
    return f;
  }
  catch(const std::exception &e)
  {
    dt_print(DT_DEBUG_HDR_MERGE, "HDR ref feature precompute raised: %s", e.what());
    return nullptr;
  }
}

void dt_hdr_cv_features_destroy(void *features)
{
  delete static_cast<RefFeatures *>(features);
}

int dt_hdr_cv_feature_homography(const void *ref_features, const uint8_t *img,
                                 int width, int height, int balance_target,
                                 double clahe_clip, const char *debug_dir,
                                 int frame_index, double H[9],
                                 dt_hdr_cv_feature_stats_t *stats)
{
  if(balance_target <= 0) balance_target = kSiftSpatialBalanceTarget;
  // Identity by default.
  H[0] = 1; H[1] = 0; H[2] = 0; H[3] = 0; H[4] = 1; H[5] = 0; H[6] = 0; H[7] = 0; H[8] = 1;
  if(stats)
  {
    *stats = dt_hdr_cv_feature_stats_t{};
    stats->reproj_mean = stats->reproj_median = stats->reproj_max = -1.0;
  }

  const RefFeatures *ref = static_cast<const RefFeatures *>(ref_features);
  if(!ref || ref->kp.empty() || ref->des.empty()) return 0;

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
    detectDescribe(img, width, height, balance_target, clahe_clip,
                   kp_i, des_i, mov_img, ki_raw, ki_floor);

    if(stats) { stats->kp_template_raw = ref->kp_raw; stats->kp_image_raw = ki_raw; }
    dt_print(DT_DEBUG_HDR_MERGE,
             "SIFT scale floor: kept template %d/%d, image %d/%d keypoints with kp.size >= %.1fpx",
             ref->kp_floor, ref->kp_raw, ki_floor, ki_raw, kSiftMinScalePx);
    if((int)kp_i.size() < ki_floor)
      dt_print(DT_DEBUG_HDR_MERGE,
               "SIFT spatial balance: image %d->%d keypoints", ki_floor, (int)kp_i.size());

    if(stats) { stats->kp_template = (int)kp_t.size(); stats->kp_image = (int)kp_i.size(); }
    if(des_t.empty() || des_i.empty()
       || (int)kp_t.size() < kMinFeatureKeypoints || (int)kp_i.size() < kMinFeatureKeypoints)
    {
      dt_print(DT_DEBUG_HDR_MERGE,
               "SIFT init: insufficient features (need >= %d), using identity initial warp",
               kMinFeatureKeypoints);
      return 0;
    }

    // FLANN kNN match with Lowe's ratio test, in both directions, then keep
    // only the mutually-best correspondences (FEATURE_REQUIRE_MUTUAL_CONSISTENCY).
    // The two directions are independent, so they run in parallel sections; each
    // builds its own matcher (FlannBasedMatcher caches per-call train index state).
    // query = moving image (des_i), train = template (des_t): yields
    // image->template matches, i.e. queryIdx in image, trainIdx in template
    // (the Python convention).
    std::vector<std::vector<cv::DMatch>> knn_it, knn_ti;
    bool match_failed = false;
    std::string match_err;
    auto knn = [&](const cv::Mat &q, const cv::Mat &train,
                   std::vector<std::vector<cv::DMatch>> &out)
    {
      try
      {
        cv::FlannBasedMatcher matcher(cv::makePtr<cv::flann::KDTreeIndexParams>(5),
                                      cv::makePtr<cv::flann::SearchParams>(50));
        matcher.knnMatch(q, train, out, 2);
      }
      catch(const cv::Exception &e)
      {
#ifdef _OPENMP
#pragma omp critical(hdr_match_err)
#endif
        {
          match_failed = true;
          match_err = e.what();
        }
      }
    };

#ifdef _OPENMP
#pragma omp parallel sections
#endif
    {
#ifdef _OPENMP
#pragma omp section
#endif
      knn(des_i, des_t, knn_it);
#ifdef _OPENMP
#pragma omp section
#endif
      knn(des_t, des_i, knn_ti);
    }
    if(match_failed)
    {
      dt_print(DT_DEBUG_HDR_MERGE, "SIFT init match raised: %s", match_err.c_str());
      return 0;
    }

    // Reverse best (template kp -> image kp) after the ratio test.
    std::vector<int> rev_best(kp_t.size(), -1);
    for(const auto &m : knn_ti)
      if(m.size() >= 2 && m[0].distance < kRatioThreshold * m[1].distance)
        rev_best[m[0].queryIdx] = m[0].trainIdx;

    int ratio_count = 0;
    std::vector<cv::DMatch> good;
    good.reserve(knn_it.size());
    for(const auto &m : knn_it)
    {
      if(m.size() < 2) continue;
      if(m[0].distance >= kRatioThreshold * m[1].distance) continue;  // Lowe ratio
      ratio_count++;
      // Mutual-consistency: the template kp must point back to this image kp.
      if(rev_best[m[0].trainIdx] != m[0].queryIdx) continue;
      good.push_back(m[0]);
    }
    if(stats) { stats->ratio_matches = ratio_count; stats->good_matches = (int)good.size(); }
    if((int)good.size() > 0)
      dt_print(DT_DEBUG_HDR_MERGE,
               "SIFT init: %d/%d ratio-test matches are mutual-consistent",
               (int)good.size(), ratio_count);
    if((int)good.size() < kMinGoodMatches)
    {
      dt_print(DT_DEBUG_HDR_MERGE,
               "SIFT init: only %d good matches, using identity initial warp",
               (int)good.size());
      return 0;
    }

    // Subsample matches to an even spatial distribution, so RANSAC is constrained
    // by correspondences across the whole frame rather than one dense region.
    {
      const int before = (int)good.size();
      good = spatialSubsample(good, kp_i, width, height, kMaxMatchesForRansac);
      if((int)good.size() < before)
        dt_print(DT_DEBUG_HDR_MERGE,
                 "SIFT init: spatially-subsampled to %d matches (%dx%d grid) for RANSAC",
                 (int)good.size(), kSpatialGrid, kSpatialGrid);
    }

    // src = template coords, dst = image coords  =>  warp maps template->image,
    // consistent with WARP_INVERSE_MAP sampling.
    std::vector<cv::Point2f> src, dst;
    src.reserve(good.size());
    dst.reserve(good.size());
    for(const auto &m : good)
    {
      src.push_back(kp_t[m.trainIdx].pt);
      dst.push_back(kp_i[m.queryIdx].pt);
    }

    cv::Mat inliers;
    cv::Mat Hh = cv::findHomography(src, dst, cv::RANSAC, kRansacReprojThreshold, inliers,
                                    kRansacMaxIters, kRansacConfidence);
    int n_in = inliers.empty() ? 0 : cv::countNonZero(inliers);
    const int homog_inliers = n_in;
    const double homog_ratio = good.empty() ? 0.0 : (double)n_in / (double)good.size();
    const bool weak = Hh.empty() || n_in < kHomographyMinInliers
                      || homog_ratio < kHomographyMinInlierRatio;

    // Choose the model: homography, or estimateAffine2D fallback on weak support.
    cv::Mat chosen, chosen_inl;
    bool used_affine = false;
    if(weak)
    {
      cv::Mat ainl;
      cv::Mat A = cv::estimateAffine2D(src, dst, ainl, cv::RANSAC, kRansacReprojThreshold,
                                       kRansacMaxIters, kRansacConfidence);
      if(!A.empty())
      {
        chosen = cv::Mat::eye(3, 3, CV_64F);
        A.copyTo(chosen(cv::Rect(0, 0, 3, 2)));
        chosen_inl = ainl;
        used_affine = true;
        n_in = ainl.empty() ? 0 : cv::countNonZero(ainl);
        dt_print(DT_DEBUG_HDR_MERGE,
                 "SIFT init: weak homography support (%d/%d inliers, %.0f%%); using affine fallback",
                 homog_inliers, (int)good.size(), homog_ratio * 100.0);
      }
    }
    if(chosen.empty())
    {
      if(Hh.empty()) return 0;
      chosen = Hh;
      chosen_inl = inliers;
    }
    fromMat3x3(chosen, H);

    // Cluster-degradation: a homography (or affine) fit to inliers crammed into a
    // couple of grid cells overfits scale/shear and extrapolates wildly across the
    // frame; if the inlier displacements agree, refit as a robust translation.
    double Htrans[9];
    cv::Mat reproj_H = chosen;
    bool used_translation = false;
    if(degradeClusteredToTranslation(src, dst, chosen_inl, width, height, Htrans))
    {
      for(int k = 0; k < 9; k++) H[k] = Htrans[k];
      used_translation = true;
      reproj_H = (cv::Mat_<double>(3, 3) << 1, 0, Htrans[2], 0, 1, Htrans[5], 0, 0, 1);
      dt_print(DT_DEBUG_HDR_MERGE,
               "SIFT init: inliers cluster in <= %d cells; refitting as translation-only "
               "tx=%.2f ty=%.2f",
               kClusterDegradeMaxCells, Htrans[2], Htrans[5]);
    }

    if(stats)
    {
      stats->inliers = n_in;
      stats->used_affine = used_affine ? 1 : 0;
      stats->used_translation = used_translation ? 1 : 0;
    }
    reprojStats(src, dst, chosen_inl, reproj_H, stats);

    // Optional debug visuals: the (CLAHE'd) SIFT input, the kept keypoints, and
    // the colour-coded matches -- the quickest way to see *why* the match set
    // consensed where it did (e.g. a periodic structure matching one period off).
    // `debug_dir` / `frame_index` are supplied by the (per-merge) caller.
    if(debug_dir && debug_dir[0])
    {
      const char *dir = debug_dir;
      const int fr = frame_index;
      // The reference (template) visuals are identical for every moving frame,
      // so write them once (on the first aligned frame) instead of per frame.
      if(frame_index <= 1)
      {
        writeDebugImage(dir, fr, "1_template_sift_input", t);
        cv::Mat kt_vis;
        cv::drawKeypoints(t, kp_t, kt_vis, cv::Scalar::all(-1),
                          cv::DrawMatchesFlags::DRAW_RICH_KEYPOINTS);
        writeDebugImage(dir, fr, "3_template_keypoints", kt_vis);
      }
      writeDebugImage(dir, fr, "2_image_sift_input", mov_img);
      cv::Mat ki_vis;
      cv::drawKeypoints(mov_img, kp_i, ki_vis, cv::Scalar::all(-1),
                        cv::DrawMatchesFlags::DRAW_RICH_KEYPOINTS);
      writeDebugImage(dir, fr, "4_image_keypoints", ki_vis);
      // All matches, colour-coded: red = RANSAC outlier, green = inlier.  `good`
      // matches have queryIdx -> kp_i (image), trainIdx -> kp_t (template), and
      // chosen_inl is the per-match inlier mask over `good`.  Draw the outliers
      // first, then the inliers over the top so the consensus stands out.
      std::vector<char> inlier_mask(good.size(), 0), outlier_mask(good.size(), 1);
      for(size_t k = 0; k < good.size(); k++)
      {
        const bool in = !chosen_inl.empty() && chosen_inl.at<uchar>((int)k);
        inlier_mask[k] = in ? 1 : 0;
        outlier_mask[k] = in ? 0 : 1;
      }
      const cv::Scalar red(0, 0, 255), green(0, 255, 0);   // BGR
      cv::Mat match_vis;
      cv::drawMatches(mov_img, kp_i, t, kp_t, good, match_vis, red, red, outlier_mask,
                      cv::DrawMatchesFlags::NOT_DRAW_SINGLE_POINTS);
      cv::drawMatches(mov_img, kp_i, t, kp_t, good, match_vis, green, green, inlier_mask,
                      cv::DrawMatchesFlags::DRAW_OVER_OUTIMG
                          | cv::DrawMatchesFlags::NOT_DRAW_SINGLE_POINTS);
      writeDebugImage(dir, fr, "5_all_matches_green_inlier_red_outlier", match_vis);
    }
    return n_in;
  }
  catch(const cv::Exception &e)
  {
    dt_print(DT_DEBUG_HDR_MERGE, "SIFT init raised: %s", e.what());
    return 0;
  }
}

int dt_hdr_cv_count_features(const uint8_t *luma, int width, int height, int probe_dim)
{
  try
  {
    cv::Mat in = wrapU8(luma, width, height);
    cv::Mat resized;
    const int max_side = std::max(width, height);
    if(probe_dim > 0 && max_side > probe_dim)
    {
      const double s = (double)probe_dim / (double)max_side;
      cv::resize(in, resized, cv::Size(), s, s, cv::INTER_AREA);
    }
    // The probe ranks the gamma-encoded proxies as detection sees them (CLAHE
    // off by default); the ranking only needs to be self-consistent.  Detect
    // directly on the wrapped (or resized) buffer -- SIFT does not modify its
    // input, so no defensive copy is needed.
    const cv::Mat &probe = resized.empty() ? in : resized;

    cv::Ptr<cv::SIFT> sift = makeSift();
    std::vector<cv::KeyPoint> kps;
    sift->detect(probe, kps);
    return (int)kps.size();
  }
  catch(const cv::Exception &)
  {
    return 0;
  }
}

} // extern "C"

#endif // HAVE_OPENCV
