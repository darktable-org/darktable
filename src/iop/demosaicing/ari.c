/*
    ARI (Adaptive Residual Interpolation) Custom Demosaic

    Based on Monno et al. 2017 "Adaptive Residual Interpolation for Color
    and Multispectral Image Demosaicking" with customizations:
    - Three candidate generators: MLRI, RI, GF-RI
    - Per-pixel adaptive selection based on residual variance
    - Automatic noise estimation for adaptive threshold

    References:
    - Kiku et al. 2013 "Residual Interpolation for Color Image Demosaicking"
    - Kiku et al. 2014 "Minimized-Laplacian Residual Interpolation..."
    - Monno et al. 2017 "Adaptive Residual Interpolation..."
    - Hamilton & Adams 1997 "Adaptive color plan interpolation..."
    - He et al. 2013 "Guided Image Filtering"
*/

#define ARI_BORDER     10
#define ARI_GF_RADIUS   2
#define ARI_SEL_RADIUS  2

/* ===== float comparator for qsort ===== */
static int _ari_compare_float(const void *a, const void *b)
{
  const float fa = *(const float *)a, fb = *(const float *)b;
  return (fa > fb) - (fa < fb);
}

/* ===== Noise estimation from dark pixels ===== */
static float _ari_estimate_noise(const float *const restrict in,
                                 const int width, const int height,
                                 const uint32_t filters)
{
  /* Find 10th percentile intensity */
  float vmin = FLT_MAX, vmax = 0.0f;
  for(int y = 0; y < height; y += 8)
    for(int x = 0; x < width; x += 8)
    {
      const float v = in[(size_t)y * width + x];
      if(v > 1e-8f && v < vmin) vmin = v;
      if(v > vmax) vmax = v;
    }
  if(vmax <= vmin) return 0.001f;

  /* Histogram for percentile */
#define ARI_NOISE_BINS 256
  int hist[ARI_NOISE_BINS];
  memset(hist, 0, sizeof(hist));
  const float hscale = (float)(ARI_NOISE_BINS - 1) / fmaxf(vmax - vmin, 1e-10f);
  int total = 0;

  for(int y = 0; y < height; y += 4)
    for(int x = 0; x < width; x += 4)
    {
      const float v = in[(size_t)y * width + x];
      if(v > 0.0f)
      {
        hist[CLAMP((int)((v - vmin) * hscale), 0, ARI_NOISE_BINS - 1)]++;
        total++;
      }
    }

  const int target = total / 10;
  int cumsum = 0;
  float p10 = vmin;
  for(int b = 0; b < ARI_NOISE_BINS; b++)
  {
    cumsum += hist[b];
    if(cumsum >= target) { p10 = vmin + (float)b / hscale; break; }
  }
#undef ARI_NOISE_BINS

  /* Collect squared same-color differences in dark region */
#define ARI_MAX_NOISE_SAMPLES 50000
  float *dsq = dt_alloc_align_float(ARI_MAX_NOISE_SAMPLES);
  if(!dsq) return 0.001f;

  int ns = 0;
  for(int y = 2; y < height - 2 && ns < ARI_MAX_NOISE_SAMPLES; y++)
    for(int x = 2; x < width - 4 && ns < ARI_MAX_NOISE_SAMPLES; x += 2)
    {
      const float v1 = in[(size_t)y * width + x];
      const float v2 = in[(size_t)y * width + x + 2];
      if(v1 > 0.0f && v1 < p10 && v2 > 0.0f && v2 < p10)
      {
        const float d = v2 - v1;
        dsq[ns++] = d * d;
      }
    }

  if(ns < 100) { dt_free_align(dsq); return 0.001f; }

  qsort(dsq, ns, sizeof(float), _ari_compare_float);
  const float sigma = sqrtf(dsq[ns / 2] / 0.9098f);
  dt_free_align(dsq);
  return fmaxf(sigma, 1e-6f);
#undef ARI_MAX_NOISE_SAMPLES
}

/* ===== Box filter via integral image ===== */
static void _ari_box_filter(const float *const restrict src,
                            float *const restrict dst,
                            const int width, const int height,
                            const int radius)
{
  double *integral = calloc((size_t)(width + 1) * (height + 1), sizeof(double));
  if(!integral) { memcpy(dst, src, sizeof(float) * (size_t)width * height); return; }

  /* Build integral image */
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
      integral[(size_t)(y + 1) * (width + 1) + (x + 1)] =
          (double)src[(size_t)y * width + x]
        + integral[(size_t)y * (width + 1) + (x + 1)]
        + integral[(size_t)(y + 1) * (width + 1) + x]
        - integral[(size_t)y * (width + 1) + x];

  /* Query */
  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
  {
    const int y0 = MAX(0, y - radius), y1 = MIN(height - 1, y + radius);
    for(int x = 0; x < width; x++)
    {
      const int x0 = MAX(0, x - radius), x1 = MIN(width - 1, x + radius);
      const double sum = integral[(size_t)(y1 + 1) * (width + 1) + (x1 + 1)]
                       - integral[(size_t)y0 * (width + 1) + (x1 + 1)]
                       - integral[(size_t)(y1 + 1) * (width + 1) + x0]
                       + integral[(size_t)y0 * (width + 1) + x0];
      const int cnt = (y1 - y0 + 1) * (x1 - x0 + 1);
      dst[(size_t)y * width + x] = (float)(sum / cnt);
    }
  }
  free(integral);
}

/* ===== Guided filter -- single channel (He et al. 2013) ===== */
static void _ari_guided_filter_ch(const float *const restrict input,
                                  const float *const restrict guide,
                                  float *const restrict output,
                                  const int width, const int height,
                                  const int radius, const float eps)
{
  const size_t npix = (size_t)width * height;

  float *mean_I  = dt_alloc_align_float(npix);
  float *mean_p  = dt_alloc_align_float(npix);
  float *mean_Ip = dt_alloc_align_float(npix);
  float *mean_II = dt_alloc_align_float(npix);
  float *aa      = dt_alloc_align_float(npix);
  float *bb      = dt_alloc_align_float(npix);
  float *mean_a  = dt_alloc_align_float(npix);
  float *mean_b  = dt_alloc_align_float(npix);
  float *tmp1    = dt_alloc_align_float(npix);
  float *tmp2    = dt_alloc_align_float(npix);

  if(!mean_I || !mean_p || !mean_Ip || !mean_II || !aa || !bb
     || !mean_a || !mean_b || !tmp1 || !tmp2)
  {
    memcpy(output, input, sizeof(float) * npix);
    goto cleanup_gf;
  }

  /* Products */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    tmp1[i] = guide[i] * input[i];
    tmp2[i] = guide[i] * guide[i];
  }

  /* Box-filtered means */
  _ari_box_filter(guide, mean_I, width, height, radius);
  _ari_box_filter(input, mean_p, width, height, radius);
  _ari_box_filter(tmp1,  mean_Ip, width, height, radius);
  _ari_box_filter(tmp2,  mean_II, width, height, radius);

  /* a, b coefficients */
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
  {
    const float cov = mean_Ip[i] - mean_I[i] * mean_p[i];
    const float var = mean_II[i] - mean_I[i] * mean_I[i];
    aa[i] = cov / (var + eps);
    bb[i] = mean_p[i] - aa[i] * mean_I[i];
  }

  /* Final: mean(a) * guide + mean(b) */
  _ari_box_filter(aa, mean_a, width, height, radius);
  _ari_box_filter(bb, mean_b, width, height, radius);

  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++)
    output[i] = mean_a[i] * guide[i] + mean_b[i];

cleanup_gf:
  dt_free_align(mean_I);  dt_free_align(mean_p);
  dt_free_align(mean_Ip); dt_free_align(mean_II);
  dt_free_align(aa);      dt_free_align(bb);
  dt_free_align(mean_a);  dt_free_align(mean_b);
  dt_free_align(tmp1);    dt_free_align(tmp2);
}

/* ===== Green interpolation: Hamilton-Adams (for RI) ===== */
static void _ari_green_ha(float *const restrict green,
                          const float *const restrict in,
                          const int width, const int height,
                          const uint32_t filters)
{
  /* Copy known G values, zero non-G */
  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      const size_t pos = (size_t)y * width + x;
      green[pos] = (FC(y, x, filters) == 1) ? in[pos] : 0.0f;
    }

  /* Interpolate G at R/B positions (HA: gradient + Laplacian correction) */
  DT_OMP_FOR()
  for(int y = 2; y < height - 2; y++)
    for(int x = 2; x < width - 2; x++)
    {
      if(FC(y, x, filters) == 1) continue;
      const size_t p = (size_t)y * width + x;

      const float dH = fabsf(in[p - 2] - in[p])
                      + fabsf(in[p - 1] - in[p + 1]);
      const float dV = fabsf(in[p - 2 * width] - in[p])
                      + fabsf(in[p - width] - in[p + width]);

      const float Gh = (in[p - 1] + in[p + 1]) * 0.5f
                     + (2.0f * in[p] - in[p - 2] - in[p + 2]) * 0.25f;
      const float Gv = (in[p - width] + in[p + width]) * 0.5f
                     + (2.0f * in[p] - in[p - 2 * width] - in[p + 2 * width]) * 0.25f;

      green[p] = (dH < dV) ? Gh : (dV < dH) ? Gv : (Gh + Gv) * 0.5f;
    }
}

/* ===== Green interpolation: MLRI (Laplacian energy direction) ===== */
static void _ari_green_mlri(float *const restrict green,
                            const float *const restrict in,
                            const int width, const int height,
                            const uint32_t filters)
{
  /* Copy known G values */
  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      const size_t p = (size_t)y * width + x;
      green[p] = (FC(y, x, filters) == 1) ? in[p] : 0.0f;
    }

  /* Border region: HA (needs only 2-pixel border) */
  for(int y = 2; y < height - 2; y++)
    for(int x = 2; x < width - 2; x++)
    {
      if(y >= 4 && y < height - 4 && x >= 4 && x < width - 4) continue;
      if(FC(y, x, filters) == 1) continue;
      const size_t p = (size_t)y * width + x;
      const float dH = fabsf(in[p - 2] - in[p]) + fabsf(in[p - 1] - in[p + 1]);
      const float dV = fabsf(in[p - 2 * width] - in[p]) + fabsf(in[p - width] - in[p + width]);
      const float Gh = (in[p - 1] + in[p + 1]) * 0.5f + (2.0f * in[p] - in[p - 2] - in[p + 2]) * 0.25f;
      const float Gv = (in[p - width] + in[p + width]) * 0.5f + (2.0f * in[p] - in[p - 2 * width] - in[p + 2 * width]) * 0.25f;
      green[p] = (dH < dV) ? Gh : (dV < dH) ? Gv : (Gh + Gv) * 0.5f;
    }

  /* Interior: MLRI (5x5 Laplacian energy direction selection) */
  DT_OMP_FOR()
  for(int y = 4; y < height - 4; y++)
    for(int x = 4; x < width - 4; x++)
    {
      if(FC(y, x, filters) == 1) continue;
      const size_t p = (size_t)y * width + x;
      const int w = width;

      /* Directional green estimates with Laplacian correction */
      const float Gh = (in[p - 1] + in[p + 1]) * 0.5f
                     + (2.0f * in[p] - in[p - 2] - in[p + 2]) * 0.25f;
      const float Gv = (in[p - w] + in[p + w]) * 0.5f
                     + (2.0f * in[p] - in[p - 2*w] - in[p + 2*w]) * 0.25f;

      /* Horizontal residual at center and same-color neighbors */
      const float rh   = in[p] - Gh;
      const float rh_l = in[p - 2] - (in[p - 3] + in[p - 1]) * 0.5f;
      const float rh_r = in[p + 2] - (in[p + 1] + in[p + 3]) * 0.5f;

      /* Vertical residual */
      const float rv   = in[p] - Gv;
      const float rv_u = in[p - 2*w] - (in[p - 3*w] + in[p - w]) * 0.5f;
      const float rv_d = in[p + 2*w] - (in[p + w] + in[p + 3*w]) * 0.5f;

      /* Laplacian energy of residuals + 2nd difference of raw */
      const float Lh = fabsf(rh_l - 2.0f * rh + rh_r)
                     + fabsf(in[p - 2] - 2.0f * in[p] + in[p + 2]);
      const float Lv = fabsf(rv_u - 2.0f * rv + rv_d)
                     + fabsf(in[p - 2*w] - 2.0f * in[p] + in[p + 2*w]);

      green[p] = (Lh < Lv) ? Gh : (Lv < Lh) ? Gv : (Gh + Gv) * 0.5f;
    }
}

/* ===== Bilinear color-difference interpolation ===== */
static void _ari_interpolate_cd(float *const restrict diff_r,
                                float *const restrict diff_b,
                                const float *const restrict in,
                                const float *const restrict green,
                                const int width, const int height,
                                const uint32_t filters)
{
  const size_t npix = (size_t)width * height;
  memset(diff_r, 0, sizeof(float) * npix);
  memset(diff_b, 0, sizeof(float) * npix);

  /* Compute known color differences */
  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      const size_t p = (size_t)y * width + x;
      const int c = FC(y, x, filters);
      if(c == 0)      diff_r[p] = in[p] - green[p];
      else if(c == 2) diff_b[p] = in[p] - green[p];
    }

  /* Interpolate R-G at non-Red positions
   * All interpolation uses only KNOWN Red positions:
   *   Gr (Green in Red row): horizontal R neighbors
   *   Gb (Green in Blue row): vertical R neighbors
   *   B: diagonal R neighbors */
  DT_OMP_FOR()
  for(int y = 2; y < height - 2; y++)
    for(int x = 2; x < width - 2; x++)
    {
      const size_t p = (size_t)y * width + x;
      const int c = FC(y, x, filters);
      if(c == 0) continue; /* R: known */

      if(c == 1) /* Green */
      {
        if(FC(y, x - 1, filters) == 0) /* Gr: R left/right */
          diff_r[p] = (diff_r[p - 1] + diff_r[p + 1]) * 0.5f;
        else /* Gb: R above/below */
          diff_r[p] = (diff_r[p - width] + diff_r[p + width]) * 0.5f;
      }
      else /* Blue: diagonal R */
      {
        diff_r[p] = (diff_r[p - width - 1] + diff_r[p - width + 1]
                   + diff_r[p + width - 1] + diff_r[p + width + 1]) * 0.25f;
      }
    }

  /* Interpolate B-G at non-Blue positions */
  DT_OMP_FOR()
  for(int y = 2; y < height - 2; y++)
    for(int x = 2; x < width - 2; x++)
    {
      const size_t p = (size_t)y * width + x;
      const int c = FC(y, x, filters);
      if(c == 2) continue; /* B: known */

      if(c == 1) /* Green */
      {
        if(FC(y, x - 1, filters) == 2) /* Gb: B left/right */
          diff_b[p] = (diff_b[p - 1] + diff_b[p + 1]) * 0.5f;
        else /* Gr: B above/below */
          diff_b[p] = (diff_b[p - width] + diff_b[p + width]) * 0.5f;
      }
      else /* Red: diagonal B */
      {
        diff_b[p] = (diff_b[p - width - 1] + diff_b[p - width + 1]
                   + diff_b[p + width - 1] + diff_b[p + width + 1]) * 0.25f;
      }
    }
}

/* ===== Write RGB output from green + color differences ===== */
static void _ari_write_rgb(float *const restrict out,
                           const float *const restrict green,
                           const float *const restrict diff_r,
                           const float *const restrict diff_b,
                           const int width, const int height)
{
  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      const size_t p = (size_t)y * width + x;
      const size_t o = 4 * p;
      out[o + 0] = fmaxf(green[p] + diff_r[p], 0.0f);
      out[o + 1] = fmaxf(green[p], 0.0f);
      out[o + 2] = fmaxf(green[p] + diff_b[p], 0.0f);
      out[o + 3] = 0.0f;
    }
}

/* ===== Per-pixel adaptive selection ===== */
/* Selects per-pixel among n_candidates based on local color-difference variance.
 * Candidate arrays: greens[i], diff_rs[i], diff_bs[i] for i in [0, n_candidates).
 * Candidates are ordered sharpest-first: MLRI, RI, GF-RI.
 * When variance difference < noise_thr^2, prefer the smoother (later) candidate. */
static void _ari_adaptive_select(float *const restrict out,
                                 float *const *const greens,
                                 float *const *const diff_rs,
                                 float *const *const diff_bs,
                                 const int n_candidates,
                                 const int width, const int height,
                                 const float noise_thr)
{
  const int r = ARI_SEL_RADIUS;
  const float thr_sq = noise_thr * noise_thr * 4.0f;

  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
    for(int x = 0; x < width; x++)
    {
      const size_t p = (size_t)y * width + x;

      /* Compute local variance of color differences for each candidate */
      float best_var = FLT_MAX;
      int best = n_candidates - 1; /* default: smoothest */

      for(int ci = 0; ci < n_candidates; ci++)
      {
        const float *dr = diff_rs[ci];
        const float *db = diff_bs[ci];

        /* Local variance in (2r+1)x(2r+1) window */
        float sum_r = 0, sum_r2 = 0, sum_b = 0, sum_b2 = 0;
        int cnt = 0;
        const int y0 = MAX(0, y - r), y1 = MIN(height - 1, y + r);
        const int x0 = MAX(0, x - r), x1 = MIN(width - 1, x + r);
        for(int yy = y0; yy <= y1; yy++)
          for(int xx = x0; xx <= x1; xx++)
          {
            const size_t pp = (size_t)yy * width + xx;
            sum_r += dr[pp]; sum_r2 += dr[pp] * dr[pp];
            sum_b += db[pp]; sum_b2 += db[pp] * db[pp];
            cnt++;
          }
        const float inv = 1.0f / (float)cnt;
        const float var_r = sum_r2 * inv - (sum_r * inv) * (sum_r * inv);
        const float var_b = sum_b2 * inv - (sum_b * inv) * (sum_b * inv);
        const float var = fmaxf(var_r, 0.0f) + fmaxf(var_b, 0.0f);

        if(var < best_var - thr_sq)
        {
          best_var = var;
          best = ci;
        }
      }

      /* Write selected candidate to output */
      const size_t o = 4 * p;
      out[o + 0] = fmaxf(greens[best][p] + diff_rs[best][p], 0.0f);
      out[o + 1] = fmaxf(greens[best][p], 0.0f);
      out[o + 2] = fmaxf(greens[best][p] + diff_bs[best][p], 0.0f);
      out[o + 3] = 0.0f;
    }
}

/* ===== Main entry point ===== */
static void ari_demosaic(float *const restrict out,
                         const float *const restrict in,
                         const int width, const int height,
                         const uint32_t filters,
                         const int quality)
{
  const size_t npix = (size_t)width * height;

  /* Clamp negative Bayer values (can occur after rawprepare) so all
   * subsequent gradient and residual computations see consistent data. */
  float *cfa = dt_alloc_align_float(npix);
  if(!cfa) { memset(out, 0, sizeof(float) * npix * 4); return; }
  DT_OMP_FOR()
  for(size_t i = 0; i < npix; i++) cfa[i] = fmaxf(in[i], 0.0f);

  /* === Quality 1: MLRI only, no selection === */
  if(quality <= 1)
  {
    float *green = dt_alloc_align_float(npix);
    float *dr = dt_alloc_align_float(npix);
    float *db = dt_alloc_align_float(npix);
    if(!green || !dr || !db)
    {
      dt_free_align(green); dt_free_align(dr); dt_free_align(db);
      dt_free_align(cfa);
      memset(out, 0, sizeof(float) * npix * 4);
      return;
    }
    _ari_green_mlri(green, cfa, width, height, filters);
    _ari_interpolate_cd(dr, db, cfa, green, width, height, filters);
    _ari_write_rgb(out, green, dr, db, width, height);
    dt_free_align(green); dt_free_align(dr); dt_free_align(db);
    dt_free_align(cfa);
    return;
  }

  /* === Quality 2-3: adaptive selection === */
  const float noise_sigma = _ari_estimate_noise(cfa, width, height, filters);
  const float gf_eps = noise_sigma * noise_sigma * 4.0f;

  /* Candidate 0: MLRI (sharpest) */
  float *green_mlri = dt_alloc_align_float(npix);
  float *dr_mlri    = dt_alloc_align_float(npix);
  float *db_mlri    = dt_alloc_align_float(npix);

  /* Candidate for RI / GF-RI */
  float *green_ri   = dt_alloc_align_float(npix);
  float *dr_ri      = dt_alloc_align_float(npix);
  float *db_ri      = dt_alloc_align_float(npix);
  float *dr_gfri    = dt_alloc_align_float(npix);
  float *db_gfri    = dt_alloc_align_float(npix);

  if(!green_mlri || !dr_mlri || !db_mlri || !green_ri || !dr_ri || !db_ri
     || !dr_gfri || !db_gfri)
  {
    dt_free_align(green_mlri); dt_free_align(dr_mlri); dt_free_align(db_mlri);
    dt_free_align(green_ri);   dt_free_align(dr_ri);   dt_free_align(db_ri);
    dt_free_align(dr_gfri);    dt_free_align(db_gfri);
    dt_free_align(cfa);
    memset(out, 0, sizeof(float) * npix * 4);
    return;
  }

  /* Generate MLRI candidate */
  _ari_green_mlri(green_mlri, cfa, width, height, filters);
  _ari_interpolate_cd(dr_mlri, db_mlri, cfa, green_mlri, width, height, filters);

  /* Generate RI candidate (HA green + bilinear CD) */
  _ari_green_ha(green_ri, cfa, width, height, filters);
  _ari_interpolate_cd(dr_ri, db_ri, cfa, green_ri, width, height, filters);

  /* Generate GF-RI: guided filter on RI color differences */
  _ari_guided_filter_ch(dr_ri, green_ri, dr_gfri, width, height, ARI_GF_RADIUS, gf_eps);
  _ari_guided_filter_ch(db_ri, green_ri, db_gfri, width, height, ARI_GF_RADIUS, gf_eps);

  /* Adaptive selection */
  if(quality >= 3)
  {
    /* 3 candidates: MLRI, RI, GF-RI (sharpest to smoothest) */
    float *greens[3]  = { green_mlri, green_ri, green_ri };
    float *diff_rs[3] = { dr_mlri, dr_ri, dr_gfri };
    float *diff_bs[3] = { db_mlri, db_ri, db_gfri };
    _ari_adaptive_select(out, greens, diff_rs, diff_bs, 3,
                         width, height, noise_sigma);
  }
  else
  {
    /* 2 candidates: MLRI, GF-RI */
    float *greens[2]  = { green_mlri, green_ri };
    float *diff_rs[2] = { dr_mlri, dr_gfri };
    float *diff_bs[2] = { db_mlri, db_gfri };
    _ari_adaptive_select(out, greens, diff_rs, diff_bs, 2,
                         width, height, noise_sigma);
  }

  dt_free_align(green_mlri); dt_free_align(dr_mlri); dt_free_align(db_mlri);
  dt_free_align(green_ri);   dt_free_align(dr_ri);   dt_free_align(db_ri);
  dt_free_align(dr_gfri);    dt_free_align(db_gfri);
  dt_free_align(cfa);
}
