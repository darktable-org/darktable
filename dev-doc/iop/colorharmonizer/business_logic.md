# Color Harmonizer — Business Logic & Implementation Guide

This document breaks down the math, design decisions, and code paths of the
`colorharmonizer` IOP module. Each processing step is shown for both the CPU
(`process()`) and OpenCL (`process_cl()` / `.cl` kernels) implementations, with
rationale for the choices made.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Color Spaces & Why UCS JCH](#2-color-spaces--why-ucs-jch)
3. [Harmony Geometry](#3-harmony-geometry)
4. [RYB ↔ UCS Hue Conversion](#4-ryb--ucs-hue-conversion)
5. [Node Precomputation (`commit_params`)](#5-node-precomputation-commit_params)
6. [Gaussian-Weighted Hue Shift](#6-gaussian-weighted-hue-shift)
7. [Neutral Color Protection](#7-neutral-color-protection)
8. [Per-Node Saturation Control](#8-per-node-saturation-control)
9. [Processing Pipeline — CPU](#9-processing-pipeline--cpu)
10. [Processing Pipeline — OpenCL](#10-processing-pipeline--opencl)
11. [CPU vs OpenCL: Key Differences](#11-cpu-vs-opencl-key-differences)
12. [Spatial Smoothing](#12-spatial-smoothing)
13. [Auto-Detection Algorithm](#13-auto-detection-algorithm)
14. [File & Line Reference](#14-file--line-reference)

---

## 1. Overview

The Color Harmonizer shifts pixel hues toward a set of "harmony nodes" —
canonical hue positions defined by classic color theory (complementary, triadic,
etc.). The correction is:

- **Perceptual**: operates in darktable UCS JCH, a uniform color space where
  equal angular steps correspond to equal perceived hue differences.
- **Smooth**: uses Gaussian weighting so pixels near a node are strongly
  attracted, pixels between nodes are gently nudged, and pixels far from all
  nodes are barely touched.
- **Chroma-aware**: desaturated (near-gray) pixels are protected from hue
  shifts via a tunable neutral-protection curve.

The module supports both CPU (OpenMP-parallelized) and GPU (OpenCL) execution
with identical visual results.

---

## 2. Color Spaces & Why UCS JCH

### The problem with operating in RGB or HSL

Hue in RGB-derived spaces (HSL, HSV) is non-uniform: a 30° rotation near
orange produces a much larger perceived change than 30° near blue. Shifting
hues in these spaces would produce uneven, artifact-prone corrections.

### darktable UCS JCH

The module converts every pixel to darktable's **Uniform Color Space** JCH
model:

| Component | Meaning | Range |
|-----------|---------|-------|
| **J** | Lightness (perceptual) | 0 → ~1 |
| **C** | Chroma (colorfulness) | 0 → ~2 |
| **H** | Hue angle (radians) | −π → +π |

In this space, equal angular distances produce roughly equal perceived hue
differences, making Gaussian weighting behave predictably.

### Forward conversion chain (RGB → JCH)

```
Pipeline RGB (linear, scene-referred)
  → XYZ (D50)           [3×3 matrix multiply via work profile]
  → XYZ (D65)           [chromatic adaptation, CAT16]
  → xyY                 [dt_D65_XYZ_to_xyY]
  → dt_UCS JCH          [xyY_to_dt_UCS_JCH, requires L_white]
```

**CPU** — single call wrapping the entire chain:
```c
// colorharmonizer.c:460
dt_ioppr_rgb_matrix_to_dt_UCS_JCH(px_rgb, px_JCH,
    work_profile->matrix_in_transposed, L_white);
```

**OpenCL** — equivalent steps inline in the map kernel:
```c
// colorharmonizer.cl:98-100
float4 XYZ_D65 = matrix_product_float4(fmax(0.0f, pix_in), matrix_in);
float4 xyY     = dt_D65_XYZ_to_xyY(XYZ_D65);
float4 JCH     = xyY_to_dt_UCS_JCH(xyY, L_white);
```

**Why the difference?** The CPU helper `dt_ioppr_rgb_matrix_to_dt_UCS_JCH`
bundles the matrix multiply and adaptation into one function using the
transposed work-profile matrix. On the GPU, the matrix is pre-multiplied with
`XYZ_D50_to_D65_CAT16` on the host before upload, so the kernel does a single
matrix multiply directly to D65 XYZ — avoiding an extra per-pixel adaptation
step on the GPU.

### Inverse conversion chain (JCH → RGB)

```
dt_UCS JCH
  → xyY                 [dt_UCS_JCH_to_xyY]
  → XYZ (D65)           [dt_xyY_to_XYZ]
  → XYZ (D50)           [XYZ_D65_to_D50, CPU only — GPU folds into matrix]
  → Pipeline RGB         [3×3 matrix multiply via work profile]
```

**CPU:**
```c
// colorharmonizer.c:477-482
dt_UCS_JCH_to_xyY(px_JCH, L_white, px_xyY);
dt_xyY_to_XYZ(px_xyY, px_xyz_d65);
XYZ_D65_to_D50(px_xyz_d65, px_xyz);
dt_apply_transposed_color_matrix(px_xyz,
    work_profile->matrix_out_transposed, px_rgb_out);
```

**OpenCL:**
```c
// colorharmonizer.cl:151-154
float4 xyY     = dt_UCS_JCH_to_xyY(JCH, L_white);
float4 XYZ_D65 = dt_xyY_to_XYZ(xyY);
float4 pix_out = matrix_product_float4(XYZ_D65, matrix_out);
```

The GPU `matrix_out` is pre-multiplied as
`work_profile->matrix_out × XYZ_D65_to_D50_CAT16` on the host, collapsing two
steps into one matrix multiply.

### `L_white` — a constant, not a per-pixel value

```c
const float L_white = Y_to_dt_UCS_L_star(1.0f);
```

This converts Y=1 (the scene-referred white luminance) to the UCS lightness
scale. It's the same for every pixel in a given pipeline run. The CPU computes
it once before the pixel loop; the GPU receives it as a kernel argument —
avoiding a redundant transcendental (`powf`) per pixel.

### Normalized hue

The module normalizes the JCH hue angle from [−π, +π] to [0, 1) for all
internal arithmetic:

```c
const float hue = (JCH.z + M_PI_F) / (2.0f * M_PI_F);
```

This simplifies circular-distance calculations and makes the harmony node
positions directly comparable. The inverse (back to radians) is applied just
before the JCH→RGB conversion:

```c
JCH.z = new_hue * 2.0f * M_PI_F - M_PI_F;
```

---

## 3. Harmony Geometry

### The geometry table

All predefined rules are defined in a single authoritative table in
`color_harmony.h:83`, shared with the vectorscope overlay:

```c
static const struct { int n; float offsets[4]; } table[] = {
  [MONOCHROMATIC]           = { 1, {  0/12  } },
  [ANALOGOUS]               = { 3, { -1/12,  0/12, +1/12 } },
  [ANALOGOUS_COMPLEMENTARY] = { 4, { -1/12,  0/12, +1/12, +6/12 } },
  [COMPLEMENTARY]           = { 2, {  0/12, +6/12 } },
  [SPLIT_COMPLEMENTARY]     = { 3, {  0/12, +5/12, +7/12 } },
  [DYAD]                    = { 2, { -1/12, +1/12 } },
  [TRIAD]                   = { 3, {  0/12, +4/12, +8/12 } },
  [TETRAD]                  = { 4, { -1/12, +1/12, +5/12, +7/12 } },
  [SQUARE]                  = { 4, {  0/12, +3/12, +6/12, +9/12 } },
};
```

Each offset is a fraction of a full turn. 1/12 = 30° in the RYB color wheel
(the same 12-sector wheel used in traditional color theory). The anchor
rotation is added and the result wrapped to [0, 1).

**Why a single table?** Keeping the geometry in one place guarantees that the
guide lines drawn on the vectorscope exactly match the node positions used for
processing. A dual definition would risk subtle angular mismatches.

### Custom mode

When `rule == CUSTOM`, the user provides up to 4 arbitrary hue positions
directly in UCS space. These bypass the geometry table entirely — the
`custom_hue[]` array is used as-is.

---

## 4. RYB ↔ UCS Hue Conversion

### The problem

The harmony geometry is defined in **RYB** (Red-Yellow-Blue) hue space — the
traditional artist's color wheel. But processing happens in **UCS** hue space.
These two spaces have a nonlinear relationship: RYB stretches the warm hues
(yellow through red) and compresses the cool hues (blue through green) relative
to UCS.

### The Gossett piecewise-linear model

The RYB mapping is based on the Gossett paint-mixing model. Six control points
map evenly-spaced RGB hue knots to RYB hue values:

```c
// color_ryb.h:37-41
static const float dt_color_ryb_x_vtx[7] = { 0, 1/6, 2/6, 3/6, 4/6, 5/6, 1 };  // RGB hue
static const float dt_color_ryb_y_vtx[7] = { 0, 1/3, 0.472, 0.611, 0.715, 5/6, 1 };  // RYB hue
```

The path from UCS hue to RYB hue is indirect:

```
UCS hue → gamut-clipped sRGB → linearize → RGB HSV hue → piecewise-linear → RYB hue
```

This chain involves transcendentals (gamma, trig) and is too expensive to run
per-pixel. The solution is a **pair of lookup tables**.

### Precomputed LUTs

Built once in `init_global()` via `_build_hue_luts()`:

```c
// colorharmonizer.c:641-656 (simplified)
// Forward: UCS → RYB (720 entries = 0.5° resolution)
for(int i = 0; i < 720; i++)
  s_ucs_to_ryb_lut[i] = _ucs_hue_to_ryb_hue(i / 720.0f);

// Inverse: RYB → UCS (nearest-neighbor search of the forward table)
for(int j = 0; j < 720; j++) {
  float target = j / 720.0f;
  // find the forward-table entry closest to target
  s_ryb_to_ucs_lut[j] = best_matching_ucs;
}
```

**Why nearest-neighbor for the inverse?** The forward map (UCS→RYB) is
monotonic but non-analytic (it goes through sRGB gamma and HSV extraction).
Inverting it analytically is impractical. The brute-force O(n²) scan is cheap
since it runs exactly once at application startup (720 × 720 = ~518k
comparisons, sub-millisecond on any modern CPU). The resulting table accuracy
(0.5° max error) is more than sufficient for positioning harmony nodes.

### O(1) lookup with linear interpolation

```c
// colorharmonizer.c:621-626
static inline float _ucs_to_ryb_fast(float ucs) {
  const float pos = ucs * 720;
  const int   i0  = (int)pos % 720;
  const int   i1  = (i0 + 1) % 720;
  return _hue_lerp(s_ucs_to_ryb_lut[i0], s_ucs_to_ryb_lut[i1], pos - (int)pos);
}
```

`_hue_lerp` handles circular wraparound (e.g. interpolating between 0.95 and
0.05 across the 0/1 boundary):

```c
static inline float _hue_lerp(float a, float b, float t) {
  if(b - a >  0.5f) b -= 1.0f;
  else if(a - b >  0.5f) a -= 1.0f;
  float r = a + t * (b - a);
  if(r < 0.0f) r += 1.0f;
  return r;
}
```

---

## 5. Node Precomputation (`commit_params`)

Harmony nodes are computed once per pipeline run, not per pixel:

```c
// colorharmonizer.c:366-376
void commit_params(...) {
  d->params = *p;
  get_harmony_nodes(p->rule, p->anchor_hue, p->custom_hue,
                    p->num_custom_nodes, d->nodes, &d->num_nodes);
}
```

`get_harmony_nodes()` for predefined rules:

```c
// colorharmonizer.c:342-365
// 1. Convert UCS anchor hue → RYB rotation (integer degrees)
const int rotation = (int)roundf(_ucs_to_ryb_fast(anchor_hue) * 360.0f) % 360;

// 2. Query the geometry table for absolute RYB node positions
dt_color_harmony_get_sector_angles(rule + 1, rotation, node_angles, &num_nodes);

// 3. Convert each RYB node back to UCS for processing
for(int i = 0; i < *num_nodes; i++)
  nodes[i] = _ryb_to_ucs_fast(node_angles[i]);
```

**Why convert twice (UCS → RYB → UCS)?** The geometry table defines angular
relationships in RYB space (30° analogous, 180° complementary, etc.) because
that matches the traditional artist's color wheel used in the vectorscope
overlay. The user's anchor hue is stored in UCS, so we must convert to RYB to
apply the geometry offsets, then back to UCS for processing. This round-trip
ensures the processing nodes are exactly aligned with the vectorscope guide
lines.

---

## 6. Gaussian-Weighted Hue Shift

This is the core algorithm — it determines how much each pixel's hue is pulled
toward the nearest harmony node.

### Math

For a pixel with hue `h` and `N` harmony nodes at positions `nᵢ`:

1. **Gaussian sigma** scales with the zone width parameter and inversely with
   node count:

   ```
   σ = pull_width × 0.5 / N
   ```

   More nodes = tighter zones per node (they must share the circle).

2. **For each node** `nᵢ`, compute:
   - Circular distance: `dᵢ = min(|h − nᵢ|, 1 − |h − nᵢ|)`
   - Gaussian weight: `wᵢ = exp(−dᵢ² / 2σ²)`
   - Signed angular difference: `Δᵢ = nᵢ − h` (wrapped to [−0.5, +0.5])

3. **Winner-take-all**: select the node with the highest weight:

   ```
   hue_shift = Δ_winner × w_winner
   ```

### Key properties

| Pixel position | `Δ_winner` | `w_winner` | Shift |
|---|---|---|---|
| Exactly at a node | 0 | 1.0 | **0** (no change needed) |
| Near a node | small | ~1.0 | small pull toward node |
| Between two nodes | moderate | moderate | moderate pull |
| Far from all nodes | large | ~0 | **~0** (too far to reach) |

The product `Δ × w` naturally produces zero shift at both extremes and a smooth
bell-shaped correction in between.

### Code — CPU

```c
// colorharmonizer.c:284-326
static inline float get_weighted_hue_shift(
    float px_hue, const float *nodes, int num_nodes,
    float pull_width_factor,
    int *out_winning_idx, float *out_max_weight)
{
  const float sigma      = pull_width_factor * 0.5f / (float)num_nodes;
  const float inv_2sigma2 = 1.0f / (2.0f * sigma * sigma);

  float max_w = 0.0f, diff_winning = 0.0f;
  int   winning_idx = 0;

  for(int i = 0; i < num_nodes; i++)
  {
    float d = fabsf(px_hue - nodes[i]);
    if(d > 0.5f) d = 1.0f - d;            // circular distance

    const float w = expf(-d * d * inv_2sigma2);
    float diff = nodes[i] - px_hue;        // signed difference
    if(diff > 0.5f)       diff -= 1.0f;    // wrap to (-0.5, +0.5]
    else if(diff < -0.5f) diff += 1.0f;

    if(w > max_w) {
      max_w = w;  winning_idx = i;  diff_winning = diff;
    }
  }

  *out_winning_idx = winning_idx;
  *out_max_weight  = max_w;
  return diff_winning * max_w;
}
```

### Code — OpenCL

```c
// colorharmonizer.cl:35-78
// Identical algorithm, adapted to OpenCL syntax (fabs → fabs, expf → exp,
// output via pointer args, 'constant' address space for the node array).
static inline float get_weighted_hue_shift(
    const float px_hue,
    constant const float *const nodes,
    const int num_nodes,
    const float zone_width_factor,
    int *out_winning_idx,
    float *out_max_weight)
{
  // ... same loop body, using exp() instead of expf() ...
}
```

**Why winner-take-all instead of weighted average?** A weighted average of all
nodes would pull every pixel toward the center of mass of the node set — losing
the distinct palette identity. Winner-take-all preserves the nearest node's
character and avoids averaging hues across the wheel (e.g. averaging red and
cyan would produce gray, not a useful correction).

---

## 7. Neutral Color Protection

Desaturated colors (grays, muted tones, skin highlights) should not be hue-
shifted — they have no meaningful hue, and shifting them introduces color casts
on near-neutral surfaces.

### Math

```
cutoff = neutral_protection³ × 0.03
chroma_weight = chroma / (chroma + cutoff + ε)
```

This is a **smooth hyperbolic ramp** from 0 (fully protected) to 1 (fully
corrected):

| `neutral_protection` | `cutoff` | Effect |
|---|---|---|
| 0.0 | 0 | No protection — all pixels shifted equally |
| 0.5 | 0.00375 | Pastels partially protected |
| 1.0 | 0.03 | Only vivid colors corrected |

The **cubic mapping** (`t³ × 0.03`) distributes the slider's perceptual
response evenly across its range. A linear mapping would concentrate most of
the visible change in the first 20% of slider travel.

### Why `ε = 1e-5`?

Prevents division by zero when `chroma = 0` and `cutoff = 0` (i.e.,
`neutral_protection = 0`). Without it, 0/0 would produce NaN.

### Code — CPU

```c
// colorharmonizer.c:442-443 (before pixel loop)
const float cutoff = np_t * np_t * np_t * 0.03f;

// colorharmonizer.c:471 (inside pixel loop, fused path)
const float chroma_weight = chroma / (chroma + cutoff + 1e-5f);
```

### Code — OpenCL

```c
// colorharmonizer.cl:136-138
const float t             = protect_neutral;
const float cutoff        = t * t * t * 0.03f;
const float chroma_weight = chroma / (chroma + cutoff + 1.0e-5f);
```

Both hue shift and saturation correction are then scaled by `chroma_weight`:

```c
new_hue    = wrap_hue(hue + hue_shift × pull_strength × chroma_weight)
new_chroma = max(chroma × (1 + sat_delta × chroma_weight), 0)
```

---

## 8. Per-Node Saturation Control

Each harmony node has an independent saturation multiplier (`node_saturation[i]`,
range 0–2, default 1.0).

### Math

```
sat_delta = (node_saturation[winning_idx] − 1.0) × max_weight
new_chroma = chroma × (1 + sat_delta × chroma_weight)
```

- `node_saturation = 1.0` → `sat_delta = 0` → no change
- `node_saturation = 0.5` → desaturate by up to 50%
- `node_saturation = 1.5` → boost by up to 50%

The `max_weight` factor ensures the saturation change is strongest for pixels
close to that node and tapers off for distant pixels — same Gaussian falloff as
the hue shift.

---

## 9. Processing Pipeline — CPU

`process()` at `colorharmonizer.c:417-574`.

### Path A: No Smoothing (`smoothing ≤ 0`) — Fused Single Pass

This is the default and most interactive-responsive path.

```
For each pixel (OpenMP parallelized):
  ┌─ Forward: RGB → JCH (1 matrix mul + nonlinear transforms)
  │  Extract normalized hue and chroma
  │
  ├─ Compute: hue_shift = get_weighted_hue_shift(...)
  │  Compute: sat_delta = (node_saturation[winner] − 1) × max_weight
  │
  ├─ Apply neutral protection: chroma_weight = chroma / (chroma + cutoff + ε)
  │  Update: JCH.hue    = wrap_hue(hue + shift × strength × chroma_weight)
  │  Update: JCH.chroma = max(chroma × (1 + sat_delta × chroma_weight), 0)
  │
  └─ Inverse: JCH → RGB (nonlinear transforms + 1 matrix mul)
      Copy alpha from input
```

**Why fuse?** When smoothing is off, there is no spatial dependency between
pixels — each pixel's correction depends only on its own hue. Fusing the
forward conversion, correction, and inverse conversion into a single pass
eliminates the need for intermediate buffers and halves memory traffic (read
input once, write output once).

### Path B: With Smoothing (`smoothing > 0`) — Two-Pass with JCH Cache

When smoothing is enabled, the correction field must be spatially blurred before
application. This requires two passes because blurring is a neighborhood
operation that reads neighboring pixels' corrections.

**Pass 1** — Forward conversion + correction computation:
```
Allocate:
  jch_cache[3 × npx]    — stores J, chroma, normalized hue per pixel
  corrections[2 × npx]  — stores hue_shift, sat_delta per pixel

For each pixel (OpenMP):
  Forward: RGB → JCH
  Cache: jch_cache[k] = {J, chroma, hue}
  Compute and cache: corrections[k] = {hue_shift, sat_delta}
```

**Spatial blur:**
```c
// colorharmonizer.c:533-535
const float sigma = smoothing × max(1.5, 8.0 × scale_ratio) × max(1.0, pull_width);
dt_gaussian_mean_blur(corrections, width, height, 2, sigma);
```

**Pass 2** — Apply blurred corrections from cache:
```
For each pixel (OpenMP):
  Read cached J, chroma, hue
  Read blurred hue_shift, sat_delta
  Apply neutral protection and update JCH (same formulas)
  Inverse: JCH → RGB
```

**Why cache JCH instead of re-converting from RGB?** The forward conversion
(RGB → JCH) involves a matrix multiply, chromatic adaptation, and several
nonlinear transforms (power functions, trig). It costs roughly 60% of the per-
pixel budget. Re-doing it in Pass 2 would nearly double the total cost. Caching
3 floats per pixel (J, chroma, hue) is cheap compared to the compute saved.

**Why blur corrections instead of JCH directly?** Hue is circular — a Gaussian
blur of hue values would produce nonsensical averages near the 0/1 wrap point
(e.g., averaging 0.95 and 0.05 gives 0.50, which is completely wrong). The
correction field (hue_shift, sat_delta) is a small signed displacement that is
safe to average spatially.

---

## 10. Processing Pipeline — OpenCL

`process_cl()` at `colorharmonizer.c:680-786`.

The OpenCL path always uses the two-kernel architecture (even when smoothing is
off) because kernel fusion would require a third kernel variant, adding
maintenance burden for a marginal GPU gain.

### Host setup

```c
// colorharmonizer.c:703-704
// Pre-multiply matrices to fold CAT16 adaptation into the matrix multiply:
dt_colormatrix_mul(input_matrix,  XYZ_D50_to_D65_CAT16, work_profile->matrix_in);
dt_colormatrix_mul(output_matrix, work_profile->matrix_out, XYZ_D65_to_D50_CAT16);

// Precompute L_white on host (constant for all pixels):
const float L_white = Y_to_dt_UCS_L_star(1.0f);

// Allocate GPU buffers:
cl_mem corrections_cl;  // float2 per pixel: {hue_shift, sat_delta}
cl_mem jch_cl;          // float4 per pixel: {J, chroma, hue, alpha}
```

**Why pre-multiply the matrices?** On the GPU, every instruction counts. By
folding the D50↔D65 chromatic adaptation into the RGB↔XYZ matrix on the host,
we save one 3×3 matrix-vector multiply per pixel in each kernel (two total).
The host-side matrix multiply is negligible (3×3, done once).

**Why cache alpha in the JCH buffer's `.w`?** The apply kernel does not have
access to the input image texture (it was removed as an optimization — one
fewer texture read per pixel). Alpha must travel from the map kernel to the
apply kernel somehow, and it fits neatly in the unused 4th component of the
float4 JCH cache.

### Kernel 1: `colorharmonizer_map`

```c
// colorharmonizer.cl:81-118
kernel void colorharmonizer_map(
    read_only image2d_t in, global float2 *p_out,
    global float4 *jch_out, const int width, const int height,
    constant const float *matrix_in, constant const float *nodes,
    const int num_nodes, const float zone_width,
    constant const float *node_saturation, const float L_white)
{
  // Bounds check
  if(x >= width || y >= height) return;

  // Forward: read texel → matrix multiply → XYZ(D65) → xyY → JCH
  const float4 pix_in  = read_imagef(in, sampleri, (int2)(x, y));
  float4 XYZ_D65 = matrix_product_float4(fmax(0.0f, pix_in), matrix_in);
  float4 xyY     = dt_D65_XYZ_to_xyY(XYZ_D65);
  float4 JCH     = xyY_to_dt_UCS_JCH(xyY, L_white);

  const float hue = (JCH.z + M_PI_F) / (2.0f * M_PI_F);

  // Cache JCH + alpha for the apply kernel
  jch_out[idx] = (float4)(JCH.x, JCH.y, hue, pix_in.w);

  // Compute corrections
  int winning_idx;  float max_weight;
  float hue_shift = get_weighted_hue_shift(hue, nodes, num_nodes,
                                            zone_width, &winning_idx, &max_weight);
  float sd = (node_saturation[winning_idx] - 1.0f) * max_weight;

  p_out[idx] = (float2)(hue_shift, sd);
}
```

### Optional: Spatial blur (host-side)

```c
// colorharmonizer.c:748-752
if(p->smoothing > 0.0f) {
  const float sigma = p->smoothing * fmaxf(1.5f, 8.0f * roi_in->scale / piece->iscale)
                      * fmaxf(1.0f, p->pull_width);
  dt_gaussian_mean_blur_cl(devid, corrections_cl, width, height, 2, sigma);
}
```

When smoothing is zero, the corrections buffer passes through unmodified.

### Kernel 2: `colorharmonizer_apply`

```c
// colorharmonizer.cl:120-157
kernel void colorharmonizer_apply(
    write_only image2d_t out, const int width, const int height,
    constant const float *matrix_out,
    global const float4 *jch_in, global const float2 *corrections,
    const float effect_strength, const float protect_neutral,
    const float L_white)
{
  if(x >= width || y >= height) return;

  // Read cached JCH and corrections
  const float4 cached = jch_in[k];
  const float2 corr   = corrections[k];

  // Neutral protection
  const float cutoff        = protect_neutral³ × 0.03f;
  const float chroma_weight = cached.y / (cached.y + cutoff + 1e-5f);

  // Apply corrections
  float4 JCH;
  JCH.x = cached.x;                                                     // J unchanged
  JCH.y = fmax(cached.y * (1 + corr.y * chroma_weight), 0);             // saturation
  JCH.z = wrap_hue(cached.z + corr.x * effect_strength * chroma_weight)
          * 2π − π;                                                      // hue → radians

  // Inverse: JCH → xyY → XYZ(D65) → RGB (via pre-multiplied matrix)
  float4 xyY     = dt_UCS_JCH_to_xyY(JCH, L_white);
  float4 XYZ_D65 = dt_xyY_to_XYZ(xyY);
  float4 pix_out = matrix_product_float4(XYZ_D65, matrix_out);
  pix_out.w      = cached.w;   // restore alpha from cache
  write_imagef(out, (int2)(x, y), pix_out);
}
```

---

## 11. CPU vs OpenCL: Key Differences

| Aspect | CPU | OpenCL |
|---|---|---|
| **smoothing=0 path** | Fused single pass (no buffers) | Always two kernels (map+apply) |
| **Forward conversion** | `dt_ioppr_rgb_matrix_to_dt_UCS_JCH` | Inline in map kernel |
| **Chromatic adaptation** | Separate `XYZ_D65_to_D50()` call | Folded into pre-multiplied matrix |
| **Alpha passthrough** | Read directly from input buffer | Cached in `jch_out.w` |
| **JCH cache layout** | 3 floats/pixel (J, C, H) | 4 floats/pixel (J, C, H, α) |
| **Parallelism** | OpenMP threads over rows | GPU work-items per pixel |
| **`L_white`** | Local variable | Kernel argument |

**Why no fused single-pass on GPU?** Writing a third kernel
(`colorharmonizer_fused`) for the smoothing=0 case would add code to maintain
and test with marginal benefit — the JCH cache is a device-local buffer read,
which on modern GPUs is nearly as fast as not caching at all. The two-kernel
overhead (one extra kernel dispatch + one buffer allocation) is negligible
compared to the pixel-processing time.

---

## 12. Spatial Smoothing

### Purpose

Without smoothing, the correction field can have sharp transitions at the
boundaries between adjacent harmony zones. For most images this is fine — the
Gaussian weighting already produces smooth gradients. But in images with subtle
hue gradients crossing a zone boundary, a slight halo or banding artifact can
appear. Spatial smoothing softens these transitions.

### Sigma computation

```c
// colorharmonizer.c:533-534 (CPU) / 749-751 (CL)
const float sigma = smoothing
    × max(1.5, 8.0 × roi_in->scale / piece->iscale)
    × max(1.0, pull_width);
```

- `roi_in->scale / piece->iscale`: adapts to the current zoom level / pipe
  resolution. At preview resolution the blur is tighter; at full resolution it
  scales up.
- `max(1.0, pull_width)`: wider attraction zones need wider smoothing to
  prevent visible zone boundaries.
- `max(1.5, ...)`: prevents the sigma from collapsing to sub-pixel values at
  high zoom.

### What gets blurred

The **correction field** (2 channels: hue_shift and sat_delta), not the image
itself or the JCH values. This preserves image edges and detail while smoothing
only the color-grading transitions.

---

## 13. Auto-Detection Algorithm

### Goal

Find the predefined harmony rule and anchor hue that already best explain the
image's existing color distribution — i.e., the combination requiring the least
correction.

### Input: Chroma-weighted hue histogram

Built during `_update_histogram()` (runs on the preview pipe):

```c
// For each pixel with chroma > 0.01:
local_histo[hue_bin] += chroma;  // Weight by saturation — vivid colors matter more
```

360 bins, one per degree of UCS hue.

### Scoring: `_score_harmony()`

For a candidate (rule, anchor_hue):
1. Compute harmony nodes
2. For each histogram bin with energy:
   - Find the maximum Gaussian weight across all nodes (using the same sigma
     formula as the main algorithm with `pull_width = 1.0`)
   - Accumulate `covered += histogram[bin] × max_weight`
3. Return `covered / total` — fraction of chromatic energy within attraction
   zones

### Grid search

```c
for each rule in [monochromatic, ..., square]:    // 9 rules
  for each anchor in [0°, 1°, ..., 359°]:         // 360 steps
    score = _score_harmony(smoothed_histogram, rule, anchor)
    track best
```

9 × 360 = 3,240 evaluations. Each evaluation iterates over 360 bins × up to 4
nodes = ~1,440 operations. Total: ~4.7M floating-point ops — completes in
under 1ms on a modern CPU.

### Histogram smoothing

Before scoring, the histogram is smoothed with 3 passes of a circular 3-tap
box filter to suppress noise from individual pixels:

```c
for(int pass = 0; pass < 3; pass++)
  for(int b = 0; b < 360; b++)
    tmp[b] = (smooth[b-1] + smooth[b] + smooth[b+1]) / 3;
```

Three passes of a 3-tap box filter approximate a Gaussian with σ ≈ 1.2 bins
(~1.2°). This prevents a single bright pixel from biasing the detection.

---

## 14. File & Line Reference

| Component | File | Lines |
|---|---|---|
| Params struct | `src/iop/colorharmonizer.c` | 68–79 |
| Pipe data struct | `src/iop/colorharmonizer.c` | 82–88 |
| `get_weighted_hue_shift()` (CPU) | `src/iop/colorharmonizer.c` | 284–326 |
| `get_weighted_hue_shift()` (CL) | `data/kernels/colorharmonizer.cl` | 35–78 |
| `wrap_hue()` (CPU) | `src/iop/colorharmonizer.c` | 327–332 |
| `wrap_hue()` (CL) | `data/kernels/colorharmonizer.cl` | 22–27 |
| `get_harmony_nodes()` | `src/iop/colorharmonizer.c` | 342–365 |
| `commit_params()` | `src/iop/colorharmonizer.c` | 366–376 |
| `process()` — fused path | `src/iop/colorharmonizer.c` | 445–488 |
| `process()` — two-pass path | `src/iop/colorharmonizer.c` | 490–574 |
| `process_cl()` | `src/iop/colorharmonizer.c` | 680–786 |
| `colorharmonizer_map` kernel | `data/kernels/colorharmonizer.cl` | 81–118 |
| `colorharmonizer_apply` kernel | `data/kernels/colorharmonizer.cl` | 120–157 |
| Harmony geometry table | `src/common/color_harmony.h` | 83 |
| RYB control points | `src/common/color_ryb.h` | 37–41 |
| LUT building | `src/iop/colorharmonizer.c` | 641–656 |
| `_ucs_to_ryb_fast()` | `src/iop/colorharmonizer.c` | 621–626 |
| `_ryb_to_ucs_fast()` | `src/iop/colorharmonizer.c` | 630–635 |
| `_score_harmony()` | `src/iop/colorharmonizer.c` | 1321–1357 |
| `_auto_detect_harmony()` | `src/iop/colorharmonizer.c` | 1359–1402 |
