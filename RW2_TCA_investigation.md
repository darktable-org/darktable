# Panasonic RW2 TCA compensation: investigation notes

Working document for the follow-on agent. Records what was found, what was
verified from the tree, and what is still speculation. Do not treat as a
design document.

## TL;DR

- A regression exists on master: since `47c223703e` (2026-07-27, "Add Panasonic
  RW2 embedded lens distortion correction"), the lens module auto-selects the
  *embedded metadata* correction method for any Panasonic RW2 where the camera
  wrote its DistortionInfo tag. The embedded-metadata path for Panasonic is
  distortion-only, so TCA (which was previously being handled by Lensfun in
  the default configuration) silently stops being applied on freshly-imported
  or module-reset RW2 files.
- The immediate user-visible fix is to switch the lens module's *correction
  method* from *embedded metadata* to *Lensfun database*. Lensfun's own TCA
  calibration (if present for the lens) then applies as before.
- The RW2 does carry chromatic-aberration correction data. It lives in
  `Exif.PanasonicRaw.0x011b` (64 bytes, 32 x int16 LE, single record with
  four checksums). darktable has never parsed it. Structural layout is
  substantially known from prior RE (Rigo 2011, Homeister 2018 on ExifTool
  forum 9366) and verified against our sample; per-zone coefficient
  semantics are the remaining decode gap.
- Follow-on work is (a) shoot a small RW2 corpus with focal-length and lens
  variation, (b) finish the coefficient decode against SILKYPIX renders,
  (c) extend the Panasonic embedded-metadata path in `_init_coeffs_md_v2`
  to produce non-identical R/B coefficients.

## Reproducer

1. Panasonic body that writes DistortionInfo (verified on Panasonic G9, sample
   file used during the investigation: `P1366403.RW2`, 5184x3888, LUMIX G
   42.5/F1.7 lens, DistortionCorrection = On).
2. Import into master built at or after `47c223703e`. Default history stack;
   do not touch the lens module.
3. Inspect the module UI: *correction method* reads *embedded metadata*.
4. Inspect the output for red/blue fringing at high-contrast edges near the
   image periphery. Compare with the same file processed on a pre-2026-07-27
   build (or with the method manually switched to *Lensfun database*).

I did **not** run this reproducer myself. The chain of causality is derived
from reading `src/iop/lens.cc`, not from a rendered pixel comparison. The
follow-on agent should run `darktable-cli` on the sample RW2 with both
methods and diff the outputs before treating the regression as confirmed;
see the Testing section for the exact commands.

## Root cause chain

All references are to `src/iop/lens.cc` at HEAD (`6a2f19f4e3`, 2026-09-15).

1. `src/common/exif.cc` (extended in `47c223703e`) now parses
   `Exif.PanasonicRaw.0x0119` (DistortionInfo). When the "on" flag is set it
   assigns `img->exif_correction_type = CORRECTION_TYPE_PANASONIC` and
   populates `img->exif_correction_data.panasonic` with `a, b, c, scale`.
2. `_have_embedded_metadata(self)` (`lens.cc:290`) returns true for any
   `exif_correction_type != CORRECTION_TYPE_NONE`.
3. `reload_defaults()` (`lens.cc:3587`):
   ```c
   d->method = DT_IOP_LENS_METHOD_LENSFUN;
   if(_have_embedded_metadata(self))
   {
     d->method = DT_IOP_LENS_METHOD_EMBEDDED_METADATA;
     d->md_version = DT_IOP_LENS_EMBEDDED_METADATA_VERSION_2;
     d->scale_md = 1.0f;
   }
   ```
   Any Panasonic RW2 with the flag set now defaults to embedded-metadata mode.
4. `_get_method()` (`lens.cc:3275`) only downgrades embedded-metadata -> Lensfun
   when `_have_embedded_metadata()` is false. It does not know that the
   Panasonic branch of embedded-metadata is TCA-incapable, so no fallback is
   triggered.
5. `_init_coeffs_md_v2()` (`lens.cc:2417`, Panasonic branch added by
   `47c223703e`) computes a single geometric radial multiplier:
   ```c
   cor_rgb[0][i] = cor_rgb[1][i] = cor_rgb[2][i] = fine;
   ```
   All three channels share one coefficient -> zero differential TCA.
6. Sony/Fuji/DNG/Olympus branches in the same function *do* produce distinct
   per-channel coefficients from their respective CA fields
   (`sony.ca_r/ca_b`, `fuji.ca_r/ca_b`, `dng.cwarp[3]`, `olympus.ca[]`), so
   they are unaffected. The Panasonic struct in `src/common/image.h`
   deliberately has no CA fields; that is the gap this document is about.

Before `47c223703e`, Panasonic RW2 imports produced
`exif_correction_type == CORRECTION_TYPE_NONE`, so `_have_embedded_metadata()`
returned false, `reload_defaults()` left the method at
`DT_IOP_LENS_METHOD_LENSFUN`, and Lensfun handled TCA if it had a profile for
the lens. The regression is therefore not "TCA stopped working" in the code
sense but "the module's default correction method changed for Panasonic RW2,
to a method that has no TCA path".

## History context (verified)

- Commits touching `src/iop/lens.cc` between `47c223703e` and HEAD: six, all
  GUI/plumbing (`git log --oneline 47c223703e..HEAD -- src/iop/lens.cc`).
  None touches the correction algorithm.
- Full-history pickaxe (`git log --all -S ...`) for Panasonic CA/TCA handling:
  only `47c223703e` ever touched Panasonic-specific correction code. There is
  no prior version of darktable in the tree's history that parsed 0x011b.
- Consequence: any user memory of "RW2-based TCA correction working in an
  earlier darktable release" cannot be explained by this repository. The
  likely explanation is Lensfun (before the default flipped) or a different
  tool.

## Immediate workaround (for users)

In the lens correction module, set *correction method* to *Lensfun database*.
`has_been_set` becomes true, and the choice sticks in history. This does not
require a rebuild.

## What the RW2 actually contains

Parsed IFD0 of the sample directly (not via ExifTool, which truncates unknown
undefined-type values in text output; see `-b -j` for the full base64):

```
tag=0x0119 type=UNDEFINED count=32   DistortionInfo   (darktable parses)
tag=0x011a type=SHORT      count=1   value=2                    (unknown)
tag=0x011b type=UNDEFINED count=64   <- chromatic aberration     (NOT parsed)
tag=0x011c type=SHORT      count=1   value=598          (Gamma per ExifTool)
tag=0x011d type=UNDEFINED count=102  all zero on the sample     (reserved?)
tag=0x011e type=UNDEFINED count=18                              (unknown)
tag=0x011f type=UNDEFINED count=258                             (unknown)
tag=0x0120 type=UNDEFINED count=2048 CameraIFD (sub-TIFF)
tag=0x0121 type=LONG       count=1   Multishot
```

`0x011b` on the sample, 64 bytes = 32 x int16 LE, hex:

```
fa2d 51c3 0d04 4800 aa0a 0004 8054 3f2f
bb06 8808 3bf9 cc0c 4a02 bfbc 0001 1801
8808 4404 8888 0007 d4f6 2002 8002 1105
c818 1167 6508 f8fc 0040 63f4 47a9 abe5
```

Structurally this is *one* 64-byte record with four checksums, not two
DistortionInfo-shaped blocks. Both prior RE efforts agree on the checksum
layout, and it verifies cleanly against our sample:

| word | role                            | our sample     |
|-----:|:--------------------------------|:---------------|
| 0    | CRC over data bytes 4..59, evens| `0x2dfa` (OK)  |
| 1    | CRC over data bytes 4..31       | `0xc351` (OK)  |
| 30   | CRC over data bytes 32..59      | `0xa947` (OK)  |
| 31   | CRC over data bytes 4..59, odds | `0xe5ab` (OK)  |

CRC polynomial is `csum = (73 * csum + byte) mod 0xFFEF`, identical to
0x0119. The two "half" checksums cover the two 28-byte halves; the two
"striped" checksums cover even- and odd-indexed data bytes across the whole
payload. Rigo's `parseca.c` implements this exactly (see References).

Homeister's forum post (retrieved via web.archive.org since the ExifTool
forum was intermittently down during this investigation) documents the
interior layout for MFT-sensor Panasonic bodies. Translated to 0-indexed
words:

| word | Homeister role          | our sample | interpretation on sample     |
|-----:|:------------------------|-----------:|:-----------------------------|
| 2    | interior CRC (unknown)  |       1037 | opaque                       |
| 7    | interior CRC (unknown)  |      12095 | opaque                       |
| 13   | interior CRC (unknown)  |     -17217 | opaque                       |
| 11   | full radius N1          |       3276 | normalization denominator    |
| 4    | radius N2               |       2730 | 2730/3276 = 0.8333           |
| 16   | radius N3               |       2184 | 2184/3276 = 0.6667           |
| 17   | radius N4               |       1092 | 1092/3276 = 0.3333           |
| 14   | on/off flag             |        256 | non-zero == correction "on"  |

The 1.0 / 0.833 / 0.667 / 0.333 radius progression matches Homeister's
Micro Four Thirds pattern verbatim, so our G9 file behaves the same way
his GX-8 did.

Homeister's model divides the payload into three "parts" of remaining
words, grouped by whatever role they play in the correction:

- part 1: 0-indexed words [3..6]
- part 2: 0-indexed words [8..12]
- part 3: 0-indexed words [14..29]

Concrete word-by-word role assignments *within* those parts are the next
gap. Homeister himself stopped at the radii and the flag; ExifTool has
never landed a decoded structure. This is where the RW2 corpus + SILKYPIX
comparison starts paying off.

Homeister also asserts that 0x011b covers both distortion *and* CA (with
0x0119 being a legacy simpler form used only by cams that don't emit
0x011b), and that `0x011a` is a selector: `1` on old cams (use 0x0119),
`2` on new cams (use 0x011b). Our G9 sample has `0x011a = 2`. That claim
conflicts with the ExifTool `PanasonicRaw.pm` comment "chromatic aberration
correction" and needs its own verification before we build a struct
assuming it. See "Remaining unknowns".

For legacy reference, the DistortionInfo (0x0119) layout in `47c223703e` is
a 16-word record with checksums at [0..1], on/off nibble in low byte of
[7], polynomial coefficients at [4], [8], [11], scale at [5] and a
constant `DistortionN` at [12].

Upstream reference (ExifTool `Image::ExifTool::PanasonicRaw`, `Main` tag
table):

```perl
# 0x11b - chromatic aberration correction (ref 3) (also see forum9366)
```

`ref 3` and `forum9366` are the ExifTool documentation reference and the
u.exiftool.org thread where Homeister posted his findings. ExifTool ships
no decoded structure for the tag; only Rigo's checksum code in
`parseca.c` and Homeister's forum post carry the RE.

## Where the code needs to change

For the follow-on agent, this is the minimum set of touchpoints, all
concentrated:

### 1. `src/common/image.h`

Extend `dt_image_correction_data_t::panasonic` with 0x011b fields. Given
Homeister's 4-zone model, a per-zone representation is a better fit than
the per-channel-polynomial draft in an earlier version of this document.
Draft (subject to the coefficient-decode outcome):

```c
struct {
  // from 0x0119 (already parsed by 47c223703e)
  float a, b, c;
  float scale;

  // from 0x011b (new)
  gboolean has_v2;                 // true when 0x011a == 2 and 0x011b present
  gboolean v2_on;                  // word[14] non-zero
  float n1, n2, n3, n4;            // normalized radii (n1 == 1.0 by definition)
  // per-zone coefficient sets go here once decode is complete; likely
  // three groups of 4-5 floats each, indexed by part (Homeister's parts
  // 1/2/3), with a per-channel split (R/G/B or R-vs-G / B-vs-G) that we
  // still have to establish
} panasonic;
```

Do not commit the `n2/n3/n4` fields yet: parse them alongside the
coefficients they modulate, in one go, once we know what each word does.
This is a placeholder for the follow-on agent, not a design commitment.

### 2. `src/common/exif.cc`, `_check_lens_correction_data()`

The current Panasonic branch reads `Exif.PanasonicRaw.0x0119` and also handles
a TIFF/DNG round-trip via `Exif.Image.0xf119` (a private-range alias the same
commit added in `dt_exif_read_blob`). The 0x011b reader has to mirror both:

- read `Exif.PanasonicRaw.0x011b` (undefined, 64 bytes),
- populate the new struct fields,
- add the same `dt_exif_read_blob` copy so 0x011b survives a TIFF/DNG round
  trip (pick a numeric ID in TIFF's private range analogous to `0xf119`,
  e.g. `0xf11b`; verify no collision with other private tags in exiv2 by
  running `_check_lens_correction_data` against a round-tripped file).

### 3. `src/iop/lens.cc`, `_init_coeffs_md_v2()`

The Panasonic branch currently sets all three RGB channels to the same
`fine` multiplier. Replace with a per-channel evaluation:

```c
// R and B get their own polynomials from 0x011b; G stays on the geometric
// distortion; darktable's TCA model corrects R and B relative to G
if(cor_rgb && (p->modify_flags & DT_IOP_LENS_MODIFY_FLAG_TCA)
   && cd->panasonic.has_ca)
{
  // evaluate 3rd/5th/7th-order polynomial for R and B separately, apply
  // p->cor_ca_r_ft / p->cor_ca_b_ft fine-tune the same way Lensfun mode does
}
```

The G channel keeps whatever the distortion branch computed. Do not merge
the two into one loop until the RE outcome is stable.

Consider also `p->tca_override` in the Lensfun path: on the metadata path,
the user has no way today to override the coefficients. If parity with the
Lensfun UI matters, the follow-on agent should think about whether tca_r /
tca_b / tca_override behave sensibly here too, or whether the metadata path
should ignore them.

### 4. Legacy params

`_init_coeffs_md_v2` name suggests versioning (`v2`). Confirm whether adding
CA support requires bumping to `v3` (i.e. new
`DT_IOP_LENS_EMBEDDED_METADATA_VERSION_3`): probably yes, so that a history
stack made pre-CA on an image whose Panasonic CA data would now be applied
does not silently gain a new correction on re-open. Existing history entries
should replay with v2 behavior; only new imports should default to v3.

Look at how the DNG/Fuji/Sony branches version themselves (search
`md_version`, `DT_IOP_LENS_EMBEDDED_METADATA_VERSION_1`,
`DT_IOP_LENS_EMBEDDED_METADATA_VERSION_2`): the pattern is already there.

### 5. UI

Nothing structural. The `modify_flags` menu already exposes TCA on/off and
the mixed distortion/TCA/vignetting combinations. Once
`_init_coeffs_md_v2` produces non-identical R/B coefficients on Panasonic,
the existing checkbox is functional.

### 6. Fallback

Reconsider `_get_method()` (`lens.cc:3275`). Regardless of whether 0x011b
gets decoded, it would be defensible to fall back to Lensfun for TCA when
the metadata path lacks CA data and the user has TCA in `modify_flags`. That
is a smaller, safer change that resolves the regression even without RE
work. Two possible approaches, either as an interim fix before the RE work
lands or in addition to it:

- Coarse: for `CORRECTION_TYPE_PANASONIC` specifically, keep the pre-commit
  default (Lensfun) until 0x011b is decoded. One-line change in
  `reload_defaults()`.
- Fine: in `commit_params`, if the effective method is metadata *and* the
  current correction_type cannot handle TCA *and* `modify_flags` requests
  TCA, run the Lensfun TCA on top. This is more work but preserves the
  distortion improvement `47c223703e` brought.

The coarse option is the honest "revert the regression" path. Do not
combine it with the RE work in one commit.

## Verified structure and remaining unknowns

Verified against `P1366403.RW2` (see "Sample file used"):

- checksum polynomial and four-checksum layout (Rigo's `parseca.c`),
- four radial-zone boundaries N1..N4 at words [11], [4], [16], [17] with
  MFT ratios 1.0 / 0.833 / 0.667 / 0.333 (Homeister),
- on/off flag at word [14],
- selector `0x011a == 2` on Panasonic G9 (Homeister),
- three "opaque" interior words [2], [7], [13] that neither Rigo nor
  Homeister decoded; suspected inner CRCs.

Still open:

- **Coefficient assignments.** Which of the remaining data words in each
  of Homeister's parts are which polynomial coefficient, and how they
  group by radial zone. Homeister's 2018 post stops at the radii.
- **Distortion vs CA.** Homeister claims 0x011b covers both distortion and
  CA; ExifTool's tag comment says "chromatic aberration correction". If
  Homeister is right, then a G9 using 0x011b for distortion means our
  current 0x0119-based distortion path is being applied on top of a signal
  already corrected by whatever the camera did, which is wrong. Verify by
  rendering the same RW2 through SILKYPIX with in-camera distortion
  correction on vs off and comparing radial geometry.
- **Polynomial form.** 0x0119 is `Ru = Rd + s(a*Rd^3 + b*Rd^5 + c*Rd^7)`.
  A 4-zone version might be piecewise (a/b/c per zone) or a spline
  evaluated at 4 knots or a higher-order polynomial sampled at 4 radii.
  Discriminated by watching how the coefficient words change with focal
  length on a zoom.
- **Per-channel split.** For it to correct CA, some subset of words must
  differ between R and B (or between R and G, and B and G). We do not yet
  know which words those are.
- **Zoom-dependent variation.** Do words move continuously with focal
  length, or snap between a small number of discrete profiles that the
  camera picks by zoom range?
- **Correction "off" state.** If the user disables in-camera CA
  compensation, does 0x011b vanish, is word [14] cleared, or does the
  block persist with zeroed coefficients? We know word [14] is non-zero
  ("on") on our sample; we do not know the off state.

## Coefficient decode results

Corpus: nine RW2s from the same Panasonic G9 body, one per test scene,
covering three focal lengths on the Leica DG 12-60mm f/2.8-4 (12, 25,
60mm), three on the Lumix G 45-150mm f/4-5.6 (45, 97, 150mm), and one
each on the Lumix G 42.5mm f/1.7, Sigma 30mm f/1.4 DC DN and Sigma 16mm
f/1.4 DC DN. All nine files pass Rigo's four checksums and share the
verified structural layout above. Files live under `/c/temp/tca/` and
are deliberately not committed.

Measurement pipeline (scripts under `/tmp/rw2_tca/`):

- `step1_verify.py` extracts 0x011b and validates the four checksums;
- `step2_classify.py` labels each of the 32 words as
  checksum / body-constant / homeister-opaque / focal-varying / other;
- `step3_measure.py` renders the RW2 with rawpy AAHD, then measures per
  edge chromatic offset by centroid of |gradient|, projecting the R-G
  and B-G shifts along the local radial direction;
- `step4_measure_v2.py` replaces the centroid with a parabolic peak fit
  on the |gradient| profile, bilinear-samples the profile along the
  local gradient direction, requires the three channels' integer peak
  samples to agree within +/- 1 (they must be tracking the same edge),
  and rejects any per-edge offset above 2 px as a fit artifact. It also
  dumps a per-edge point cloud to `/tmp/rw2_tca/measurements.npz` with
  fields `file_idx, x, y, r, dr, db, gx, gy, radial_align`, sign
  convention: positive dr = R displaced outward from the optical
  center relative to G;
- `step4_fit.py`, `step4_fit_v2.py`, `step4_fit_alt.py`,
  `step4_shape_correlate.py` and `step4_final_summary.py` run the
  candidate-decoding search;
- `step4_diagnose.py` and `step4_plot_fits.py` write PNGs to
  `/tmp/rw2_tca/plots/`.

### Precision self-check

Half-vs-half bin-median RMS, averaged over five random splits at 15
radial bins per file, for the centroid and parabolic estimators on
three files:

| file                     | centroid R-G / B-G | parabolic R-G / B-G |
|:-------------------------|:-------------------|:--------------------|
| P1366486 Sigma 16 @ 16mm | 0.052 / 0.036 px   | 0.077 / 0.014 px    |
| P1366477 PL 12-60 @ 12mm | 0.029 / 0.025 px   | 0.022 / 0.016 px    |
| P1366483 L 45-150 @ 150 | 0.090 / 0.057 px   | 0.029 / 0.031 px    |

Parabolic reduces the per-bin sampling scatter on five of six
file/channel pairs and cuts the small-offset (B-G) RMS by about 2-3x,
at the cost of 40-60% fewer surviving edges after the peak-quality
filters. Bumping the initial edge count from 6000 to 15000 recovers a
comparable per-bin sample size. The Sigma 16 R-G case is the exception:
that file has by far the largest CA magnitudes (peak-to-peak 0.16 px)
and the parabolic estimator scatters more where consecutive R and G
integer peaks disagree at the outermost radii. On the aggregated
corpus, per-file half-vs-half RMS lands in the 0.008-0.031 px range for
seven of nine files, with two outliers (Sigma 16 R-G 0.18 px, and L
45-150 @ 60mm which has few strong edges).

### Aggregated point cloud

`/tmp/rw2_tca/measurements.npz` holds 37,381 edges across the nine
files, filtered to `|cos(gradient, radial)| > 0.3`. Per-file counts
range from 2,877 (L 45-150 @ 45mm) to 5,006 (Sigma 16). The signal is
plainly present: bin medians reach 0.13 px R-G at the outer knots on
PL 12-60 @ 12mm, 0.11 px on Sigma 30, -0.05 px on L 45-150 @ 150mm.
Diagnostic plots at `/tmp/rw2_tca/plots/ca_signal.png` and
`/tmp/rw2_tca/plots/smooth_words.png`.

### The 8 smooth words are not four R and four B zone heights

The task was to test whether the eight smooth words `[2, 8, 10, 12, 20,
23, 27, 29]` split into two ordered groups of four, each group holding
the four zone heights `(h_N4, h_N3, h_N2, h_N1)` for one of the R and B
polynomials in a piecewise-linear interpolant through knots at pixel
radii 1092 / 2184 / 2730 / 3276, with a single global scaling factor k
shared across all files and both channels. This is a natural fit to the
verified 4-zone radial layout.

The hypothesis fails.

- **Global k, fixed word-index-ascending knot order (70 splits).**
  Aggregate weighted RMS 0.0357 px against a null-model RMS of 0.0373
  px. The top ten splits differ by less than 0.4% in RMS, all lie
  within 4% of the null, and every top split assigns word[2] to the R
  side. The fitted k lands at 1.48e-6..1.49e-6 pixels per int16 unit,
  which is what the algorithm returns when word[2]'s 27000-level
  values are the only lever available.
- **Global k, all 4! knot permutations per group (40,320
  configurations).** Aggregate RMS drops to 0.0323 px; still within
  14% of null. Word[2] remains in every top-20 split.
- **Per-file gain, fixed knot order.** Each file gets its own k. RMS
  falls to 0.030 px, again driven by word[2] absorbing whatever
  amplitude the file happens to need. Independent evidence this is not
  a real fit: the per-file `k_R` values in the top split flip sign
  three times across the nine files (`+3.3e-6, +1.8e-6, -1.4e-6, -6.5e-6,
  -7.7e-6, -1.1e-5, -1.3e-6, +4.6e-5, +6.2e-5`), and their absolute
  values span roughly 50x. `k_B` sign-flips six times.
- **Per-file gain, all knot permutations.** RMS 0.024 px. Same sign
  pathology.
- **`word[2]` or `word[7]` used as a multiplicative per-file gain,
  words in ascending order.** RMS 0.0357 px, indistinguishable from
  the previous fixed-knot fit. Word[2] as a scaler and word[2] as a
  knot height do the same thing for the least-squares fit given how it
  dominates every file's numeric range.
- **Polynomial-in-r or Catmull-Rom spline through the same four
  knots.** R^2 stays negative for every 4-subset and every knot
  permutation, i.e. every polynomial variant does worse than
  predicting a per-file constant.

For orientation, the top-3 fits at fixed knot order and the top-3 fits
at permuted knot order are plotted at
`/tmp/rw2_tca/plots/fit_fixed_top{1,2,3}.png` and
`/tmp/rw2_tca/plots/fit_perm_top{1,2,3}.png`. Each figure has one
panel per file including the three the task calls out (Sigma 16, PL
12-60 @ 12mm, L 45-150 @ 150mm), with the measured R-G and B-G bin
medians overlaid on the fitted piecewise-linear curve. The plots make
the failure visually obvious: the fitted curve mismatches the
measurement in shape on most files and only "lands" when the per-file
gain compensates.

### What is actually confirmed at single-word level

The one strong prior signal survives: word[8] anti-correlates with the
sign of the measured R-G radial offset in 9 of 9 files. Multiplied by a
single negative scalar, word[8] alone predicts the sign of R-G
correctly on every file in the corpus. It does not predict the shape
of R-G with radius (which peaks between N3 and N2 on the Sigma 16 and
Sigma 30 samples and monotonically climbs then falls on the PL 12-60 @
12mm sample), so word[8] is at best one contribution to a more
elaborate model, not the whole R zone-height vector.

### Interpretation

The measured signal is real: the per-file half-vs-half RMS is 0.008 to
0.03 px on eight of nine files, well below the observed CA magnitudes,
and the shapes reproduce cleanly. The corpus is not the bottleneck.

The **piecewise-linear 4-zone model with the 8 smooth words as knot
heights and a single global k** is inconsistent with the data. Neither
does a single-polynomial variant. Something in the model is wrong.
Candidates, in order of what I would test next:

1. **The tag encodes distortion per channel, not CA.** 0x0119
   distortion is small for the PL 12-60 zoom (barrel at 12mm reaching
   ~9% at the corner) and zero-scale for the primes, so the raw
   sensor-space radii and the corrected radii are close on eight of
   nine corpus files. But if 0x011b instead stores three separate
   distortion polynomials, one per RGB channel, then CA is the
   *differential* between them, and any 4-subset of these words viewed
   as a "CA zone-height vector" is going to look inconsistent. This
   would also fit the observation that six of the seven focal-monotone
   smooth words (`[8, 10, 12, 20, 23, 29]`) march together with focal
   length while word[27] moves oppositely: three-poly-of-degree-N per
   channel plus one shared sign-inverted term is a natural read.
2. **Word[2] is a lens-family key.** Word[2] varies by 25000
   between the L 45-150 (~4000) and the PL 12-60 (~28000), and by
   about 1000 within a zoom sweep, so the range within a zoom is 2%
   of the between-lens range. Word[2] looks more like a lens ID or a
   lens-family baseline than a per-file coefficient. The fits above
   keep placing word[2] in the R group precisely because it dominates
   the numeric range and lets the least-squares fit lever the sign
   correctly on each lens family; that is a symptom of an ID being
   mis-used as a knot height.
3. **The words are not signed int16 in whole.** Sub-fields, packed
   fixed-point with implicit exponents, or byte-swapped pairs could
   change the interpretation. All of the fits above assume signed
   int16 LE, which matches Rigo's `parseca.c` for structural words
   and looks right for the checksums, but nothing forces it for the
   coefficient words.
4. **Missing per-lens or per-body normalization we did not identify.**
   Word[7] is a lens-family scaler (0x7FFF on the PL 12-60, 0x3FFF on
   the others). Dividing by it changes nothing in the fit above, but
   it might enter as a denominator in a more elaborate model.

Word[27] deserves a follow-up on its own. It is the only smooth word
whose sign flips systematically with focal *opposite* to the six
grouped words, and it is small in magnitude (< 400 across the whole
corpus). Its role is not decoded by any of the models tested here.

### Confidence and honest bounds

- The **measurement** is trustworthy on the files with strong CA
  (Sigma 30, Sigma 16, PL 12-60 at 12mm): per-file half-vs-half
  bin-median RMS 0.008-0.02 px against peak-to-peak signal 0.1-0.2 px,
  and the shape is stable across seeds. On the low-CA files (L 45-150
  @ 60mm, L 45-150 @ 97mm, PL 12-60 @ 60mm) the per-file RMS is a
  significant fraction of the amplitude, so shape claims on those
  files are correspondingly weaker.
- The **structural layout** is trustworthy: checksums, N1..N4 radii,
  on/off flag, lens-family scaler word[7], the identification of the 8
  smooth words vs 13 discrete words, and the observation that word[8]
  anti-correlates with R-G sign are all reproducible from the scripts
  above.
- The **decode** is not. No candidate assignment tested here fits the
  measured CA within measurement noise. The user should not treat any
  of the top splits as a decode. There is enough per-file gain
  freedom in the search that a superficially-good aggregate fit does
  not survive contact with per-file inspection.
- **Confidence in the negative result:** high, because the failure is
  robust across model variants, gain choices and knot orderings, and
  the per-file gains are unphysical (sign-flipping across files with a
  50x spread in magnitude).

### What would resolve it

Roughly in order of expected value:

1. **A file pair from the same lens and same focal length, with
   in-camera CA correction toggled on/off in the menu.** The
   difference isolates which words track the CA setting. Homeister
   never had this control; a G9 exposes it under its shading and
   color-fringe correction settings. If the CA setting only changes
   a small subset of words, the coefficient decode collapses to those.
2. **A file pair with in-camera *distortion* correction on/off,
   holding lens and focal fixed.** Discriminates whether 0x011b holds
   distortion coefficients (which change) or pure CA coefficients
   (which do not).
3. **The paired SILKYPIX SE output as a null-corrected reference.**
   SILKYPIX SE bundled with the LUMIX bodies applies Panasonic's own
   correction, so the difference between its "profile off" and
   "profile on" render is the exact function 0x011b encodes. The
   9-file corpus does include SILKYPIX TIFFs, but the SE build in use
   silently profile-corrects even in "no correction" mode, so we do
   not have the null in the current material. A build of SILKYPIX
   Developer Studio Pro on a Windows VM would produce the null.
4. **More lens diversity, especially another wide fast prime and
   another kit zoom.** A Panasonic 8mm fisheye and a Panasonic 14-42
   kit zoom would extend the lens-family distribution of word[2] and
   word[7]; a decode that survived four Panasonic zooms and three
   Sigma primes would be far more credible.
5. **A different body.** The Homeister decode was on a GX-8; the G9
   might have layout differences in the "opaque" words. A GH5 or S5
   corpus would tell us whether the roles are body-invariant.

Absent any of the above, an honest verdict is: 0x011b's coefficient
semantics remain undecoded on the current 9-file G9 corpus. The
structural findings above (checksums, radii, flag, opaque, smooth vs
discrete) are what should feed the darktable code changes described
under "Where the code needs to change"; the coefficient path should
remain a stub until the CA-toggle control shots or SILKYPIX-Pro
reference exists.

## Reverse-engineering next steps

Everything below is a plan for the follow-on agent. Nothing here has been
executed against a corpus.

### Corpus

The lens set available on the developer machine is well-suited to the
decode:

- Leica DG Vario-Elmarit 12-60mm f/2.8-4 (zoom, high-quality, likely
  strong CA characterization),
- Lumix G 45-150mm f/4-5.6 (zoom, kit-tier optics),
- Sigma 16mm f/1.4 DC DN Contemporary (prime, third-party),
- Sigma 30mm f/1.4 DC DN Contemporary (prime, third-party),
- (already) Lumix G 42.5mm f/1.7 (prime).

Requested shooting recipe (order of value):

1. On PL 12-60: shoot at 12mm, 25mm and 60mm from a tripod, same subject,
   same aperture (say f/5.6). Same-frame test chart preferred; a busy
   scene at infinity with edges in the corners also works.
2. On Lumix 45-150: same, at 45mm and 150mm.
3. One shot on each of the four primes for a lens-diversity baseline.
4. If the G9 exposes an in-camera "CA Correction" or "Colour Shading"
   toggle independently of the distortion toggle, one file each way on
   the same lens at the same focal length. This is the control that lets
   us confirm which words track the CA setting versus the distortion
   setting.

Everything stays under `/c/temp/` or another out-of-tree location. Do not
commit RW2s.

### Cross-check tools

- **SILKYPIX Developer Studio SE.** Panasonic's own converter, bundled
  with LUMIX bodies. The reference implementation. For each RW2, export a
  16-bit TIFF at neutral settings with in-camera corrections *on*, and a
  second TIFF with them turned off. The diff is the correction the camera
  wants applied.
- **darktable-cli.** Same file, method forced to Lensfun (via a saved
  preset) for our current TCA baseline; method left at the default for
  the regressed baseline.
- Optional: RawTherapee for a third baseline where the user manually
  matches CA sliders to SILKYPIX visually. Doubles the observations.

### Decode procedure

1. Verify checksums on every RW2 in the corpus (Rigo's algorithm). Reject
   any file whose checksums do not validate before drawing conclusions.
2. For each RW2, decode words [11], [4], [16], [17] as Ni radii and
   confirm the N1 / 0.833 / 0.667 / 0.333 pattern (or a different
   MFT-consistent one) so we know Homeister's model applies. If it does
   not on the third-party Sigmas, flag: the layout might be
   lens-brand-conditional.
3. Table all 32 words across the corpus. Words constant across all files
   are structural/version markers. Words varying with focal length on a
   zoom are focal-dependent coefficients. Words varying between lenses
   but not with focal length within a zoom are lens-constants. That
   3-way partition alone probably nails the roles.
4. For each SILKYPIX-vs-null diff, characterise the CA as a per-channel
   radial displacement `d_r(r)` and `d_b(r)`. Fit a polynomial. See
   whether the fitted coefficients recover the values in the RW2 (in
   whatever encoding: raw int16, scaled by N1, scaled by 32768, etc.).
5. Only once the fit is stable across corpus files, write the C decoder.

Prototype the whole numerical loop in Python before touching C. The
existing `exiftool -b -j` extraction plus the checksum snippet in this
document is a starting point.

### Rendering test

Once a candidate decoding exists, prototype the correction in a scratch
Python script that computes `Ru_r / f_r(Ru)` and `Ru_b / f_b(Ru)` and
demosaics with per-channel radial resampling. Compare against SILKYPIX
output. Only once the pixel diff is small (say dE < 2 over the frame)
should the C code be written.

## Testing

- Build: `./build.sh --prefix /opt/darktable-test --build-type Release`.
  Nothing in this change touches OpenCL or ifdef branches; both should still
  compile.
- Integration tests: this touches `src/iop/lens.cc` and the pixelpipe on
  Panasonic files, so `src/tests/integration/` is mandatory
  (`AGENTS.md` section Testing). Add a new fixture with a Panasonic RW2 and a
  known-good output rendered against a pre-`47c223703e` build if one is
  reachable; otherwise against SILKYPIX with a documented recipe.
- `darktable-cli` for quick loops:

  ```bash
  # baseline: force Lensfun method by using a saved preset/style
  darktable-cli /c/temp/P1366403.RW2 style_forcing_lensfun.xmp out_lf.tif
  darktable-cli /c/temp/P1366403.RW2 out_default.tif   # current default
  # once implemented:
  darktable-cli /c/temp/P1366403.RW2 out_new.tif       # with 0x011b decoded
  compare -metric AE out_lf.tif out_new.tif diff.png
  ```
- `-d imageio` prints whether `_check_lens_correction_data` picked up the
  Panasonic branch:

  ```bash
  darktable-cli /c/temp/P1366403.RW2 out.tif -d imageio 2>&1 | grep -i panasonic
  ```
- Run with OpenCL on and off. The Panasonic branch of `_init_coeffs_md_v2`
  runs CPU-side (it fills coefficient tables consumed later by both paths),
  so behavior should be identical, but the AGENTS.md rule that pipelines
  must match dE < 2 CPU vs OpenCL still applies.

## Sample file used

`/c/temp/P1366403.RW2` on the current developer machine (verified during
this investigation):

- Panasonic G9, LUMIX G 42.5mm f/1.7, 5184x3888, DistortionCorrection = On,
- DistortionInfo (0x0119) present and parseable,
- 0x011b present, 64 bytes, all four checksums valid,
- `0x011a = 2` (Homeister's "new-style, use 0x011b" selector),
- word[14] = 256 (Homeister's "on" flag; non-zero),
- radii N1..N4 = 3276, 2730, 2184, 1092 -> ratios 1.0, 0.833, 0.667, 0.333
  (MFT-sensor pattern).

Do not check the file into the repo. Use it locally for the RE work and cite
it in reports without committing binary blobs.

## Open questions

- Whether the toggle labeled "CA Correction" in-camera actually
  suppresses 0x011b's word [14] flag or writes a distinct payload. Klaus
  Homeister reported the GX-8 always writes 0x011b regardless of the
  distortion toggle; we do not have the equivalent observation for the
  G9's CA setting specifically.
- Do 0x011b coefficients vary continuously with focal length on a zoom
  lens, or does the camera snap between a small number of discrete
  profiles? Answer determines whether a native decoder needs to
  interpolate.
- Do the Olympus branch's `ca[6]` fields on OM-D bodies map onto
  Homeister's zone model? Both are Micro Four Thirds; worth a passing
  side-by-side check but not on the critical path.
- Should the fine-tune sliders (`tca_r`, `tca_b`, `tca_override`) work on
  the metadata path the same way they work on the Lensfun path?
- Homeister's claim that 0x011b covers distortion *and* CA (with 0x0119
  as a legacy simpler form) means the current 0x0119-based distortion
  path may be applying correction on top of what 0x011b already implies.
  A SILKYPIX diff with in-camera distortion correction on vs off resolves
  this and should happen before any coefficient RE.
- Would a first-cut *interim* fix (reverting the Panasonic auto-select of
  embedded-metadata mode) be worth landing in this checkout ahead of the
  RE work? It stops the bleeding for users, buys time for the RE. Not
  chosen by the developer during this session ("very glad that the
  embedded metadata is finally worked with"), so this is on hold.

## What is *not* claimed here

- That the coefficient roles within Homeister's parts are decoded. They
  are not. The radii, checksums, on/off flag and `0x011a` selector are
  what is verified; everything else in the payload is still to be worked
  out.
- That Homeister's decode is correct for third-party lenses (Sigma 16mm
  and 30mm on the developer's machine). Verify on the corpus before
  assuming.
- That the sample file's values are representative of other Panasonic
  bodies. G9 firmware version is `0.2.7.0` per makernote; other bodies
  (esp. GH-series and S-series) may write differently.
- That the workaround (switch to Lensfun) restores *exactly* what the
  user had before `47c223703e`. It restores Lensfun-based TCA correction,
  which is what was actually happening before. If the user reports the
  fringing is subtly different, that is expected; they were always seeing
  Lensfun, not Panasonic, correction.
- That `47c223703e` should be reverted. The distortion metadata handling
  it adds is correct and useful on its own; the regression is in the
  auto-selection policy, not in the distortion math.

## References

- Regression-introducing commit: `47c223703e`, "Add Panasonic RW2 embedded
  lens distortion correction", 2026-07-27.
- Panasonic DistortionInfo (0x0119) layout notes:
  https://github.com/trou/panasonic-rw2/blob/master/notes.txt (cited in
  `47c223703e`).
- 0x011b checksum algorithm and structural offsets:
  https://raw.githubusercontent.com/trou/panasonic-rw2/master/parseca.c
  (Raphael Rigo, 2011). Verified against `P1366403.RW2` in this
  investigation; all four checksums match.
- 0x011b radial-zone model:
  https://web.archive.org/web/2024/https://exiftool.org/forum/index.php?topic=9366.0
  (Klaus Homeister, ExifTool forum thread 9366, 2018). The live ExifTool
  forum was intermittently down during this investigation; Wayback has
  the full thread.
- ExifTool `Image::ExifTool::PanasonicRaw.pm`, `Main` tag table, comment
  at tag 0x11b: `# 0x11b - chromatic aberration correction (ref 3) (also
  see forum9366)`.
- Rigo's project page (may carry more historical context):
  http://syscall.eu/#pana (last updated pre-2016; freenode `#panarw2`
  channel referenced there is defunct).
- Repo files touched by the design:
  - `src/common/exif.cc`: `_check_lens_correction_data()`,
    `dt_exif_read_blob()`.
  - `src/common/image.h`: `dt_image_correction_data_t::panasonic`.
  - `src/iop/lens.cc`: `_init_coeffs_md_v2()` (Panasonic branch),
    `reload_defaults()`, `_get_method()`, `commit_params()`,
    `_have_embedded_metadata()`.
- Relevant developer docs to read first: `dev-doc/IOP_Module_API.md`,
  `dev-doc/pixelpipe_architecture.md`. Not modified by this work but they
  document the invariants (input!=output buffers, `DT_OMP_FOR`, etc.) that
  the Panasonic branch has to keep respecting.
