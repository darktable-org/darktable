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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <errno.h>

#ifndef SPEKTRA_INLINE
#define SPEKTRA_INLINE static inline
#endif

/* Spatial effects implemented in spektra_core.c (they use dt_gaussian and so
   need darktable linkage; everything else in this header is inline). */
void sf_blur_plane3(float *buf, int w, int h, float sigma, float *plane);
void sf_halation(float *raw, int w, int h, double pixel_um, float amount, float spatial_scale);
void sf_boost_highlights(float *raw, int w, int h, float boost_ev, float boost_range,
                         float protect_ev);
void sf_diffusion_filter(float *raw, int w, int h, double pixel_um, float strength,
                         float spatial_scale, float halo_warmth);

/* Diffusion-filter Gaussian bank, built host-side and consumed by the GPU path
   (the CPU path builds it internally). Each entry is one Gaussian blur of the
   linear plane, with a per-channel weight; the scattered image is their sum, and
   the final mix is (1-p_s)*in + p_s*scatter. */
#define SF_DIFFUSION_MAX_BANK 11  /* core(2) + halo(3) + bloom(4) + margin */
typedef struct sf_diffusion_plan_t
{
  int n;                              /* number of Gaussian components */
  float sigma_um[SF_DIFFUSION_MAX_BANK];   /* blur sigma in micrometres (×scale/pixel = px) */
  float wr[SF_DIFFUSION_MAX_BANK];    /* per-channel weight (already ×group weight) */
  float wg[SF_DIFFUSION_MAX_BANK];
  float wb[SF_DIFFUSION_MAX_BANK];
  float p_s;                          /* scatter fraction */
} sf_diffusion_plan_t;

/* Fill `plan` for the given strength/warmth. Returns 0 and sets plan->p_s=0 when
   the filter is a no-op. spatial_scale/pixel are applied by the caller (sigma_px
   = sigma_um * spatial_scale / pixel_um). */
int sf_diffusion_build_plan(float strength, float halo_warmth, sf_diffusion_plan_t *plan);


/* Whole-file reader for the bundle loader (bundle.json and the .cube LUTs are
   small enough to slurp). Inside darktable the including .c maps these to glib
   (g_file_get_contents / g_free); darktable poisons bare libc fopen, so no libc
   fallback is emitted in a darktable translation unit. The standalone unit test
   (-DSF_STANDALONE) gets a small stdio-based fallback.

   SF_READ_FILE(path, char **out_buf, size_t *out_len) -> 0 on success, the buffer
   is NUL-terminated and owned by the caller, freed with SF_FREE_FILE. */
#ifndef SF_READ_FILE
#ifdef SF_STANDALONE
#include <stdio.h>
SPEKTRA_INLINE int sf_read_file_stdio(const char *path, char **out, size_t *len)
{
  FILE *f = fopen(path, "rb");
  if(!f) return -1;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if(sz < 0) { fclose(f); return -1; }
  char *b = (char *)malloc((size_t)sz + 1);
  if(!b) { fclose(f); return -1; }
  if(fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return -1; }
  b[sz] = 0;
  fclose(f);
  *out = b;
  if(len) *len = (size_t)sz;
  return 0;
}
#define SF_READ_FILE(path, out, len) sf_read_file_stdio((path), (out), (len))
#define SF_FREE_FILE(buf) free(buf)
#else
#error "SF_READ_FILE must be defined (map to g_file_get_contents) before including spektra_core.h"
#endif
#endif

/* Locale-independent ASCII float parse. darktable runs under the user locale
   (e.g. de_DE uses ',' as decimal), but .cube / bundle.json always use '.'.
   sscanf("%f")/strtod honour LC_NUMERIC, so we must not use them. The module
   maps SF_STRTOD to g_ascii_strtod; standalone uses a small C-locale parser. */
#ifndef SF_STRTOD
SPEKTRA_INLINE double sf_ascii_strtod(const char *s, char **end)
{
  while(*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
  double sign = 1.0;
  if(*s == '+')
    s++;
  else if(*s == '-')
  {
    sign = -1.0;
    s++;
  }
  double val = 0.0;
  int any = 0;
  while(*s >= '0' && *s <= '9')
  {
    val = val * 10.0 + (*s - '0');
    s++;
    any = 1;
  }
  if(*s == '.')
  {
    s++;
    double f = 0.0, sc = 1.0;
    while(*s >= '0' && *s <= '9')
    {
      f = f * 10.0 + (*s - '0');
      sc *= 10.0;
      s++;
      any = 1;
    }
    val += f / sc;
  }
  if(any && (*s == 'e' || *s == 'E'))
  {
    s++;
    int es = 1, e = 0;
    if(*s == '+')
      s++;
    else if(*s == '-')
    {
      es = -1;
      s++;
    }
    while(*s >= '0' && *s <= '9')
    {
      e = e * 10 + (*s - '0');
      s++;
    }
    double m = 1.0;
    for(int i = 0; i < e; i++) m *= 10.0;
    val = es > 0 ? val * m : val / m;
  }
  if(end) *end = (char *)s;
  return any ? sign * val : 0.0;
}
#define SF_STRTOD(s, end) sf_ascii_strtod((s), (end))
#endif

SPEKTRA_INLINE float sf_clampf(float x, float lo, float hi)
{
  return x < lo ? lo : (x > hi ? hi : x);
}

/* ---------------- .cube + bundle ---------------- */
typedef struct
{
  int n;
  float *data;
} sf_cube_t; /* n^3 * 3, R fastest */
typedef struct
{
  sf_cube_t film, print;
  float d_min[3], d_max[3]; /* cmy_film wire */
  char name[256];           /* bundle dir name */
  int valid;
  int is_positive; /* slide/reversal film: film cube has inverted density slope */
  int is_combined; /* 1-LUT (combined rgb_in->rgb_out) bundle: one cube in `film`,
                      no density split, no `print`. Used for B&W and any 1lut bake. */
  float input_gain; /* bundle.json input_exposure.gain: the cube was baked so that
                       film_pipeline(decode(coord) * gain). At runtime we sample at
                       coord = srgb_oetf(linear / input_gain). Default 1.0. */
} sf_bundle_t;

SPEKTRA_INLINE int sf_load_cube(const char *path, sf_cube_t *c)
{
  char *buf = NULL;
  size_t len = 0;
  if(SF_READ_FILE(path, &buf, &len) != 0 || !buf)
  {
#ifdef SF_DIAG_LOG
    SF_DIAG_LOG("[spektrafilm] read cube FAILED: %s\n", path);
#endif
    return -1;
  }

  c->n = 0;
  c->data = NULL;
  int idx = 0, cap = 0;
  /* Walk the file buffer line by line (the .cube grammar is line-oriented):
     header keywords (LUT_3D_SIZE, DOMAIN_*, TITLE) and one "r g b" triplet per
     data line, with R varying fastest. */
  char *p = buf;
  while(*p)
  {
    char *eol = p;
    while(*eol && *eol != '\n') eol++;
    const char hold = *eol;
    *eol = 0; /* terminate this line for the parsers below */

    char *s = p;
    while(*s == ' ' || *s == '\t') s++;
    if(*s == '#' || *s == '\r' || *s == 0)
    {
      /* comment or blank: skip */
    }
    else if(!strncmp(s, "LUT_3D_SIZE", 11))
    {
      c->n = atoi(s + 11);
      cap = c->n * c->n * c->n * 3;
      c->data = (float *)malloc(sizeof(float) * cap);
      if(!c->data)
      {
        SF_FREE_FILE(buf);
        return -1;
      }
    }
    else if(!strncmp(s, "DOMAIN_", 7) || !strncmp(s, "TITLE", 5) || (*s >= 'A' && *s <= 'Z'))
    {
      /* other header keyword: skip */
    }
    else
    {
      char *e1 = NULL, *e2 = NULL, *e3 = NULL;
      const float r = (float)SF_STRTOD(s, &e1);
      const float g = (float)SF_STRTOD(e1, &e2);
      const float b = (float)SF_STRTOD(e2, &e3);
      if(e1 != s && e2 != e1 && e3 != e2 && idx + 3 <= cap)
      {
        c->data[idx++] = r;
        c->data[idx++] = g;
        c->data[idx++] = b;
      }
    }

    if(hold == 0) break;
    p = eol + 1;
  }
  SF_FREE_FILE(buf);

  if(c->n <= 0 || idx != c->n * c->n * c->n * 3)
  {
#ifdef SF_DIAG_LOG
    SF_DIAG_LOG("[spektrafilm] cube row/size mismatch n=%d got=%d expect=%d: %s\n", c->n, idx,
                c->n * c->n * c->n * 3, path);
#endif
    free(c->data);
    c->data = NULL;
    return -1;
  }
  return 0;
}
SPEKTRA_INLINE void sf_cube_free(sf_cube_t *c)
{
  free(c->data);
  c->data = NULL;
  c->n = 0;
}

SPEKTRA_INLINE void sf_cube_sample(const sf_cube_t *c, const float in[3], float out[3])
{
  const int n = c->n;
  float fx = sf_clampf(in[0], 0, 1) * (n - 1), fy = sf_clampf(in[1], 0, 1) * (n - 1),
        fz = sf_clampf(in[2], 0, 1) * (n - 1);
  int x0 = (int)fx, y0 = (int)fy, z0 = (int)fz, x1 = x0 < n - 1 ? x0 + 1 : x0,
      y1 = y0 < n - 1 ? y0 + 1 : y0, z1 = z0 < n - 1 ? z0 + 1 : z0;
  float dx = fx - x0, dy = fy - y0, dz = fz - z0;
#define SFI(X, Y, Z) (((size_t)(Z) * n * n + (size_t)(Y) * n + (X)) * 3)
  for(int ch = 0; ch < 3; ch++)
  {
    float a = c->data[SFI(x0, y0, z0) + ch] * (1 - dx) + c->data[SFI(x1, y0, z0) + ch] * dx;
    float b = c->data[SFI(x0, y1, z0) + ch] * (1 - dx) + c->data[SFI(x1, y1, z0) + ch] * dx;
    float cc = c->data[SFI(x0, y0, z1) + ch] * (1 - dx) + c->data[SFI(x1, y0, z1) + ch] * dx;
    float d = c->data[SFI(x0, y1, z1) + ch] * (1 - dx) + c->data[SFI(x1, y1, z1) + ch] * dx;
    float e = a * (1 - dy) + b * dy, g = cc * (1 - dy) + d * dy;
    out[ch] = e * (1 - dz) + g * dz;
  }
#undef SFI
}

/* tiny JSON scrapes (sufficient for the fixed spektrafilm bundle.json schema) */
SPEKTRA_INLINE int sf_scrape_float(const char *b, const char *k, float *out)
{
  /* find key k (e.g. "\"gain\"") then the number after the following ':' */
  const char *p = strstr(b, k);
  if(!p) return -1;
  p = strchr(p, ':');
  if(!p) return -1;
  p++;
  while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  char *end = NULL;
  double d = SF_STRTOD(p, &end);
  if(end == p) return -1;
  *out = (float)d;
  return 0;
}

SPEKTRA_INLINE int sf_scrape_vec3(const char *b, const char *k, float v[3])
{
  const char *p = strstr(b, k);
  if(!p) return -1;
  p = strchr(p, '[');
  if(!p) return -1;
  p++;
  for(int i = 0; i < 3; i++)
  {
    while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
    char *end = NULL;
    double d = SF_STRTOD(p, &end);
    if(end == p) return -1;
    v[i] = (float)d;
    p = end;
  }
  return 0;
}
SPEKTRA_INLINE int sf_scrape_path(const char *buf, const char *role, char *out, int sz)
{
  const char *p = buf;
  while((p = strstr(p, "\"role\"")))
  {
    const char *c = strchr(p, ':'), *q1 = c ? strchr(c, '"') : 0,
               *q2 = q1 ? strchr(q1 + 1, '"') : 0;
    if(!q2)
    {
      p += 5;
      continue;
    }
    int len = (int)(q2 - q1 - 1);
    if((int)strlen(role) == len && !strncmp(q1 + 1, role, len))
    {
      const char *nr = strstr(q2, "\"role\""), *pa = strstr(q2, "\"path\"");
      if(!pa || (nr && pa > nr))
      {
        p = q2;
        continue;
      }
      pa = strchr(pa, ':');
      pa = strchr(pa, '"');
      if(!pa) return -1;
      pa++;
      const char *e = strchr(pa, '"');
      if(!e || e - pa >= sz) return -1;
      memcpy(out, pa, e - pa);
      out[e - pa] = 0;
      return 0;
    }
    p = q2;
  }
  return -1;
}
/* load a bundle dir (containing bundle.json + the two cubes) */
SPEKTRA_INLINE int sf_load_bundle(const char *dir, sf_bundle_t *b)
{
  memset(b, 0, sizeof *b);
  char jp[1024];
  snprintf(jp, sizeof jp, "%s/bundle.json", dir);
  char *buf = NULL;
  size_t sz = 0;
  if(SF_READ_FILE(jp, &buf, &sz) != 0 || !buf || sz == 0)
  {
    SF_FREE_FILE(buf);
    return -1;
  }
  /* input exposure gain (bundle.json input_exposure.gain). The cube maps
     output(coord) = film_pipeline(decode(coord) * gain), so at runtime we sample
     at coord = srgb_oetf(linear / gain). Default 1.0 when absent (older bundles
     or stops_above_midgray=null). Scrape the "gain" key inside "input_exposure". */
  b->input_gain = 1.0f;
  {
    const char *ie = strstr(buf, "\"input_exposure\"");
    if(ie)
    {
      float g = 1.0f;
      if(!sf_scrape_float(ie, "\"gain\"", &g) && g > 1e-4f) b->input_gain = g;
    }
  }
  /* 1-LUT (combined) bundle? It has a single lut with role "combined" and no
     film/print density wire. Load that one cube into `film` and mark combined. */
  char cp[256] = {0};
  if(!sf_scrape_path(buf, "combined", cp, sizeof cp))
  {
    SF_FREE_FILE(buf);
    char full[2048];
    snprintf(full, sizeof full, "%s/%s", dir, cp);
    if(sf_load_cube(full, &b->film)) return -1;
    b->is_combined = 1;
    b->valid = 1;
    return 0; /* no density wire / print / positive-detection for combined */
  }

  int ok =
      !sf_scrape_vec3(buf, "\"d_max\"", b->d_max) && !sf_scrape_vec3(buf, "\"d_min\"", b->d_min);
  char fp[256] = {0}, pp[256] = {0};
  ok = ok && !sf_scrape_path(buf, "film", fp, sizeof fp) &&
       !sf_scrape_path(buf, "print", pp, sizeof pp);
  SF_FREE_FILE(buf);
  if(!ok)
  {
#ifdef SF_DIAG_LOG
    SF_DIAG_LOG("[spektrafilm] bundle.json parse failed (wire/paths) in %s\n", dir);
#endif
    return -1;
  }
  char full[2048];
  snprintf(full, sizeof full, "%s/%s", dir, fp);
  if(sf_load_cube(full, &b->film)) return -1;
  snprintf(full, sizeof full, "%s/%s", dir, pp);
  if(sf_load_cube(full, &b->print))
  {
    sf_cube_free(&b->film);
    return -1;
  }
  b->valid = 1;

  /* Detect positive (slide/reversal) film: sample the film cube at black and
     white, convert to cmy_film density via the wire, and compare. Negative
     films -> density rises with input; positive films -> density falls. This
     needs no metadata (bundle.json omits film type) and no name matching. */
  {
    float blk[3] = {0.f, 0.f, 0.f}, wht[3] = {1.f, 1.f, 1.f}, fo_b[3], fo_w[3];
    sf_cube_sample(&b->film, blk, fo_b);
    sf_cube_sample(&b->film, wht, fo_w);
    float d_b = 0.f, d_w = 0.f;
    for(int c = 0; c < 3; c++)
    {
      d_b += b->d_min[c] + fo_b[c] * (b->d_max[c] - b->d_min[c]);
      d_w += b->d_min[c] + fo_w[c] * (b->d_max[c] - b->d_min[c]);
    }
    b->is_positive = (d_w < d_b) ? 1 : 0; /* white darker than black => slide */
  }
  return 0;
}
SPEKTRA_INLINE void sf_bundle_free(sf_bundle_t *b)
{
  sf_cube_free(&b->film);
  sf_cube_free(&b->print);
  b->valid = 0;
}

SPEKTRA_INLINE void sf_to_density(const sf_bundle_t *b, const float v[3], float d[3])
{
  for(int c = 0; c < 3; c++) d[c] = b->d_min[c] + v[c] * (b->d_max[c] - b->d_min[c]);
}
SPEKTRA_INLINE void sf_from_density(const sf_bundle_t *b, const float d[3], float v[3])
{
  for(int c = 0; c < 3; c++)
    v[c] = sf_clampf((d[c] - b->d_min[c]) / (b->d_max[c] - b->d_min[c]), 0, 1);
}

/* ---------------- sRGB transfer (module is scene-linear; cubes are sRGB) ---------------- */
SPEKTRA_INLINE float sf_srgb_oetf(float x)
{
  x = x < 0 ? 0 : x;
  return x <= 0.0031308f ? 12.92f * x : 1.055f * powf(x, 1.0f / 2.4f) - 0.055f;
}
SPEKTRA_INLINE float sf_srgb_eotf(float x)
{
  x = sf_clampf(x, 0, 1);
  return x <= 0.04045f ? x / 12.92f : powf((x + 0.055f) / 1.055f, 2.4f);
}

/* ---------------- grain (validated) ----------------
 *
 * Grain must be random per pixel yet perfectly reproducible (stable under
 * re-render, pan and zoom, and identical on CPU and GPU). So instead of a
 * stateful PRNG we use a stateless integer HASH keyed on the pixel coordinates:
 * hash(x, y, channel) -> a random-looking value for that exact pixel. The hash
 * constants below are published, well-tested values, NOT tunable parameters;
 * any good integer hash would do, and changing them only reshuffles the noise.
 */

/* sf_h: Chris Wellons' "lowbias32" integer hash finalizer. The multipliers and
   shift sequence are the published, bias-minimised constants of that algorithm. */
SPEKTRA_INLINE uint32_t sf_h(uint32_t x)
{
  x ^= x >> 16;
  x *= 0x7feb352dU;
  x ^= x >> 15;
  x *= 0x846ca68bU;
  x ^= x >> 16;
  return x;
}
/* sf_u01: hash -> uniform float in [0,1) using the top 24 bits (float mantissa). */
SPEKTRA_INLINE float sf_u01(uint32_t s)
{
  return (sf_h(s) & 0xffffff) / (float)0x1000000;
}
/* sf_nrm: two uniforms -> one standard-normal sample via the Box-Muller
   transform. 6.2831853 is 2*pi; 2654435761 is Knuth's golden-ratio multiplier
   (2^32 / phi), used only to decorrelate the second uniform from the first. */
SPEKTRA_INLINE float sf_nrm(uint32_t s)
{
  float u1 = fmaxf(sf_u01(s), 1e-7f), u2 = sf_u01(s * 2654435761u + 1u);
  return sqrtf(-2.f * logf(u1)) * cosf(6.2831853f * u2);
}
/* sf_layer_particle: draw the developed density of one emulsion layer as a
   doubly-stochastic process. First the number of developed grains in this pixel
   (mean lam, Poisson -> normal approximation), then the fraction that record
   signal (binomial -> normal approximation). The 0x9e3779b9 / 0x85ebca6b offsets
   are standard hash-mixing constants (golden ratio; murmurhash) that simply give
   the two normal draws independent seeds. */
/* sf_pixel_seed: combine pixel coordinates and a channel/sub-layer index into one
   seed for the grain hash. The three large primes are Teschner et al.'s published
   spatial-hash constants; XOR-mixing distinct primes per axis keeps neighbouring
   pixels and channels from sharing a seed (which would correlate their grain).
   Uses ABSOLUTE image coordinates so grain is stable while panning. */
SPEKTRA_INLINE uint32_t sf_pixel_seed(uint32_t xi, uint32_t yi, uint32_t chan)
{
  return xi * 73856093u ^ yi * 19349663u ^ chan * 83492791u;
}

/* Print-stage grading applied to the CMY film density before the print cube
   (2lut path only). Mirrors the spektrafilm app's print controls, approximated on
   the baked density rather than by re-running the paper model:
   - print_exposure (stops): a uniform density shift (brighter print = less
     density); dchange = -print_exposure * SF_PRINT_EV_TO_DENSITY.
   - print_contrast: pivots density about a mid-grey Dp so slopes steepen/flatten.
   - filtration_m / filtration_y: subtractive printing filters. Magenta rides the
     green record, yellow the blue record; each adds a small per-channel density.
   density[] is modified in place; d_ref is a representative mid density (mean of
   the film's d_min/d_max) used as the contrast pivot. */
#define SF_PRINT_EV_TO_DENSITY 0.30103f /* log10(2): one stop == 0.301 density */
#define SF_FILTRATION_TO_DENSITY 0.30f  /* full filtration slider == 0.30 density */
SPEKTRA_INLINE void sf_apply_print_grading(float density[3], float d_ref, float print_exposure,
                                           float print_contrast, float filtration_m,
                                           float filtration_y)
{
  const float ev = -print_exposure * SF_PRINT_EV_TO_DENSITY;
  for(int c = 0; c < 3; c++)
  {
    float v = density[c] + ev;                       /* print exposure */
    v = d_ref + (v - d_ref) * print_contrast;        /* print contrast (pivot) */
    density[c] = v;
  }
  /* subtractive filters: M -> green channel (index 1), Y -> blue channel (index 2) */
  density[1] += filtration_m * SF_FILTRATION_TO_DENSITY;
  density[2] += filtration_y * SF_FILTRATION_TO_DENSITY;
}

SPEKTRA_INLINE float sf_layer_particle(float density, float dmax, float npart, float unif,
                                       uint32_t seed)
{
  float p = sf_clampf(density / dmax, 1e-6f, 1 - 1e-6f), od = dmax / npart,
        sat = 1.f - p * unif * (1 - 1e-6f), lam = npart / sat;
  float seeds = lam + sqrtf(fmaxf(lam, 0)) * sf_nrm(seed * 0x9e3779b9u + 1u);
  if(seeds < 0) seeds = 0;
  float mean = seeds * p, var = seeds * p * (1 - p),
        g = mean + sqrtf(fmaxf(var, 0)) * sf_nrm(seed * 0x85ebca6bU + 7u);
  if(g < 0) g = 0;
  if(g > seeds) g = seeds;
  return g * od * sat;
}
/* SF_GRAIN_REF_UM: the fixed reference scale (spektrafilm's own
   pixel_size_um=10) the particle model is generated at, independent of the
   live pipe's pixel_um — this keeps grain CHARACTER constant across zoom.
   Callers that turn the generated delta into visible clump STRUCTURE (the
   blur step) must still convert this reference into real pixels via the
   pipe's own pixel_um, or clump SIZE silently stops scaling with output
   resolution — see the grain blur in spektrafilm.c/.cl and
   _max_halo_sigma's ROI padding, all of which must agree. */
#define SF_GRAIN_REF_UM 10.0f
/* grain on one CMY-density pixel; strength scales particle count effect via amount */
SPEKTRA_INLINE void sf_grain_px(float dens[3], float pixel_um, float amount, float size,
                                uint32_t xi, uint32_t yi)
{
  const float dmin[3] = {0.03f, 0.03f, 0.03f}, dmaxc[3] = {2.2f, 2.2f, 2.2f};
  const float pscale[3] = {1.6f, 1.6f, 3.2f}, unif[3] = {0.97f, 0.99f, 0.97f};
  const int nsub = 1;
  /* Grain is rendered at a FIXED reference scale (like spektrafilm's
     pixel_size_um=10), NOT the live pipe pixel_um, so grain character stays
     constant across zoom. The size slider scales this reference: larger size =>
     larger effective grain pixel => fewer particles per pixel => coarser grain.
     size=1.0 reproduces the app's default look. pixel_um is unused for grain
     (still used by halation). */
  const float parea = 0.2f;
  const float ref_um = SF_GRAIN_REF_UM / fmaxf(size, 0.05f); /* size up => coarser grain */
  float pix = ref_um * ref_um;
  (void)pixel_um;
  for(int c = 0; c < 3; c++)
  {
    float npart = pix / (parea * pscale[c]), dmax = dmaxc[c] + dmin[c];
    float din = dens[c] + dmin[c], acc = 0;
    for(int sl = 0; sl < nsub; sl++)
      acc += sf_layer_particle(
          din, dmax, npart, unif[c],
          sf_pixel_seed(xi, yi, (uint32_t)(c + sl * 10)));
    acc /= nsub;
    acc -= dmin[c];
    dens[c] = dens[c] + (acc - dens[c]) * amount; /* amount=1 -> full spektrafilm grain */
  }
}

/* apply halation+scatter to a w*h*3 LINEAR plane in place (amount scales both passes) */
/* Compute the grain DELTA (grained density - clean density) for one pixel into
   out_delta[3]. Generation matches the validated per-pixel particle model; the
   visible film STRUCTURE comes from blurring this delta buffer afterwards (as
   spektrafilm blurs its grain by grain.blur). Generated at a fine fixed scale so
   the subsequent blur produces organic clumps rather than 1px speckle. */
/* dmax_c: the emulsion's actual per-channel maximum density (base-subtracted).
   Using a too-small value saturates the particle model in dense areas (slide
   shadows) and produces a channel-dependent -- i.e. coloured -- bias.
   dmin_c/rms_c/unif_c: the stock's own catalogue grain characteristics
   (film_render_defaults[stock].grain in the pack — rms_granularity,
   uniformity, density_min), so e.g. Portra 400 and Tri-X no longer share one
   hardcoded grain signature. Callers without per-film data may pass the
   SF_GRAIN_LEGACY_* arrays below to reproduce the earlier fixed look. */
/* SF_GRAIN_REF_UM (defined above, with sf_grain_px) is reused here for the
   same fine-generation reference scale. */
SPEKTRA_INLINE void sf_grain_delta_dmax(const float dens[3], float amount, float out_delta[3],
                                        uint32_t xi, uint32_t yi, int mono,
                                        const float dmax_c[3], const float dmin_c[3],
                                        const float rms_c[3], const float unif_c[3])
{
  const float dmin[3] = { dmin_c[0], dmin_c[1], dmin_c[2] };
  const float dmaxc[3] = { fmaxf(dmax_c[0], 1e-3f), fmaxf(dmax_c[1], 1e-3f),
                           fmaxf(dmax_c[2], 1e-3f) };
  /* Latest spektrafilm grain model (study a90): per-channel particle area from
     catalogue RMS-granularity (sigma_48 through a 48um aperture, ISO 6328):
       a_grain = (rms/1000)^2 * A48 / (D_ref (Dmax - u D_ref)),  D_ref = 1 + d_min.
     N = pixel_area / a_grain. Generated at a fine fixed reference scale; the blur
     afterwards sets visible clump size. rms/unif come from the film stock's own
     catalogue data (see header comment) rather than one shared constant. */
  const float rms[3] = { rms_c[0], rms_c[1], rms_c[2] };
  const float unif[3] = { unif_c[0], unif_c[1], unif_c[2] };
  const float A48 = 3.14159265f * 24.0f * 24.0f;
  const float ref_um = SF_GRAIN_REF_UM, pix = ref_um * ref_um;
  /* mono (B&W / combined): the three channels carry the same value, so grain must
     be ACHROMATIC — one grain realisation applied identically to all channels.
     Per-channel independent grain (the colour path) would otherwise paint colour
     speckle onto a grey image. Use channel 1's parameters and the mean density. */
  if(mono)
  {
    const float dm = (dens[0] + dens[1] + dens[2]) / 3.0f;
    const float dmax = dmaxc[1] + dmin[1];
    const float d_ref = 1.0f + dmin[1];
    const float sig = rms[1] / 1000.0f;
    const float denom = fmaxf(d_ref * (dmax - unif[1] * d_ref), 1e-6f);
    const float a_grain = sig * sig * A48 / denom;
    const float npart = pix / fmaxf(a_grain, 1e-4f);
    const float din = dm + dmin[1];
    float g = sf_layer_particle(din, dmax, npart, unif[1],
                                sf_pixel_seed(xi, yi, 0u)) - dmin[1];
    const float d = (g - dm) * amount;
    out_delta[0] = out_delta[1] = out_delta[2] = d; /* identical -> grey grain */
    return;
  }
  for(int c = 0; c < 3; c++)
  {
    const float dmax = dmaxc[c] + dmin[c];
    const float d_ref = 1.0f + dmin[c];
    const float sig = rms[c] / 1000.0f;
    const float denom = fmaxf(d_ref * (dmax - unif[c] * d_ref), 1e-6f);
    const float a_grain = sig * sig * A48 / denom;
    const float npart = pix / fmaxf(a_grain, 1e-4f);
    const float din = dens[c] + dmin[c];
    float g = sf_layer_particle(din, dmax, npart, unif[c],
                                sf_pixel_seed(xi, yi, (uint32_t)c));
    g -= dmin[c];
    out_delta[c] = (g - dens[c]) * amount; /* delta to be blurred then added */
  }
}

/* Fallback catalogue values (spektrafilm's original single fixed profile) for
   callers with no per-film pack data (see sf_pack_film_grain / sf_sim_film_grain3
   for the real per-stock values). */
#define SF_GRAIN_LEGACY_DMAX { 2.2f, 2.2f, 2.2f }
#define SF_GRAIN_LEGACY_DMIN { 0.03f, 0.03f, 0.03f }
#define SF_GRAIN_LEGACY_RMS { 6.0f, 8.0f, 10.0f }
#define SF_GRAIN_LEGACY_UNIFORMITY { 0.97f, 0.97f, 0.97f }

SPEKTRA_INLINE void sf_grain_delta(const float dens[3], float amount, float out_delta[3],
                                   uint32_t xi, uint32_t yi, int mono)
{
  const float legacy_dmax[3] = SF_GRAIN_LEGACY_DMAX;
  const float legacy_dmin[3] = SF_GRAIN_LEGACY_DMIN;
  const float legacy_rms[3] = SF_GRAIN_LEGACY_RMS;
  const float legacy_unif[3] = SF_GRAIN_LEGACY_UNIFORMITY;
  sf_grain_delta_dmax(dens, amount, out_delta, xi, yi, mono, legacy_dmax, legacy_dmin,
                      legacy_rms, legacy_unif);
}
