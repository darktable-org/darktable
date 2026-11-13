# XYZ consumer comparison, November 9, 2026

PR: https://github.com/darktable-org/darktable/pull/22888

## Finding

A concrete luminance-only Reinhard operator can preserve neutrals and handle
gamut limits correctly using XYZ. It does not require XYZ pipeline support.
The buffered module-local version matched pipeline conversion output exactly
in this experiment. An equivalent RGB implementation avoided XYZ conversion
and was faster in this CPU benchmark. This is evidence against using this
operator to justify the existing infrastructure proposal.

This is a standalone algorithm experiment, not a production IOP, a new user
feature, or a full pixelpipe integration test. The source and measurements are
recorded here for review. The implementation proposal still needs an agreed
consumer; this operator does not establish that need.

## Concrete algorithm

For positive scene luminance Y, apply the global luminance-only Reinhard curve:

    Yout = Y / (1 + Y)
    XYZout = XYZ / (1 + Y)

Scaling all three coordinates by the same factor preserves xy chromaticity
before gamut mapping. The experiment maps nonpositive Y to black while
preserving alpha. That is an explicit prototype policy, not an assertion that
clipping negative luminance is appropriate for every production algorithm.

Under D50, a neutral follows (X,Y,Z) = Y * (0.9642, 1, 0.8249).
The test verifies those ratios from the actual ICC-derived profile matrices.
The curve preserves that ray. It does not converge toward equal XYZ channels.
An equal-XYZ negative control remains nonneutral in working RGB: the channel
spread after processing was 0.1214 for Rec.2020 and 0.1390 for Rec.709.

## Gamut handling

Gamut mapping happens in the selected linear working RGB profile, not by
clipping XYZ coordinates or treating the XYZ octant as a visible gamut.
For this SDR-target experiment the target bounds are RGB [0,1]. First convert
the toned XYZ to working RGB. Let w be the luminance row of that profile's
RGB-to-XYZ matrix. The neutral with the same Y is g = Y / sum(w). Contract the
color along the ray from (g,g,g) to the original RGB, choosing the largest
t in [0,1] that places every component within [0,1].

    RGBout = g + t * (RGB - g)

Both endpoints have the same Y, so contraction preserves Y. Gamut mapping may
change xy and perceptual hue; no perceptual hue preservation is claimed.
The working RGB gamut is not necessarily the final export or display gamut.
Real production output mapping would need an agreed target and rendering intent.
The experiment does not test spectral-locus membership or invent a physical
meaning for imaginary XYZ colors. Deliberately imaginary and negative-coordinate
stress samples exercise the boundary policy.

## Implementations compared

1. Fused module-local RGB-to-XYZ, curve, XYZ-to-RGB, gamut mapping in one loop.
2. Buffered module-local conversion using existing darktable inline helpers.
3. RGB-to-XYZ and XYZ-to-RGB through the PR's actual CPU colorspace dispatcher,
   with separate curve and RGB gamut passes.
4. RGB equivalent: compute Y from the profile luminance row, multiply RGB by
   1/(1+Y), then apply the same gamut mapping.

The RGB equivalence follows from linearity: M(s * RGB) = s * M(RGB).
Therefore this particular luminance curve cannot demonstrate a need for an XYZ
processing type, even though its definition naturally uses XYZ luminance.
The fused version's slower result also shows why loop layout and vectorization
must be controlled before attributing a difference to pipeline architecture.

## Actual validation

Compiled successfully with the existing native C toolchain and build flags,
including warnings as errors. The three profile source/header snapshots are
from PR head 9c9c12b7a025bbf078e74be395198dc1ea0d2059. They are compiled against
the existing CPU build and current checkout headers at
412ea7f6b8b8dc71905687ba4dbcf4a9d1909812. The test inputs and measurements are recorded in results.json.
Only the standalone executable overrides the profile implementation; the
installed library and plugins are untouched.

Correctness checks passed for linear Rec.2020 and linear Rec.709 at:

- 131,072 pixels with one and four OpenMP threads;
- 2,097,152 pixels per profile with one thread.

Inputs include a neutral ramp from black through 64, HDR and negative RGB,
equal XYZ coordinates, negative X/Z, zero and negative Y, and deterministic
random RGB spanning [-2,18]. Alpha was bit-preserved by all four versions.
Outputs were checked for finiteness and RGB gamut bounds (3e-5 tolerance).
The 2.1MP runs each exercised over 1.89 million gamut contractions and over
33,000 samples with nonpositive Y. Gamut contraction preserved Y, and the
tone stage preserved xy using a cross-product check, including near-black
samples without unstable xy division.

Maximum absolute errors on the 2.1MP inputs:

| Working profile | Fused vs dispatcher | Buffered vs dispatcher | Neutral channel spread | Output Y error |
| --- | ---: | ---: | ---: | ---: |
| linear Rec2020 | 1.36e-06 | 0 | 2.38e-07 | 3.28e-07 |
| linear Rec709 | 1.73e-06 | 0 | 1.79e-07 | 3.28e-07 |

The RGB equivalent differed from dispatcher output by at most
6.75e-06 in an RGB channel.
These are floating-point differences, not image delta-E measurements.

## CPU timings

Milliseconds, median of 15 timed iterations, rotating execution order,
one thread, 2,097,152 pixels per profile. Buffers allocated before timing.

| Working profile | Fused local XYZ | Buffered local XYZ | Dispatcher XYZ | RGB equivalent |
| --- | ---: | ---: | ---: | ---: |
| linear Rec2020 | 71.606 | 51.307 | 54.805 | 45.157 |
| linear Rec709 | 71.117 | 52.134 | 56.606 | 47.230 |

These are microbenchmarks on this shared cloud CPU. They exclude full
pixelpipe scheduling, cache behavior, blending, pickers, tiling, and device
transfers. They do not establish a general speed advantage for either design.
The result provides no performance justification for requiring central XYZ
dispatch for this operator; local buffering and RGB algebra are alternatives.

## Limits and next decision

No new GUI module, full-image export, integration reference comparison,
OpenCL execution, dedicated GPU, macOS, or Windows run was performed for this
experiment. Nonlinear and nonmatrix ICC profiles were not tested. These measurements do not constitute a full build or current-head CI validation.

Do not add a new tone-mapping module merely to give the infrastructure a caller.
Before further production work, agree on an algorithm users need and show why
local conversion or equivalent RGB/LMS math is insufficient. This candidate
fails that necessity test. The existing infrastructure proposal remains subject to that scope decision.

## Reproduce

Build this branch on Linux with OpenMP, `BUILD_TESTING=ON`,
`CMAKE_EXPORT_COMPILE_COMMANDS=ON`, and the Ninja generator.
The runner reuses the native test compiler and linker
commands; it does not add dependencies or modify the build configuration.

    python3 dev-doc/experiments/xyz-consumer/run.py /path/to/build --threads 1 --pixels 2097152
    python3 dev-doc/experiments/xyz-consumer/run.py /path/to/build --threads 4 --pixels 131072

The source checkout and built library must match. The original measurements
used the standalone source-snapshot setup described above; this runner is a
recipe for repeating the comparison after building the PR branch. It does not
recreate the original cloud machine or promise identical timings.

## Follow-up branch checks

After removing the duplicated picker declaration and overlapping fallback call,
the six affected core translation units compiled with OpenCL disabled and enabled:
color picker, histogram, profile conversion, blending, image operations, and
pixelpipe. The standalone consumer source also compiled against the updated
branch headers in both configurations. The runner's argument parsing and Python
syntax were checked. A complete build/link, full-image exports, and GPU execution
were not repeated for this follow-up. Current-head CI remains authoritative for
those platforms and full build checks.
