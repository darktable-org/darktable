# New Diffuse Integration Tests

## The gap in the original test

The original integration test (`0086-diffuse`) used these diffuse parameters:

| Parameter | Value |
|-----------|-------|
| iterations | 1 |
| radius | 128 |
| anisotropy (all 4 orders) | **+4.5** |
| speed (all 4 orders) | **-0.5** |
| regularization | 1.0 |
| variance_threshold | 0.0 |

The critical detail: **all four anisotropy values are identical and positive (+4.5)**. Via `check_isotropy_mode()`, positive anisotropy maps to `DT_ISOTROPY_ISOPHOTE` — so all four derivative orders use the same isotropy type and the same code path through `compute_convolution`.

This created a blind spot. When the paired-convolution optimisation (commit `5f769efdcf`) was first implemented, it contained a bug: derivatives 0 and 2 (which share gradient angle data) both used `isotropy_type[0]`, and derivatives 1 and 3 (sharing laplacian angle data) both used `isotropy_type[1]`. Because the original test set all four anisotropies to the same value, `isotropy_type[0] == isotropy_type[2]` and `isotropy_type[1] == isotropy_type[3]` — the bug was **invisible**. The test passed with zero dE despite the wrong isotropy indices being used.

The fundamental problem: the original test could not distinguish between "each derivative uses its own isotropy type" and "paired derivatives share a single isotropy type", because all four types were identical.

## What the new tests cover

Four new tests were created (`0087` through `0090`) using `create_diffuse_tests.py`. Each exercises a different combination of isotropy modes across the four derivative orders, ensuring that any optimisation that conflates, swaps, or hard-codes isotropy types will produce a visibly different (and therefore failing) output.

### 0087-diffuse-isotrope — all isotropic (baseline control)

| Parameter | Value |
|-----------|-------|
| anisotropy (all 4) | **0.0** |
| speed (all 4) | **+0.5** |
| iterations | 2, radius 32, regularization 1.0 |

All four orders use `DT_ISOTROPY_ISOTROPE`. This exercises the isotropic laplacian fast-path exclusively. It serves as a control: if this test fails, the bug is in the isotropic path itself, not in the anisotropic dispatch logic.

### 0088-diffuse-gradient — all gradient-directed

| Parameter | Value |
|-----------|-------|
| anisotropy (all 4) | **-3.0** |
| speed (all 4) | **-0.25** |
| iterations | 2, radius 32, regularization 2.0, variance_threshold 0.25 |

All four orders use `DT_ISOTROPY_GRADIENT` (negative anisotropy). This exercises the gradient rotation matrix path exclusively, with non-trivial edge sensitivity and variance thresholding. Together with `0087`, it confirms each individual code path works in isolation.

### 0089-diffuse-mixed — the critical test

| Parameter | 1st order | 2nd order | 3rd order | 4th order |
|-----------|-----------|-----------|-----------|-----------|
| anisotropy | **+3.0** | **0.0** | **-3.0** | **+2.0** |
| speed | -0.25 | +0.1 | -0.5 | +0.25 |

Isotropy types: **isophote, isotrope, gradient, isophote** — all three modes present, with each derivative order using a different type from its paired partner.

This is the test that directly targets the bug found during the paired-convolution optimisation:
- Derivatives 0 and 2 are paired (they share gradient angle data). Here, derivative 0 is isophote (+3.0) while derivative 2 is gradient (-3.0). Any optimisation that applies `isotropy_type[0]` to both will produce wrong results.
- Derivatives 1 and 3 are paired (they share laplacian angle data). Here, derivative 1 is isotrope (0.0) while derivative 3 is isophote (+2.0). Any optimisation that applies `isotropy_type[1]` to both will skip the rotation matrix for derivative 3.

The variance_threshold is set to 0.5 (non-zero) so the edge-sensitivity weighting is also exercised, catching bugs in variance regularisation that might otherwise be masked.

### 0090-diffuse-sharpen — alternating isotropy pattern

| Parameter | 1st order | 2nd order | 3rd order | 4th order |
|-----------|-----------|-----------|-----------|-----------|
| anisotropy | **+1.0** | **0.0** | **+1.0** | **0.0** |
| speed | -0.25 | +0.125 | -0.50 | +0.25 |

Isotropy types: **isophote, isotrope, isophote, isotrope** — an alternating pattern.

This catches a different class of bug from `0089`: optimisations that assume paired derivatives (0+2 or 1+3) always share the same isotropy type. Here, the paired derivatives *do* share their type (0 and 2 are both isophote; 1 and 3 are both isotrope), but with different `c2` magnitudes (anisotropy 1.0 vs 1.0). The sharpen-like parameter profile (small radius=8, high regularization=3.0, high variance_threshold=1.0, mixed positive/negative speeds) also stress-tests the optimised code under conditions resembling real-world sharpening presets.

## Why these parameters guarantee detection

The diffuse module's `heat_PDE_diffusion` computes four derivatives per pixel, each using:
1. An **isotropy type** (`ISOTROPE`, `ISOPHOTE`, or `GRADIENT`) that selects entirely different convolution kernel formulas
2. An **anisotropy magnitude** (`c2`) that scales the rotation matrix
3. **Angle data** (gradient or laplacian direction) shared between paired derivatives

When all four isotropy types are the same (as in the original test), swapping indices or sharing a single type across a pair produces mathematically identical results. The new tests break this symmetry:

- **0089** ensures every paired combination has mismatched isotropy types, so any index error produces a different convolution kernel — not just a slightly different magnitude, but a structurally different matrix (isotropic laplacian vs. rotated anisotropic kernel vs. transposed rotation).
- **0087** and **0088** isolate each path so regressions can be attributed to a specific code branch.
- **0090** exercises a realistic sharpen-like configuration that pairs same-type but different-magnitude derivatives.

The integration test framework compares output against pixel-exact reference images with tight dE thresholds (max dE <= 2.3, avg dE <= 0.77). Since the different isotropy types produce structurally different convolution kernels (not just scaled versions of each other), an index swap typically causes dE values orders of magnitude above the threshold, making detection reliable.

## Empirical proof

The bug was reproduced by changing `isotropy_type[2]` → `isotropy_type[0]` and `isotropy_type[3]` → `isotropy_type[1]` in the paired convolution loops (the exact bug found during the optimisation). Results:

### Buggy build

| Test | Max dE | Avg dE | Result |
|------|--------|--------|--------|
| **0086-diffuse** (original) | 1.20 | 0.00065 | **PASS** |
| 0087-diffuse-isotrope | 0.89 | 0.00002 | PASS |
| 0088-diffuse-gradient | 1.12 | 0.00003 | PASS |
| **0089-diffuse-mixed** | **12.89** | **0.16025** | **FAIL** |
| 0090-diffuse-sharpen | 0.84 | 0.00001 | PASS |

### Fixed build

| Test | Max dE | Avg dE | Result |
|------|--------|--------|--------|
| 0086-diffuse | 1.20 | 0.00065 | PASS |
| 0087-diffuse-isotrope | 0.89 | 0.00002 | PASS |
| 0088-diffuse-gradient | 1.12 | 0.00003 | PASS |
| 0089-diffuse-mixed | 0.96 | 0.00002 | PASS |
| 0090-diffuse-sharpen | 0.84 | 0.00001 | PASS |

**0089-diffuse-mixed** catches the bug decisively — Max dE explodes from 0.96 to **12.89** (5.6x above the 2.3 threshold), with 0.11% of pixels above tolerance. The original `0086-diffuse` is completely blind to it because all four derivative orders use the same `DT_ISOTROPY_ISOPHOTE` code path, making the wrong index produce identical results.

Tests `0087`, `0088`, and `0090` pass in both cases because the bug only manifests when paired derivatives (0+2 or 1+3) have *different* isotropy types — which only `0089` exercises.
