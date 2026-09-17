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
  forum 9366), verified against our G9 sample and confirmed
  body-invariant against a mirror corpus on a Panasonic GX80. The
  coefficient semantics within the layout are the remaining decode gap.
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

### Cross-body verification (GX80)

The G9 corpus was mirrored on a Panasonic DMC-GX80 body (16 MP, 2016)
across the same nine lens/focal combinations at f/5.6, files
`/c/temp/tca/P126063{3..9}.RW2` and `P1260640.RW2`, `P1260641.RW2`.
See `/tmp/rw2_tca/step5_g9_vs_gx80.py` and `step5_cross_body_word8.py`
for reproducibility.

All nine GX80 files pass all four checksums. Structural layout is
identical to the G9: `0x011a = 2`, `word[14] = 256` flag, 4-zone model
with the same 0.333 / 0.667 / 0.833 / 1.0 knot ratios, same
`word[2]` / `word[7]` / `word[13]` opaque positions, same smooth vs
discrete word partition (spot-checked on `step2_classify.py` outputs).
Same lens-generation pattern too: the L 45-150 shots on the GX80 also
carry an empty 0x0119 payload with `flag = 0`, the PL 12-60 shots carry
the same high-order bit progression in the 0x0119 flag byte, and the
primes carry `flag = 1`.

The 4-zone radii scale with body sensor size. GX80's `N1 = 2888`
against G9's `N1 = 3276`, ratio 0.882, matching the ratio of the two
bodies' half-diagonals (2871.2 / 3240.2 = 0.886, within 0.5%). The
radii are in some sensor-pixel unit that shrinks proportionally to the
body's sensor; the *ratios* are body-invariant.

Word-by-word cross-body comparison for the same lens at the same focal
length:

- **Identical:** only `word[14]` (the flag, always 256).
- **Body-scaled by 0.882:** the four radii `word[4, 11, 16, 17]`.
- **Body-varying, not linearly scaled:** everything else. Some
  coefficient words (`word[8]`, `word[27]`, some of the smooth-monotone
  set) scale with a body factor in the 0.72 to 1.17 range, close to but
  not equal to 0.882. Others (`word[13]`, `word[18]`, `word[22]`,
  `word[28]`) shift by tens of thousands of units and sign-flip between
  bodies. These are the words we already had reason to distrust: they
  are in the discrete/bimodal partition where step 2 already classified
  them as non-coefficient.

Word[8]'s anti-correlation with the sign of the measured R-G radial
offset holds cleanly on the GX80: 9 of 9 files, no exceptions. The two
files that were ambiguous on the G9 (small measured signal on L 45-150
@ 97mm and Lumix 42.5) resolve unambiguously on the GX80, presumably
because the GX80's coarser pixel pitch turns a sub-pixel signal into
a slightly-larger sub-pixel signal that's easier to measure. Combined:
16 of 18 file/body pairs show clean anti-correlation, 2 are too small
to call, zero mismatches.

The GX80 data therefore confirms that Homeister's structural model is
body-invariant, that the radii are sensor-scaled and everything else
is per-body-per-lens-per-focal, and that word[8]'s sign relation to
the R correction is genuinely a property of the encoding, not an
artifact of the G9 body.

### Distortion measurement attempt (session 3)

To break out of the sub-pixel CA fit that failed in session 2, we
tried measuring geometric distortion directly and fitting 0x011b to
that. Distortion is a 30-150 px signal vs sub-pixel CA, and the L
45-150 subset has empty 0x0119, so on that lens anything applied to
the in-camera JPEG must come from 0x011b (Homeister's hypothesis).

Scripts under `/tmp/rw2_tca/step6_*.py`. Method: rawpy uncorrected
render, SIFT feature-matching against the paired camera JPEG,
affine-only registration, then per-feature residual radial
displacement binned by `r / N1`.

Two hard findings emerged and one soft one.

- **The L 45-150 JPEG has no distortion applied.** After affine
  registration the residual dr is 0.15 px RMS at all radii, no
  systematic shape (verified on all 6 L 45-150 files across G9 and
  GX80 in `step6_02_prove_jpeg_distortion.py`). Homeister's hypothesis
  that 0x011b replaces 0x0119 as an active correction on this lens is
  wrong: **the camera simply does not apply either tag's payload to
  the JPEG when the lens has an empty 0x0119**. The L 45-150 has
  visible barrel and CA in the raw AND in the JPEG. So on that lens,
  Panasonic's in-camera pipeline just does not correct.
- **0x0119 predicts observed distortion cleanly where it is
  populated.** On the 12-60 sweep and the primes,
  `step6_06_0119_predicts.py` computes the classical Rigo polynomial
  `Ru = Rd + s * (a*Rd^3 + b*Rd^5 + c*Rd^7)` and finds R^2 = 0.75 to
  0.96 against the measured raw-to-JPEG shift. The shape matches. The
  scale is off by a per-file factor: **best-fit k = 0.90 on 12-60 @
  12mm, k = 5.24 at 25mm, k = -0.95 at 60mm on the same lens's zoom**.
  On the primes k is closer to 1 (0.79 on Sigma 30, ~1 on Sigma 16
  and Lumix 42.5). Sign-flipping within the same lens's zoom is not
  compatible with the formula as we have it. Something is missing.
- **The suspected missing term is the high byte of `word[7]`.** On
  0x0119, the flag word is `word[7]`. Rigo defines the low nibble as
  the on/off bit and calls the rest padding. On the 12-60 files, the
  high byte progresses `0xF0` (12mm), `0xB7` (25mm), `0x8D` (60mm).
  Low nibble stays at `1` (on) but the top bits change with focal
  length. On all primes the flag word is exactly `0x0001` and the
  best-fit k lands close to +1. Correlation is tight enough on this
  small sample to hypothesize that the top byte of word[7] carries a
  per-focal-length strength scaler that both Rigo and darktable's
  `47c223703e` currently discard. This wants verification on more
  zoom lenses (only the PL 12-60 in the corpus exposes the varying
  flag byte), but if confirmed it changes the darktable code path.

The soft finding: fitting 0x011b against the residual after 0x0119's
best-scaled prediction on the 12-60 and prime files goes nowhere until
the strength-scaler above is resolved. Any 0x011b fit against those
residuals is only measuring our ignorance of 0x0119. On the L 45-150
subset the "residual" is 0.15 px of registration noise, so all three
model families in `step6_08_fit.py` (piecewise-linear zone heights,
Rigo cubic-quintic-septic, zone heights x word[7] gain) land at R^2
around 0.53, which is fitting noise. The top splits reproduce the
prior finding that words 8 and 27 carry the strongest sign signal but
do not constitute a decode.

**Practical implications for the darktable regression fix.** (Item
(b) below was refuted by session 4; see "word[7] high byte decode"
for the details. The other items still stand.)

1. The regression `47c223703e` introduces two problems, not one:
   (a) auto-selecting embedded-metadata mode drops Lensfun TCA on
   Panasonic files where 0x0119 is populated (established earlier in
   this document); (b) ~~the distortion correction it applies to
   those files may itself be scaled wrong on lenses that use the
   high-byte flag encoding on 0x0119 (this session). Item (b) is
   severe on the Leica DG 12-60 (k values of 5.24 and -0.95 imply
   visible over/under/mis-corrected distortion).~~ Item (b) was
   session 3's reading of the wild `k` values; session 4 showed those
   values are a measurement artifact, not a real per-file scaling
   error in `47c223703e`. Item (a) is the user-reported symptom and
   is unchanged. Item (b) is retracted.
2. ~~`dt_image_correction_data_t::panasonic` should be extended to
   store the raw `word[7]` value from 0x0119 so a follow-up patch can
   apply the missing scaler once its exact functional form is nailed
   down.~~ No missing scaler was found; word[7]'s low nibble
   (on/off) suffices, as `47c223703e` already implements.
3. Reading and decoding 0x011b is on hold pending 0x0119. The tag is
   present, its structure is verified body-invariant, but no in-camera
   pipeline in our corpus applies it, so we cannot observe its
   correction output to reverse-engineer.

### word[7] high byte decode (session 4)

The prior session identified word[7]'s high byte as a suspect
per-focal-length scaler. On PL 12-60 the high byte progresses `0xF0`
(12mm) -> `0xB7` (25mm) -> `0x8D` (60mm) while the primes hold
`0x00`. The per-file best-fit scalars from `step6_06_0119_predicts.py`
(single-parameter `k` such that `k * Rigo_poly(r) ~ measured_dr`) came
out `+0.90 / +5.24 / -0.95` on the zoom and `+0.7-0.9` on the primes.
The size of the swing and the sign flip made a hidden scaler
plausible.

**H1-H5 all fail.** No decoding of word[7]'s high byte, or of any
other 0x0119 word not already used by Rigo's formula, predicts the
observed `k` across the corpus. The wildly varying `k` at 25mm and
60mm on the PL 12-60 is instead a measurement artifact: on those two
files the Rigo polynomial predicts a small-amplitude nonlinear signal
(peak 2 px and 15 px respectively) that is dominated by a small
monotonic-in-`r` residual which the r^3/r^5/r^7 polynomial cannot
match. The scripts and logs are under `/tmp/rw2_tca/step7_*.py` and
`/tmp/rw2_tca/step7_*.log`.

**Freshly measured `k_only` values** (rerun of `step6_06`, translation-
only registration, bin-median least-squares):

| file                     | k_only  | R^2   | word[7]     | poly peak | peak sign |
|:-------------------------|--------:|------:|:------------|----------:|:----------|
| G9 PL12-60 @12mm         |  +0.901 | 0.931 | 0xF001 (-4095)  |  107 px | +         |
| G9 PL12-60 @25mm         |  +5.240 | -0.107| 0xB701 (-18687) |    2 px | +         |
| G9 PL12-60 @60mm         |  -0.948 | 0.070 | 0x8D01 (-29439) |   15 px | -         |
| G9 Sigma 30              |  +0.795 | 0.853 | 0x0001 (+1)     |   20 px | +         |
| G9 Sigma 16              |  +0.733 | 0.850 | 0x0001 (+1)     |   79 px | +         |
| G9 Lumix 42.5            |  +0.882 | 0.964 | 0x0001 (+1)     |   37 px | +         |
| GX80 PL12-60 @12mm       |  +0.849 | 0.982 | 0xF001 (-4095)  |  163 px | +         |
| GX80 PL12-60 @25mm       |  +5.171 | 0.741 | 0xB701 (-18687) |    2 px | +         |
| GX80 PL12-60 @60mm       |  -1.041 | 0.911 | 0x8C01 (-29695) |   13 px | -         |
| GX80 Sigma 16            |  +0.709 | 0.772 | 0x0001 (+1)     |   50 px | +         |
| GX80 Sigma 30            |  +0.966 | 0.998 | 0x0001 (+1)     |   33 px | +         |
| GX80 Lumix 42.5          |  +0.966 | 0.990 | 0x0001 (+1)     |   31 px | +         |

Sign convention: positive `k_only` means the polynomial's own sign
(driven by `word[8] = a`) matches the measured shift direction.
Negative `k_only` means measured direction is opposite. The 12-60 @
60mm case has `a = -244` (polynomial predicts inward) but the JPEG
shows outward shift, giving `k = -1`. On the other five 0x0119-
populated files the polynomial's `a` sign matches the measured sign.

**H1 - linear/inverse-linear/exponential in the high byte.** Best
single-feature linear R^2 against `k` is 0.15 (`word[14]` signed,
which is a checksum). `w[7]_hi` in every encoding tested (signed
int8, unsigned int8, top nibble signed or unsigned, bits 11..8,
whole int16, `|w[7]|`, `w[7]/32768`) delivers R^2 in the range
[-0.05, +0.15]. No functional form better than "essentially random"
survives. `step7_hypotheses.py` and `step7_multi_word_search.py`.

**H2 - subfields of the top byte.** Top nibble, bits 11..8 and lo
nibble each score R^2 < 0.10. `w[7]_hi` sorted by value gives
non-monotonic `k`: `(0x00,+0.87), (0x8D,-0.95), (0xB7,+5.24),
(0xF0,+0.90)`. No monotonic function fits.

**H3 - `k = 1 + w[7]/M`.** For any `M`, the required per-file `M`
disagrees by more than 10x across the four PL 12-60 files. The best
whole-corpus R^2 over `M` in {8192, 16384, 32768, 65536} is +0.075.

**H4 - LUT index.** With only three distinct nonzero `w[7]_hi`
values (from one lens's zoom sweep) and one zero value (from three
primes), the corpus does not constrain a LUT. Any 4-entry table
fits perfectly; nothing generalizes.

**H5 - a different 0x0119 word carries the scaler.** All six words
Rigo does not use (word[2, 3, 6, 9, 10, 13]) plus every two-way
combination of them and `w[7]` were scanned as linear predictors of
`k`. Best individual R^2 is 0.28 (`word[9]` and `word[13]` tie, both
signed). Best two-word linear fit is R^2 = 0.51. Nothing that ought
to be a real decode. `step7_multi_word_search.py` output logged to
`/tmp/rw2_tca/step7_multi.log`.

**What is really going on.** Look at the shape of the measured `dr`
on the two anomalous files. On PL 12-60 @ 25mm (both bodies) and PL
12-60 @ 60mm (both bodies), the measurement is roughly linear in
`r_norm` from 0 to about 10-13 px at the corner. That is exactly
what an unmodelled residual radial scale in the raw-to-JPEG
registration would look like. The r^3/r^5/r^7 polynomial cannot
match a linear shape, so the least-squares fit dresses up whatever
`k * poly` best approximates the linear signal in the observed
r-range. At 25mm the polynomial peaks at r=0.65 and drops, so the
fitter uses a large positive `k` to match the peak; at 60mm the
polynomial has the opposite sign, so the fitter negates.

Joint fit `dr = k * poly(r) + alpha * r_pixel` (`step7_joint_crossbody.py`,
log at `/tmp/rw2_tca/step7_joint.log`) confirms this:

- On the six files where the polynomial predicts a large-amplitude
  nonlinear signal (12mm PL, all three primes, per body: 8 files),
  `k_joint` collapses toward 1 (range 0.74-1.28, median 1.02) and
  `alpha` is small.
- On the four anomalous files, `k_joint` scatters across
  [-1.4, +2.1] while `alpha` explains most of the signal.

`alpha` sits at 0.001-0.005 (0.1-0.5% radial dilation) on the four
anomalous files. That is small enough to be invisible on files with
a strong polynomial signal, and dominant on files where the tag's
correction is close to zero. The GX80 12-60 @ 60mm's Rigo-poly R^2
of 0.911 is the strongest counter-argument, but the same file's
"pure `alpha * r`" fit lands at R^2 = 0.725 (see
`step7_residual.log`), i.e. any monotone-increasing model would fit
its data reasonably. `k = -1.04` is just what the least-squares
extracts when you force a wrong-shape polynomial onto a mostly-linear
signal.

Where the ~0.5% radial dilation comes from is a separate question. It
is not a per-body constant: `alpha` varies from -0.012 to +0.005
across the corpus, with the negative values on the strongest-signal
files where the joint fit is co-linear with the polynomial's own
low-order Taylor expansion anyway. It could be a small crop-factor
difference between rawpy's output and Panasonic's JPEG geometry, a
residual translation not absorbed by the median centroid, or an
actual small linear-in-r correction the tag encodes elsewhere that
we did not decode. Either way it is not word[7]'s high byte.

The `k_only` values on the four *reliable* zoom-and-prime files
cluster in [0.71, 0.97] with median 0.87 (`step7_joint.log`, files
with `R^2_only > 0.7`, N=8 across bodies). That 13% offset from unity
is at the noise floor: rawpy's AAHD demosaic, the SIFT peak-fit
precision, and small crop-factor mismatches between the raw and JPEG
image spaces can each account for a few percent. Rigo's formula
therefore predicts the observed distortion within measurement noise
on every file where the tag's own polynomial dominates the signal.

**What this means for the darktable code.** Session 3's
"Practical implications" bullet (b) - that the distortion correction
`47c223703e` applies may be silently mis-scaled on the Leica DG 12-60
- **does not survive contact with the corrected measurement**. There
is no missing per-file scaler in `47c223703e`'s Panasonic branch. The
`k = 5.24` and `k = -0.95` observations that motivated the search
were an artifact of fitting an r^3/r^5/r^7 polynomial against a
mostly-linear-in-r measurement residual on files where the tag's own
correction is small. `_init_coeffs_md_v2`'s Panasonic branch as it
stands is scaling the polynomial the same way the tag intends. The
regression covered elsewhere in this document (default-method switch
dropping Lensfun TCA) is still real; the distortion math is not.

**What could still change this conclusion.** A raw-vs-JPEG comparison
that reliably strips residual affine scale from the measurement on
low-signal files would either confirm the current finding
(polynomial fits, no per-file scaler) or reveal a genuinely missing
correction term. Candidates the current session did not run:

- render the RW2 with dcraw or Panasonic-SDK-based decoders to check
  the raw geometry is identical to rawpy's output;
- SIFT with a homography model (not just affine) to see whether the
  linear-in-r residual is coming from perspective, keystone or a
  crop-then-resize step in the JPEG pipeline;
- shoot a chart with fixed grid intersections at every focal length,
  so the geometry is anchored by known-collinear points instead of
  scene-dependent SIFT features.

None of these is essential for the darktable code path. The
`47c223703e` distortion math is defensible as-is; where it silently
fails is only on files where the tag's own polynomial predicts near-
zero correction, and there the failure mode is "we apply nothing
useful" rather than "we apply a mis-scaled correction".

### DNG WarpRectilinear ground truth (session 5)

The prior sessions were fitting 0x011b against pixel measurements. This
session drops that. Adobe DNG Converter renders every RW2 in the corpus
into a DNG that carries a `WarpRectilinear` opcode -- Adobe's own
per-plane radial correction, meant to reproduce whatever Panasonic's
in-camera algorithm applies. The opcode floats are the CA target the
smooth words in 0x011b have to encode.

**What the opcode holds.** `OpcodeList3` on every DNG in
`/c/temp/tca/dng/*.dng` is a single `WarpRectilinear` (opcode id 1)
with three planes (R, G, B) and per plane six doubles
`(k_r0, k_r1, k_r2, k_r3, k_t0, k_t1)` plus an image-shared center
`(cx, cy)`. The DNG spec applies each plane as

```
R_source = R_dest * (k_r0 + k_r1 * R^2 + k_r2 * R^4 + k_r3 * R^6)
```

with `R` the destination distance from `(cx, cy)`, normalized so that
the corner is `R = 1`. `k_t0` and `k_t1` are the tangential terms
and are exactly zero on all 18 files. Centers sit at `(0.5000, 0.5000)`
on G9 files and `(0.4987, 0.5000)` on GX80 files -- Adobe crops one raw
column on the GX80. All numbers below come from
`/tmp/rw2_tca/step8_dng_opcodes.py` and the dataset dump at
`/tmp/rw2_tca/decode_dataset.npz`
(reader: `/tmp/rw2_tca/step9_build_dataset.py`).

Define per file

```
D_R[i] = plane_R.k_r[i] - plane_G.k_r[i]     i in 0..3
D_B[i] = plane_B.k_r[i] - plane_G.k_r[i]
```

so that the G plane carries whatever distortion Panasonic wants applied
in common to all three channels, and `D_R`, `D_B` carry the
pure-per-channel offset. `|D_R|` and `|D_B|` sit in the 1e-5 to 1e-3
range across the corpus.

**Sanity check: G plane vs Rigo 0x0119.** DNG's polynomial takes the
undistorted radius as input and returns the distorted radius, which is
the multiplicative inverse of Panasonic's `Ru = Rd * (1 + s*(a*Rd^2 +
b*Rd^4 + c*Rd^6))`. `/tmp/rw2_tca/step10_fit.py` inverts the Rigo
polynomial numerically on `[0, 1.05]`, fits a four-term polynomial in
`R^2`, and compares against the DNG G plane per file. On the 12
files where 0x0119 is enabled, the per-coefficient agreement is close
(k_r1, k_r2 and k_r3 typically within a few percent) but the DNG G
plane adds a small per-file scaling: `k_r0` sits at 0.9987 on PL 12-60
@ 12mm, 0.9956 at 25mm, 0.9929 at 60mm, and 1.0000 on the primes and
Lumix 42.5. Adobe renormalizes the whole polynomial by that factor,
presumably to keep the destination inside the frame. On the six L
45-150 files with `flag = 0` on 0x0119 the DNG G plane is identity
`(1, 0, 0, 0)`, matching session 3's finding that the camera applies
no distortion correction to those.

So the DNG-G-to-Panasonic-0x0119 mapping is what we expected up to a
per-file Adobe framing constant. The check does not decode anything new;
it confirms the RGB planes carry Panasonic's own coefficients rather
than Adobe's rederivation.

**Decode result.** Two regressions, one per differential:

```
D_R[i] = sum_j C_R[i, j] * word[smooth_j]        i in 0..3
D_B[i] = sum_j C_B[i, j] * word[smooth_j]
```

with `smooth_j in {2, 8, 10, 12, 20, 23, 26, 27, 29}` from prior
sessions. 18 files provide the observations, so with 9 predictors the
in-sample fit is nearly perfect and unhelpful; the honest metric is
cross-validation. Two schemes:

- **LOO** (`step10_fit.py`): drop one file, fit on 17, predict the
  dropped one. LOO leaks the same-lens/same-focal sibling from the
  other body into training, so it reports 0.99 R^2 on almost every k
  and is not the metric we care about.
- **LOGO** (`step11_cv.py`, `step14_final.py`): drop the pair
  `(G9_file, GX80_file)` that share `(lens, focal)`, fit on the other
  16, predict both. This is the fair test for whether the fit
  generalizes across bodies at unseen lens configurations.

Under LOGO with all nine smooth words, the fit works cleanly on `k_r0`
and `k_r1` (R^2_LOGO 0.90-0.99) but collapses on `k_r2` and `k_r3`
(0.73 down to -4.5). Dropping the three words with the largest
between-file dynamic range -- `w[2]` (a per-lens family baseline
running 0-30000), `w[26]` and `w[29]` -- and refitting with the
remaining six recovers the higher orders on the R channel:

*Reduced predictor set:* `w[8]`, `w[10]`, `w[12]`, `w[20]`, `w[23]`,
`w[27]`. Six words, four coefficients, so `C_R` and `C_B` are 4 x 6
matrices.

*D_R LOGO R^2* (`step14_final.py`, `step16_final_report.py`):

| coefficient | R^2_train | R^2_LOGO | RMS_train | max abs value |
|:------------|----------:|---------:|----------:|--------------:|
| k_r0        |     0.996 |    0.994 |  2.17e-05 |      6.10e-04 |
| k_r1        |     0.998 |    0.990 |  1.51e-05 |      6.21e-04 |
| k_r2        |     0.988 |    0.973 |  2.05e-05 |      4.95e-04 |
| k_r3        |     0.989 |    0.975 |  1.41e-05 |      3.69e-04 |

All four coefficients generalize. Worst per-file LOGO residual is
1.3e-04 on G9 PL 12-60 @ 12mm (the strongest-CA file in the corpus,
`|D_R|` = 1.0e-3, 12% relative). Median per-file LOGO residual is
5.4e-05, about 15% of the mean `|D_R|`.

*D_B LOGO R^2:*

| coefficient | R^2_train | R^2_LOGO | RMS_train | max abs value |
|:------------|----------:|---------:|----------:|--------------:|
| k_r0        |     0.995 |    0.993 |  1.14e-05 |      3.94e-04 |
| k_r1        |     0.996 |    0.988 |  1.77e-05 |      4.10e-04 |
| k_r2        |     0.858 |    0.331 |  5.71e-05 |      2.95e-04 |
| k_r3        |     0.810 |    0.080 |  3.66e-05 |      1.89e-04 |

R and B agree on the low orders but B's `k_r2` and `k_r3` do not
generalize. Cause: the actual `D_B` values differ substantially between
bodies at the same lens and focal. For example on Lumix 42.5, G9
`D_B.k_r2 = -2.3e-05`, GX80 `D_B.k_r2 = -8.6e-05`; on L 45-150 @ 97mm,
G9 = +1.3e-04, GX80 = +3.8e-05. That is a 3-4x cross-body swing, and it
persists no matter which words or which scaling we use. The dominant
low-order B correction is decoded; the higher-order shape is not, and a
larger scan across bodies and word subsets (`step13_sweep.py`,
`step14_final.py::d_b_subset_search`) does not close the gap. Best
single-target LOGO R^2 for `D_B.k_r2` across all subsets tested is
0.79; for `D_B.k_r3` it is 0.80, but no subset delivers both above 0.7
simultaneously.

*Evaluated at the pipeline output.* `step12_curve_eval.py` computes
`D_R_curve(R) = k_r0 + k_r1*R^2 + k_r2*R^4 + k_r3*R^6` and its
equivalent for B, at R = 0.25, 0.50, 0.75, 1.00. Under LOGO on the
reduced set:

| R    | D_R R^2_LOGO | D_B R^2_LOGO |
|:-----|-------------:|-------------:|
| 0.25 |        0.896 |        0.962 |
| 0.50 |        0.893 |        0.967 |
| 0.75 |        0.879 |        0.976 |
| 1.00 |        0.484 |        0.881 |

So the polynomial *value* -- the quantity the pipeline actually uses to
resample -- is predicted well in the interior for both channels, with
`D_R` degrading at R = 1.0 because the PL 12-60 @ 12mm outlier has
strong opposing k_r2 and k_r3 that partially cancel in the interior and
compound at the corner. The corner error in pixels is 0.03-0.75 px on
15 of 18 files and 1.4 px on the PL 12-60 @ 12mm pair.

**Physical sanity.**

- *Near-zero on small-CA lenses.* The Lumix 42.5 has session-3 word[8]
  in the low hundreds and the smallest measured CA in the corpus.
  Predicted `|D_R|` under LOGO: 1.3e-04 on both bodies. Measured
  `|D_R|`: 1.3e-04 (G9), 1.7e-04 (GX80). Match.
- *Cross-body magnitude.* Same lens and focal length, both bodies:
  measured `|D_R|` differs by 5-30%; predicted `|D_R|` also differs by
  5-30% and in the same direction. The fit does not artificially
  equalise the two bodies.
- *Sign flip along a zoom.* PL 12-60 measured `D_R.k_r1` on the G9 goes
  `-6.2e-04` at 12mm, `-2.4e-04` at 25mm, `+1.6e-04` at 60mm; the LOGO
  prediction goes `-4.7e-04`, `-2.4e-04`, `+2.0e-04`. Sign and rough
  magnitude are preserved. GX80 shows the same pattern.

**Rejected functional forms.** Under LOGO with all nine words:

- F1 constant scaling: `k_r0`, `k_r1` fit; `k_r2`, `k_r3` collapse on
  the R channel and stay negative on the B channel.
- F2 `y * N1^(2i+1) = W @ c`: R^2_LOGO 1.000 on `k_r0` (a numerical
  coincidence: N1 is nearly bimodal) but -5.4 on `k_r3`. Rejected.
- F3 `y / (w7 / 32768) = W @ c`: R^2_LOGO negative on 7 of 8 targets.
  Rejected.
- F3b `y * (w7 / 32768) = W @ c`: same collapse.
- F4 sparse one-word: only `k_r0` decodes single-handedly, and by a
  single word (`w[12]` for R, `w[27]` for B); the other coefficients
  need a linear combination.
- F4b sparse two-word: `D_R.k_r1` at LOGO 0.916 with `w[12]` and
  `w[23]`, but `k_r2` and `k_r3` do not survive.
- F5 body-normalized `(D * (N1/3276)^p) = W @ c`, p in [-2, +2]: no
  exponent recovers `D_B.k_r{2,3}`.
- Reduced 6-word set (dropping `w[2]`, `w[26]`, `w[29]`): the one
  that works, above.

**Coefficient matrices (train on all 18 files, reduced 6-word set).**
Stored at `/tmp/rw2_tca/final_fit.npz`; here for reference. `word[k]`
denotes the signed int16 read from the 0x011b payload at 32-bit-word
index `k`.

*C_R:*

```
        w[8]         w[10]        w[12]        w[20]        w[23]        w[27]
k_r0:  -5.5919e-08  -2.7534e-07  -1.0043e-06  +9.4388e-08  +8.1750e-08  +3.1028e-07
k_r1:  +1.7918e-06  +3.4704e-07  +5.4376e-06  -1.0369e-07  -4.9216e-06  -4.6536e-07
k_r2:  -4.0368e-06  +2.2315e-06  -7.8190e-06  -1.0252e-06  +8.9802e-06  -1.7742e-06
k_r3:  +1.5442e-06  -3.2808e-06  +3.1874e-06  +1.5409e-06  -3.5842e-06  +2.5324e-06
```

*C_B (k_r0 and k_r1 only; k_r2 and k_r3 do not generalize):*

```
        w[8]         w[10]        w[12]        w[20]        w[23]        w[27]
k_r0:  +1.1514e-07  +3.7170e-07  +9.8105e-09  -1.2143e-07  -1.4375e-07  -1.4212e-06
k_r1:  -3.3139e-07  -4.9106e-06  -9.7770e-08  +1.6006e-06  +4.4665e-07  +5.8716e-06
```

**Formula for a follow-on darktable patch.**

Note: session 8 (below, JPEG-referenced decode) found that the raw
Adobe-DNG-derived matrices here over-correct by roughly 11x versus
the CA the camera itself applies to its JPEG. The shippable form
divides the output by K = 11.48; see the "Formula for the darktable
patch (JPEG-referenced, session 8)" block for the final version.
The matrices themselves stay as they are here, kept as historical
reference for the Adobe-DNG target.

```
static const int words_R[6] = {8, 10, 12, 20, 23, 27};
static const double C_R[4][6] = {
  { -5.5919e-08, -2.7534e-07, -1.0043e-06, +9.4388e-08, +8.1750e-08, +3.1028e-07 },
  { +1.7918e-06, +3.4704e-07, +5.4376e-06, -1.0369e-07, -4.9216e-06, -4.6536e-07 },
  { -4.0368e-06, +2.2315e-06, -7.8190e-06, -1.0252e-06, +8.9802e-06, -1.7742e-06 },
  { +1.5442e-06, -3.2808e-06, +3.1874e-06, +1.5409e-06, -3.5842e-06, +2.5324e-06 },
};
/* for i in 0..3: plane_R.k_r[i] = plane_G.k_r[i]
                                 + sum_j C_R[i][j] * word[words_R[j]]
   (Adobe-DNG target; divide the RHS sum by K = 11.48 for the
   in-camera JPEG target; see session 8.) */
```

For the B channel only the low orders are safe:

```
static const double C_B_lo[2][6] = {
  { +1.1514e-07, +3.7170e-07, +9.8105e-09, -1.2143e-07, -1.4375e-07, -1.4212e-06 },
  { -3.3139e-07, -4.9106e-06, -9.7770e-08, +1.6006e-06, +4.4665e-07, +5.8716e-06 },
};
/* plane_B.k_r[0] = plane_G.k_r[0] + sum_j C_B_lo[0][j] * word[words_R[j]]
   plane_B.k_r[1] = plane_G.k_r[1] + sum_j C_B_lo[1][j] * word[words_R[j]]
   plane_B.k_r[2] and plane_B.k_r[3]: no reliable decode; leave equal to
   plane_G.k_r[2] and plane_G.k_r[3] respectively, or fall back to the
   Lensfun path on files where CA at the corner matters. */
```

**Bounds on the claim.**

- The decode uses 18 files, six words, and validates by leaving out
  both bodies of the same lens/focal at once. R^2_LOGO in the 0.97-0.99
  range on all four R coefficients is real signal, not overfit: the
  same 6 x 4 coefficient matrix predicts a held-out pair back to within
  a couple of pixels of Adobe's number at the corner and to sub-pixel
  precision in the interior on 15 of the 18 files.
- The decode was found by dropping predictors and looking at LOGO
  scores. Feature-selecting on the same corpus used for validation is
  a form of leakage. The next corpus expansion (a different pair of
  Panasonic bodies, or a lens outside the 12mm-150mm range this corpus
  covers) is the honest test.
- `D_B.k_r{2, 3}` genuinely do not decode from these words. Best
  single-target LOGO R^2 across every subset tested is 0.79; no
  scaling recovers it. Either Adobe's B-plane fit picks up
  body-specific residuals that are not encoded in 0x011b, or the words
  we called "smooth" in prior sessions miss a B-channel-specific
  higher-order term. Not resolved here.
- The reduced 6-word set was found empirically. `w[2]`, `w[26]` and
  `w[29]` are numerically large but do not appear to be CA
  coefficients on the R channel; they may be lens IDs, per-focal
  scalers or internal state. Prior sessions had `w[2]` flagged as a
  suspected lens-family key; this session's fit agrees.

**Where this points the follow-on darktable patch.**

1. Extend `dt_image_correction_data_t::panasonic` (or add a
   `dt_image_correction_data_t::panasonic_ca` sibling) with a 32-word
   signed-int16 copy of 0x011b's payload plus a validated flag from the
   four checksums.
2. In `_check_lens_correction_data()` in `src/common/exif.cc`,
   parse 0x011b, validate Rigo's four checksums, store the words and
   the `word[14] != 0` on/off flag.
3. In `_init_coeffs_md_v2()`'s Panasonic branch in `src/iop/lens.cc`,
   after the existing distortion evaluation, evaluate the R and B
   deltas from the six words listed above with the matrices given and
   write them into `cor_rgb[0][i]` and `cor_rgb[2][i]` respectively.
   Leave `cor_rgb[1][i]` alone; that is the G plane already computed
   from 0x0119.
4. The B channel's `k_r2` and `k_r3` do not have a validated decode.
   Two options in the patch: (a) apply only the low-order B correction
   and leave the higher-order B coefficients at the G plane's values
   (safer, but leaves some corner B fringing on strong-CA files); or
   (b) reuse the R channel's matrix negated with a sign convention that
   matches the low-order B fit (worse than "leave off" on some files,
   better on others; not defensible from this corpus).
5. A larger corpus with a third body (G9 II, GH-series, or one of the
   S bodies) would let us either close the B-channel gap or confirm it
   as body-dependent. Do this before shipping option (b).

Scripts and data used this session:

- `/tmp/rw2_tca/step8_dng_opcodes.py`: DNG opcode reader.
- `/tmp/rw2_tca/step9_build_dataset.py`: builds the working npz.
- `/tmp/rw2_tca/decode_dataset.npz`: 18-file dataset.
- `/tmp/rw2_tca/step10_fit.py`: F1-F5 sweep with LOO.
- `/tmp/rw2_tca/step11_cv.py`: F1 under LOGO.
- `/tmp/rw2_tca/step12_curve_eval.py`: polynomial-value R^2 at fixed R.
- `/tmp/rw2_tca/step13_sweep.py`: full LOGO sweep, subset search.
- `/tmp/rw2_tca/step14_final.py`: reduced-set physics check, D_B
  subset and scaling search.
- `/tmp/rw2_tca/step15_body.py`: body-indicator and per-body fits.
- `/tmp/rw2_tca/step16_final_report.py`: final coefficient dump.
- `/tmp/rw2_tca/final_fit.npz`: `C_R` and `C_B` matrices.

### Third-body validation (session 6)

Session 5 warned that feature-selecting on the same corpus used for
validation was a form of leakage, and that "the next corpus expansion
(a different pair of Panasonic bodies, or a lens outside the 12-150mm
range this corpus covers) is the honest test." This session runs that
test. It uses RW2 samples from four bodies outside the G9+GX80 corpus,
sourced from raw.pixls.us: **DC-S5** (full frame, L-mount), **DC-G9M2**
(2023 G9 successor), **DC-GH5**, and **DMC-GX8**. 16 files across four
lenses that were not all in the training corpus. All data lives under
`/tmp/rw2_tca/third_body/`; scripts are `step17_*.py`. Nothing under
`/c/temp/` was touched.

**Corpus.** From `exiftool -Model -LensID -FocalLength -FNumber`:

| body    | file                | lens                                        | focal   | f/    |
|:--------|:--------------------|:--------------------------------------------|:--------|:------|
| DC-S5   | P1047510.RW2        | LUMIX S 85mm F1.8                           | 85 mm   | 6.3   |
| DC-S5   | P1047513.RW2        | LUMIX S 85mm F1.8                           | 85 mm   | 8.0   |
| DC-S5   | dc-s5_6k4k.RW2      | LUMIX S 20-60mm F3.5-5.6                    | 60 mm   | 8.0   |
| DC-G9M2 | P1000019..034 (5)   | Lumix G X Vario 12-35mm F2.8 II             | 26 mm   | 5.6   |
| DC-GH5  | _T012010..014 (4)   | Leica DG Vario-Elmarit 12-60mm F2.8-4       | 25-27mm | 5.6   |
| DMC-GX8 | P1020809..812 (4)   | Lumix G X Vario 35-100mm F2.8               | 75 mm   | 5.6   |

The GH5 Leica 12-60 is the same lens family as the G9 PL 12-60 in the
training corpus, so that group is a direct cross-body test. The three
other bodies use lenses outside the training set.

**Path C -- structural check** (`step17_structural_check.py`,
`step17_structural_check.log`). All 16 files carry a 64-byte 0x011b
payload; Rigo's four checksums pass on every file; word[14] = 256 on
every file. Homeister's radii ratios:

| body    | tag_011a | N2/N1  | N3/N1  | N4/N1  |
|:--------|---------:|-------:|-------:|-------:|
| DC-S5   | 2        | 0.8571 | 0.5714 | 0.2857 |
| DC-G9M2 | *absent* | 0.8334 | 0.6668 | 0.3334 |
| DC-GH5  | 2        | 0.8333 | 0.6667 | 0.3333 |
| DMC-GX8 | 2        | 0.8333 | 0.6667 | 0.3333 |

Three things worth flagging:

- **DC-S5's radii are a distinct pattern.** Full frame uses
  1.0 / 6/7 / 4/7 / 2/7 rather than the MFT 1.0 / 5/6 / 4/6 / 2/6.
  Homeister's four-zone model still holds; the specific radii change
  with sensor format. The `dt_image_correction_data_t::panasonic_ca`
  parse should not hard-code the MFT ratios.
- **DC-G9M2 does not carry 0x011a at all**, yet ships a valid 0x011b
  with all four checksums OK and word[14] = 256. Homeister's rule
  "`0x011a = 2` selects 0x011b" is not universal on newer bodies; on
  the G9 II we get 0x011b without the explicit selector. Any code
  path that gates 0x011b reading on `0x011a == 2` will silently miss
  Panasonic's newest generation. Either treat `0x011a` absent as
  equivalent to `0x011a == 2` on the newer bodies, or gate on the
  0x011b checksums alone.
- Everything else matches: the 32-word signed-int16 layout, the flag
  location, the four-zone structural role of words [4], [11], [16],
  [17] all reproduce on every third-body sample. The structural
  findings from sessions 1-5 generalize cleanly.

**Path A -- raw-pixel CA vs C_R prediction** (`step17_pathA.py`,
`step17_pathA.log`, `/tmp/rw2_tca/third_body/pathA_results.json`). For
each file, `step17_pathA.py` reads the 32 signed-int16 words from
0x011b, computes the predicted `D_R` coefficients using session 5's
C_R matrix, evaluates `D_R(R) = k_r0 + k_r1*R^2 + k_r2*R^4 + k_r3*R^6`
at destination radii from 0.10 to 0.88 in normalized units (corner
= 1), multiplies by `R_dest` in pixels to get a predicted R-vs-G
displacement, and compares against a rawpy AAHD measurement of the
same displacement using `step4_measure_v2.measure_file`. The sign
convention is the same one session 3 established: positive means R is
displaced outward from the optical center relative to G.

Per-body aggregate. "signal bins" are per-radius bins where the
predicted correction exceeds max(0.05 px, per-file half-vs-half RMS
precision floor). The training corpus files P1366477, P1366484 and
P1366486 are included as `_training_ref` to anchor what the same
measurement pipeline says on the corpus that trained C_R.

| body            | files | signal bins | sign match | median |pred/meas| | noise px |
|:----------------|------:|------------:|:-----------|-------------------:|---------:|
| DC-G9M2         |     5 |          24 | 24/24      |                9.1 |    0.021 |
| DC-GH5          |     4 |          16 | 12/16      |               17.1 |    0.010 |
| DC-S5           |     3 |          16 | 11/16      |               16.7 |    0.013 |
| DMC-GX8         |     4 |          22 | 22/22      |               16.5 |    0.027 |
| _training_ref   |     3 |          17 | 15/17      |               10.5 |    0.011 |

Two things fall out.

*Sign generalizes.* On the files where the C_R prediction is above the
measurement noise floor, the sign of the predicted R-G displacement
matches the sign of the measured R-G displacement on 69 of 78 bins
across all four third-body corpora combined -- the same rate the
same pipeline gives on the training corpus (15/17). Sign matches at
r_norm = 0.1..0.9 on both zoom (Lumix G X 12-35 on the G9M2, G X
35-100 on the GX8) and prime (Lumix S 85 f/1.8 on the S5) lenses that
were never in the training set. The nine misses split as four on
DC-GH5 and five on DC-S5, all at radii where the measured signal is
at or below the ~0.01 px precision floor; they are noise, not
disagreement.

*Magnitude does not.* The predicted-to-measured ratio is 9-17x on
every body -- including on the training corpus itself, where the same
comparison is 10.5x. C_R was fit to reproduce Adobe DNG Converter's
`WarpRectilinear` opcodes, which it does to within ~5% on the training
set (session 5). But the DNG opcodes themselves predict a ~10x larger
correction than what a rawpy AAHD demosaic shows in the raw. So this
factor-of-ten gap is a property of Adobe's opcode vs the raw sensor,
not a failure of C_R to generalize. Applying C_R naively in
darktable's Panasonic branch would follow Adobe's DNG behavior: a much
stronger correction than the physical CA visible in the raw, which
will over-correct on files where the physical raw is close to
CA-free. That is a known bound on the C_R decode, not a new one.

*Cross-body coefficient sanity.* Direct comparison of the C_R
prediction for the GH5 Leica 12-60 @ 25mm against the actual DNG D_R
values for the G9 and GX80 PL 12-60 @ 25mm (`step9_build_dataset.py`
dataset) shows the same lens read differently by different bodies:

```
predicted GH5 D_R:  +1.63e-04  -2.33e-04  +9.68e-05  -6.46e-05
actual G9 D_R:      +2.88e-04  -2.38e-04  +7.57e-05  -4.34e-05
actual GX80 D_R:    +2.57e-04  -2.34e-04  +7.49e-05  -4.48e-05
```

k_r1 and k_r2 agree across all three within 30%; k_r0 differs by 40%.
The 0x011b word[8] value for the "same" lens at the same focal is
-397 on G9, -287 on GX80, but -47 on GH5. Either the specific Leica
12-60 sample used for the GH5 shots has a different serial-number
calibration, or the GH5 body writes 0x011b with slightly different
scaling. Either way, the fit still returns a plausible D_R for the
GH5 file -- shape preserved, low-order term shrunk.

**B-plane high-order gap** (`step17_b_plane_gap.py`,
`step17_b_plane_gap2.py`). Session 5 left `D_B.k_r2` and `D_B.k_r3`
undecoded and hypothesized that one of the discrete/bimodal words from
session 2's classification (`[3, 5, 6, 9, 15, 18, 19, 21, 22, 24, 25,
26, 28]`) might close the gap. Retested here by adding candidates to
the base 6-word predictor set and rerunning LOGO on the original
18-file `decode_dataset.npz`:

| target      | base R^2 | best single add     | best pair          | best triple            | best quad                   |
|:------------|---------:|:--------------------|:-------------------|:-----------------------|:----------------------------|
| D_B.k_r2    |    0.331 | w[19] -> 0.473      | w[9]+w[25] 0.564   | w[25]+w[26]+w[28] 0.769 | w[22]+w[25]+w[26]+w[28] 0.774 |
| D_B.k_r3    |    0.080 | w[24] -> 0.274      | w[3]+w[19] 0.720   | w[24]+w[25]+w[28] 0.828 | w[22]+w[24]+w[25]+w[28] 0.854 |

D_B.k_r3 does clear the 0.85 threshold, but only by adding four
discrete words on top of the base six. 6+4 predictors on 18 files is
close to over-parameterisation (16 training files per LOGO fold), and
the same four-word augmentation does not lift D_B.k_r2 past 0.774. No
single discrete word closes either gap; the pair search peaks at 0.72
for k_r3. The honest conclusion is the same as session 5's: the
low-order B decode is real but the higher-order B shape does not
generalize from these six smooth words alone, and picking four extra
discrete words to force a fit through 18 samples is an overfit rather
than a decode. The gap is still open.

**Bounds this adds.**

- The C_R decode reproduces the DNG `WarpRectilinear` R-plane
  differential on both MFT and full-frame Panasonic bodies without
  refit. Structural parsing (checksums, radii ratios, on/off flag)
  survives every third-body sample tested.
- The absolute pixel-space magnitude the C_R decode predicts is
  ~10x the CA visible in the raw. That gap is present on the
  training corpus itself and is a property of the Adobe DNG opcode,
  not of C_R. A darktable patch that follows this decode should
  either scale the coefficients down by a factor that matches the raw
  (measured here as roughly 10x), or accept that it will match Adobe
  DNG Converter's output rather than the sensor's actual CA.
- Homeister's `0x011a == 2` selector rule does not hold on the G9 II;
  the darktable Panasonic branch has to accept 0x011b whenever the
  four checksums pass, regardless of 0x011a.
- The DC-S5 uses 1.0 / 6/7 / 4/7 / 2/7 as its four-zone radii, not the
  MFT 1.0 / 5/6 / 4/6 / 2/6. Any code path that hard-codes the MFT
  ratios will decode DC-S5's 0x011b wrongly.

Scripts and data this session:

- `/tmp/rw2_tca/step17_structural_check.py` and `.log`
- `/tmp/rw2_tca/step17_pathA.py` and `.log`,
  `/tmp/rw2_tca/third_body/pathA_results.json`
- `/tmp/rw2_tca/step17_b_plane_gap.py`, `step17_b_plane_gap2.py` and
  their `.log` files
- `/tmp/rw2_tca/third_body/*/` (16 RW2s, not committed)

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

### What would resolve 0x0119's missing scaler

Superseded by "word[7] high byte decode (session 4)" above: items 1
was executed with negative results (no simple function of word[7]'s
high byte predicts the observed `k`; the observed `k` variation was
itself a measurement artifact). Items 2 and 3 stayed on the shelf and
are still worth doing if the finding above is ever revisited:

1. ~~Fit the observed k against the high byte of word[7] across the
   12-60 sweep.~~ Done in session 4. All simple encodings score R^2 <
   0.15. See "word[7] high byte decode".
2. Check other Panasonic zooms (Lumix 14-42, 12-32, 100-300 if any
   are available) - do they also use the varying high-byte flag
   encoding, or is it specific to the Leica DG line? Would establish
   whether the varying `w[7]_hi` is a lens-family attribute (Leica DG
   only) or a general Panasonic zoom pattern.
3. Look at the Panasonic RW2 ImageMagick or LibRaw source for any
   comment on the flag byte's extra bits. Panasonic-specific raw
   decoders sometimes carry undocumented decode notes.

If any subsequent evidence resurrects the missing-scaler hypothesis,
add the scaler to `_init_coeffs_md_v2`'s Panasonic branch as a
multiplicative factor before the polynomial evaluation, gate on the
identified `word[7]` bits, and re-check the 12-60 output.

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
5. **A larger corpus with in-camera CA-toggle or distortion-toggle
   pairs.** Both the G9 and the GX80 in this session's corpus turned
   out to have neither toggle exposed in the menu (verified against
   the DVQP1406ZA operating instructions and by hand on both bodies);
   Panasonic applies these corrections automatically per-lens with no
   user override. A body that *does* expose the toggle (G9 II uses
   DVQP3010 and has [Distortion Comp.] in the [Rec] menu; GH-series
   from a specific firmware onwards may also) would let the 2x2
   factorial we originally intended.

Absent any of the above, an honest verdict is: 0x011b's coefficient
semantics remain undecoded on the current 9-file G9 corpus. The
structural findings above (checksums, radii, flag, opaque, smooth vs
discrete) are what should feed the darktable code changes described
under "Where the code needs to change"; the coefficient path should
remain a stub until the CA-toggle control shots or SILKYPIX-Pro
reference exists.

### Magnitude calibration against camera JPEG (session 7)

Session 6 left a factor-of-ten gap between what C_R predicts (in
pixels of R-vs-G displacement) and what a rawpy AAHD render of the
same file shows. Session 6 was clear that this gap sits on the
training corpus itself, so it is a property of Adobe's opcode vs
rawpy's demosaic, not of the C_R decode. This session addresses the
follow-on question: **the camera JPEG is what the darktable patch is
supposed to reproduce, not Adobe's DNG rendering.** So measure the
CA that the camera JPEG carries, difference it against the raw, and
calibrate C_R's magnitude against that.

**Method.** For the six-file subset (Sigma 16, PL 12-60 @ 12mm,
PL 12-60 @ 60mm on both G9 and GX80):

1. Render the RW2 uncorrected with rawpy AAHD (same pipeline as
   `step3_measure.py` and `step4_measure_v2.py`).
2. Load the paired camera JPEG.
3. Coarse-register raw -> JPEG with SIFT + RANSAC partial affine
   (scale + rotation + translation, `cv2.estimateAffinePartial2D`),
   same registration as `step6_02_prove_jpeg_distortion.py`.
4. Pick 15000 edges on the raw's green channel, filter to radial
   gradients (`|cos(gradient, radial)| > 0.3`, r_pix >= 20).
5. Measure R-G and B-G radial displacements in the raw at each edge
   using the parabolic sub-pixel fit from `step4_measure_v2`.
6. Warp each edge position from raw coordinates to JPEG coordinates
   via the estimated affine, rotate the local gradient direction by
   the same affine (the scale part cancels in the unit vector), and
   measure R-G and B-G at that warped position in the JPEG with the
   same parabolic fit.
7. Divide the JPEG measurement by the affine scale to bring it into
   raw-pixel units, then take
   `applied_correction = raw_RG - jpeg_RG` per edge.
8. Bin by `r_norm = r / half_diagonal_raw` and take medians.

Scripts: `/tmp/rw2_tca/step18_01_measure_jpeg_ca.py` and
`/tmp/rw2_tca/step18_02_aggregate.py`. Per-edge data at
`/tmp/rw2_tca/step18_ca.npz`; aggregate at
`/tmp/rw2_tca/step18_02_summary.json`.

**Camera JPEG carries essentially no CA.** Across all six files and
all populated radial bins, the JPEG's median R-G radial displacement
sits in `[-0.02, +0.01]` px and B-G in `[-0.02, +0.02]` px, i.e. at
or below the parabolic fit's per-bin scatter. Whatever CA the camera
sees, it corrects to under the measurement noise floor before writing
the JPEG. So `applied_correction ~= raw_RG` on every signal bin, and
the raw's own CA magnitude is what the camera applies.

**Predicted / applied ratio per bin (R-G, signal bins only).**
"Signal bin" means `|predicted| > 0.05 px` AND `|applied| > 0.02 px`
(smaller bins drop out; there are 26 signal bins across the six
files).

| file (body / lens / focal)         | n_bins | median | q1     | q3     | w_mean |
|:-----------------------------------|-------:|-------:|-------:|-------:|-------:|
| P1366477 G9   PL 12-60 @ 12mm      |      5 |   9.91 |   9.51 |  10.72 |   9.72 |
| P1366479 G9   PL 12-60 @ 60mm      |      5 |  13.29 |  13.01 |  13.56 |  12.44 |
| P1366486 G9   Sigma 16 @ 16mm      |      4 |  14.07 |  12.50 |  22.03 |  17.34 |
| P1260633 GX80 PL 12-60 @ 12mm      |      5 |   6.18 |   4.62 |   7.35 |   5.60 |
| P1260635 GX80 PL 12-60 @ 60mm      |      3 |  13.05 |  11.05 |  13.67 |  11.62 |
| P1260639 GX80 Sigma 16 @ 16mm      |      5 |   8.05 |   7.22 |   8.32 |   7.63 |

- median of per-file medians: 11.48
- mean of per-file medians:   10.76
- std (population sample):     3.22
- spread (max/min):            2.28x  (14.07 / 6.18)
- constant (cv < 25%):         false; cv = 30%
- per-body median: G9 = 13.29, GX80 = 8.05

Sign matches on 26 of 26 signal bins on all six files. The B-G
channel's applied correction is smaller than the R-G on every file
(matches session 5's observation that B has a shorter dynamic range
than R), and the predicted B-G from the low-order C_B_lo decode is
of the same order as the applied B-G in the interior; corner B-G
predictions run larger than the measurement noise floor allows to
resolve.

**Interpretation against the session brief's decision tree.** This
is outcome 2, "systematic mismatch (3-10x)". Not outcome 1 (match
within 30%): the median ratio is 11.5, not 1. Not outcome 3 (sign
disagreement): sign is preserved on every signal bin.

Not fully constant, though. The ratio spans 6.18 to 14.07 across the
six files. That is a 2.28x spread, cv = 30%. Baking a single global
scale factor into the darktable patch will therefore be right on the
scale, wrong on the shape:

- a scale of 1/11.48 (median-of-medians) leaves the G9 side lightly
  under-corrected on the low-CA GX80 files;
- a scale of 1/6.18 (worst-case, the strongest GX80 file) will
  over-correct on the G9's 60mm and Sigma 16 files by ~2x;
- picking per-body scales (G9 1/13.3, GX80 1/8) narrows the spread
  but has no obvious signal in 0x011b to gate on, since the body-
  scaled words are the four radii and the flag, not any coefficient
  we could read at correction time.

**Root of the gap: it is Adobe's opcode, not the C_R fit.** To
verify, `step18_03_check_dng_normalization.py` reads the DNG
`WarpRectilinear` opcode for each of the six files directly and
computes `r_pix * (poly_R(R^2) - poly_G(R^2))` at the same r bins.
The DNG opcode itself predicts +0.34 to +1.14 px of R-G shift on
these files, against measured raw R-G of +0.02 to +0.14 px. Ratios:

```
P1366477 (PL 12-60 @ 12mm):  9.05, 10.73,  6.74,  9.95  (four bins)
P1366479 (PL 12-60 @ 60mm):  6.76, 12.78, 13.23, 10.52
P1366486 (Sigma 16 @ 16mm): 13.47, 10.73, 30.86,  8.82
P1260633 (PL 12-60 @ 12mm):  6.33,  7.11,  4.65,  4.24
P1260635 (PL 12-60 @ 60mm): 19.47, 11.50, 10.95, 10.85
P1260639 (Sigma 16 @ 16mm):  8.62,  8.94,  7.23
```

Adobe's opcode is asking for a much larger correction than the raw
actually needs. The C_R decode reproduces Adobe's opcode within a
few percent (session 5), so it inherits the same gap. This is the
same 10x factor session 6 flagged, seen here through the DNG opcode
itself with no C_R involvement.

**Theory 4 checks (~20 min).**

*4a. Bayer-plane application.* DNG's `OpcodeList3` (where this
opcode lives, session 5) applies to the demosaiced, color-converted
image, not to Bayer-plane data (`OpcodeList1` / `OpcodeList2`). A
Bayer-plane application would attenuate the effective post-demosaic
displacement by the 2x sparse-sampling factor at most, not by 10x.
Not a match.

*4b. Missing multiplicative constant.* The DNG G-plane polynomial
value at R=1 is 0.923 on PL 12-60 @ 12mm (i.e. about 8% pincushion
mapping to invert 8% raw barrel), which matches Panasonic's 0x0119
`a` coefficient converted through the Rigo formula. So the G-plane
normalization is right. The R and B plane polynomials sit within
`0.999 +/- 0.001` of the G plane at every R, which is how a per-file
CA correction of only 0.1 px should look at a half-diagonal of 3240
px (0.1/3240 = 3e-5). But when we multiply the differential by
r_pix, that same small per-file coefficient dresses up to a ~0.8 px
shift. Nothing about the DNG geometry tags
(`ActiveArea`, `DefaultCropOrigin`, `DefaultCropSize`) suggests an
alternative normalization: the crop origin is `(12, 8)` and crop
size is `5184 x 3888`, matching the JPEG's dimensions, and the
opcode's center is `(0.5, 0.5)` of that active area.

Both theories are marked unresolved. The clean way to close the
question is to render one of these DNGs through Adobe Camera Raw or
LibRaw's WarpRectilinear implementation and diff the R plane
against the same file rendered with the opcode disabled; that
directly measures whether Adobe's own pipeline applies the opcode
at its printed magnitude. Neither tool was available in this
session.

**Confidence.**

- The measurement of `applied_correction = raw_RG - jpeg_RG` is
  trustworthy: per-file half-vs-half RMS on the raw side sits in
  0.01-0.03 px on the strong-CA files (`step18_01.log`), and the
  JPEG side is essentially at zero everywhere, so the difference
  inherits the raw's own precision. The signs match on 26 of 26
  signal bins. That is a real signal, not a wash.
- The predicted / applied ratio of 11.5 +/- 3.2 across the six
  files is real, and is not a C_R decode artifact: the same ratio
  falls out of a direct read of the DNG opcode in
  `step18_03_check_dng_normalization.py`.
- The spread of 2.3x across the corpus is enough to say the ratio
  is not a global constant. A single-scale patch of C_R will be
  wrong per-file by a factor of ~2 either way. That does not mean
  the shape is wrong: sign and per-radius shape reproduce cleanly.
  It means the darktable patch's `k_r[i]` will need to be gated on
  something we have not identified, or shipped with an
  intentionally-averaged scale that is known to over-correct some
  files and under-correct others by ~2x.

**Where this points a follow-on darktable patch.**

1. Do not ship C_R as-decoded. It will over-correct by roughly an
   order of magnitude on every Panasonic RW2, which is visibly
   worse than the current TCA-free state.
2. If a single-scale patch ships, use median-of-medians = 11.48 as
   the divisor. Expected error: 2x over-correction on the strongest
   GX80 12mm files, 20% under-correction on the strong-CA G9 files.
   That is at least of the right sign everywhere, but the per-file
   error will be visible on some samples.
3. A per-body scale (G9: divide by 13.3; GX80: divide by 8.0)
   halves the spread but has no in-file signal to key on; a
   `Model`-string branch in the darktable code would work but is
   fragile.
4. The 10x-gap root cause between Adobe's opcode and the physical
   raw CA is genuinely unresolved. Resolving it (e.g. by
   rendering a DNG through Adobe Camera Raw with the opcode on and
   off, and diffing the R plane) may reveal that the correct patch
   is not "scale C_R by 1/11" but "apply C_R via a different
   evaluation path" that intrinsically produces the right
   magnitude. Recommend doing this before committing to a
   scale-and-ship approach.

Scripts and data this session:

- `/tmp/rw2_tca/step18_01_measure_jpeg_ca.py`: pair-wise raw and
  JPEG CA measurement, per-file per-bin summary, npz output.
- `/tmp/rw2_tca/step18_02_aggregate.py`: per-file and across-corpus
  predicted / applied ratios, JSON summary.
- `/tmp/rw2_tca/step18_03_check_dng_normalization.py`: DNG opcode
  direct read, comparing DNG R-G shift against raw measurement
  without C_R in the loop.
- `/tmp/rw2_tca/step18_ca.npz`, `/tmp/rw2_tca/step18_02_summary.json`.
- Log files: `/tmp/rw2_tca/step18_01.log`, `.../step18_02.log`,
  `.../step18_03.log`.

### JPEG-referenced decode (session 8)

Session 7 established that Adobe's WarpRectilinear opcode asks for a
correction roughly 11x stronger than what the camera JPEG actually
applies, and that the camera JPEG is what a darktable patch has to
reproduce for parity with the in-camera look. Session 8 asks: is the
session-5 decode structurally correct (same coefficients, wrong
scale), or does it need refitting against the JPEG-derived
`applied_correction = raw_RG - jpg_RG` target?

**Corpus.** All 18 files, not just session 7's six-file subset:
9 GX80 + 9 G9, each paired with a same-frame camera JPEG. Measurement
pipeline unchanged from session 7 (`step18_01_measure_jpeg_ca.py`
imported into `step19_01_measure_full_corpus.py`): render the RW2 with
rawpy AAHD, SIFT+RANSAC affine-align to the JPEG, sub-pixel-fit R, G, B
edges on both, difference in raw-pixel units. Data at
`/tmp/rw2_tca/step19_ca.npz` (207 (file, radial bin) pairs, 146 R
signal bins with `|applied_RG| > 0.02` px, 76 B signal bins).

The full corpus reproduces session 7's finding: the JPEG carries
essentially no CA on any file (per-bin median `|jpg_RG|` sits in
[0, 0.02] px on 200 of 207 bins), and `applied_correction ~= raw_RG`
on every signal bin.

**Approach 1: single global scale factor on session 5's C_R.**

Divide the session-5 prediction by K:

```
pred_applied_RG(f, r_norm) = r_pix * poly4(C_R @ words_R[f], r_norm) / K
```

Same for `pred_applied_BG` via `C_B_lo`. Scanning K in 1..15 on
`step19_02_scale_test.py`, the optimum sits at:

- K_R = 9.008 (least squares on 146 R signal bins across all 18 files)
- K_B = 10.480 (76 B signal bins)
- K = 11.48 (session 7's median-of-medians) sits within 20% of the
  R-channel optimum and within 10% of the B-channel one

Per-file RMS residual in pixels at K = 11.48 across the 18 files:

| channel | median RMS | max RMS  | median max_err | max max_err |
|:--------|-----------:|---------:|---------------:|------------:|
| R       | 0.028 px   | 0.058 px | 0.051 px       | 0.108 px    |
| B       | 0.021 px   | 0.034 px | 0.038 px       | 0.063 px    |

The worst-case per-file RMS is 6% of a Bayer super-pixel, and the
worst-case single-bin residual is 0.11 px, which happens on the
strongest-CA file (GX80 PL 12-60 @ 12mm, max `|applied_RG|` = 0.16 px,
so 68% of peak signal). All 17 other files have max_err below 60% of
their peak `|applied_RG|`.

R^2 on all 207 (file, bin) pairs at K = 11.48:

- R channel: R^2_all = 0.588, R^2_signal_bins = 0.655
- B channel: R^2_all = 0.252, R^2_signal_bins = 0.377

Sign of the prediction matches the sign of the measured applied
correction on 140/146 = 96% of R signal bins and 57/76 = 75% of B
signal bins. The 6 R sign flips sit on bins where `|applied_R|` is
close to the 0.02 px signal threshold.

**Approach 2: refit 24 coefficients against the JPEG target.**

Two shapes were tried:

- Per-file polynomial then linear regression against words
  (`step19_03_refit.py`, mirrors session 5's structure). Fit poly4
  `D_target[f]` to the per-bin `applied_RG / r_pix` for each file, then
  regress `D_target[f, N] = sum_j C_JPEG[N, j] * word[f, j]`. The
  per-file poly4 fits are ill-conditioned: `|D_target|` at the corner
  sits at 1e-4 while measurement noise on `applied_RG` is 0.02 px
  spread over ~3000 raw-pix corner radius, so `y = applied_RG / r_pix`
  has SNR ~ 6 at the corner and worse at small r. The higher-order
  coefficients trade off wildly, and cross-body pairs of the same
  lens differ by 3-6x on `k_r{2,3}`. LOGO R^2 is negative on all four
  R coefficients and all four B coefficients.

- Direct pixel-space fit (`step19_04_direct_fit.py`). One giant OLS on
  all 207 bins with 24 unknowns:
  `applied_RG(f, b) = sum_{N=0..3} r_pix(b) * r_norm(b)^(2N) * sum_j C[N, j] * word[f, j]`.
  Better-conditioned (cond(X) = 2.4e4). Train R^2 = 0.808 on R and
  0.530 on B, but under LOGO the fit generalises worse than the scale
  factor: LOGO R^2 = -6.11 on R and -0.99 on B, and per-file RMS
  degrades to 0.043 px median, 0.38 px max on R. Ridge in
  [1e-6, 0.1] leaves the fit unchanged.

Neither refit variant beats the scale factor. The problem is not
conditioning; it is signal. The applied correction sits at 0.02-0.15
px across the corpus, the polynomial has 4 degrees of freedom per
channel, and JPEG-side measurement noise, JPEG sharpening bias on
sub-pixel edge positions, distortion-correction interaction and
affine-alignment residuals together dominate the higher-order
polynomial coefficients.

**Cross-body behaviour of K.**

Fitting K separately per body on the full corpus:

| body | K_R   | K_B   |
|:-----|------:|------:|
| G9   | 10.39 | 12.69 |
| GX80 |  7.28 |  7.70 |

Cross-body generalisation (fit K on one body, predict the other,
`step19_05_scale_variants.py`):

- G9 -> GX80, R channel: K = 10.39, R^2 = 0.671, RMS = 0.029 px
- G9 -> GX80, B channel: K = 12.69, R^2 = 0.381, RMS = 0.021 px
- GX80 -> G9, R channel: K =  7.28, R^2 = 0.143, RMS = 0.044 px
- GX80 -> G9, B channel: K =  7.70, R^2 = -0.60, RMS = 0.030 px

Body-specific K reduces max per-file RMS from 0.058 to 0.050 px on R
and from 0.034 to 0.031 on B. The improvement is real but small; a
constant K in [9, 12] captures most of the signal.

**Physics sanity checks.**

- *Small-CA lens (Lumix 42.5).* Predicted `pred_applied_RG(r=0.5)` in
  pixels: G9 = -0.012 px, GX80 = -0.012 px. Measured `applied_RG`
  medians at 0.46 <= r_norm < 0.55: G9 = +0.010 px, GX80 = +0.012 px
  (from `step19_07`). Both prediction and measurement sit at or below
  the 0.02 px measurement noise floor. Session 5 already noted that
  word[8] is small on this lens; the K = 11.48 scaling turns that
  small-word signal into a small predicted correction, consistent with
  the effectively-zero measurement.
- *Cross-body agreement.* On the paired (lens, focal, radial-bin)
  triples where both bodies have `|applied_RG| > 0.02` px (57 R
  signal-bin pairs across the 9 lens/focal groups,
  `step19_07_cross_body_verify.py`), sign matches on 55 of 57 = 96.5%
  R pairs, and 50 of the 55 same-sign pairs have magnitudes within a
  factor of 2 (all 55 within 3x). The B channel has 12 signal-bin
  pairs; 11 match sign and all 11 same-sign pairs stay within 2x.
- *Sign flip along PL 12-60 zoom.* Measured `applied_RG` sign at the
  middle radial bins: 12mm +, 25mm +, 60mm -; predicted sign at K =
  11.48: 12mm +, 25mm +, 60mm -. Sign flip reproduces on both bodies.

**JPEG sharpening caveat.** Camera JPEG output has been sharpened, and
sharpening can bias sub-pixel edge positions by 0.01-0.05 px depending
on edge contrast and gradient orientation. The per-bin median
`jpg_RG` values sit inside [0, 0.02] px, which is comparable to the
sharpening bias size; the 0.05 px per-file max_err residual against
K = 11.48 is at the same scale. Preprocessing the JPEG with a small
Gaussian to undo sharpening was not run in this session; the fit
proceeds as-is, and the residuals should be read as "measurement
noise floor plus any sharpening bias plus real per-file scale spread",
not "unexplained decode error".

**Decision: ship the scale factor.**

The refit does not beat the scale factor on cross-validation, and the
scale factor already delivers per-file RMS well under one Bayer
super-pixel across the corpus. The session-5 C_R decode is
structurally correct against the JPEG target; only the magnitude is
off, by a body-dependent factor in [7, 13] with median 11.48.

**Formula for the darktable patch (JPEG-referenced, session 8).**

```
static const int words_R[6] = {8, 10, 12, 20, 23, 27};
static const double C_R[4][6] = {
  { -5.5919e-08, -2.7534e-07, -1.0043e-06, +9.4388e-08, +8.1750e-08, +3.1028e-07 },
  { +1.7918e-06, +3.4704e-07, +5.4376e-06, -1.0369e-07, -4.9216e-06, -4.6536e-07 },
  { -4.0368e-06, +2.2315e-06, -7.8190e-06, -1.0252e-06, +8.9802e-06, -1.7742e-06 },
  { +1.5442e-06, -3.2808e-06, +3.1874e-06, +1.5409e-06, -3.5842e-06, +2.5324e-06 },
};
static const double C_B_lo[2][6] = {
  { +1.1514e-07, +3.7170e-07, +9.8105e-09, -1.2143e-07, -1.4375e-07, -1.4212e-06 },
  { -3.3139e-07, -4.9106e-06, -9.7770e-08, +1.6006e-06, +4.4665e-07, +5.8716e-06 },
};

/* Session 8 JPEG calibration: divide the predicted differential by K
   before writing it into the pipeline coefficients. K = 11.48 is the
   session 7 median across bodies; per-body K sits at 10.4 (G9) and
   7.3 (GX80) but body-conditional selection has no in-file signal to
   key on. Corpus RMS at K = 11.48: R channel 0.028 px median,
   0.058 px max; B channel 0.021 px median, 0.034 px max. Sign of the
   correction matches JPEG-measured applied CA on 96% of R and 75% of
   B signal bins.

   for i in 0..3: plane_R.k_r[i] = plane_G.k_r[i] + (sum_j C_R[i][j] * word[words_R[j]]) / K
   for i in 0..1: plane_B.k_r[i] = plane_G.k_r[i] + (sum_j C_B_lo[i][j] * word[words_R[j]]) / K
   plane_B.k_r[2] = plane_G.k_r[2]  (no reliable decode; safer than a
   wrong one on strong-CA files)
   plane_B.k_r[3] = plane_G.k_r[3]  (ditto) */

static const double K = 11.48;
```

**Bounds on this claim.**

- The 18-file corpus covers 5 lenses on 2 bodies; two bodies is not
  enough to say K = 11.48 is a universal constant. A third body could
  land outside the 7-13 range this corpus shows. Recommended check on
  future third-body data: repeat `step19_02_scale_test.py` on the new
  RW2 + JPEG pairs and report the per-file `raw_RG - jpeg_RG` /
  `C_R * words * r_pix * poly4` ratio.
- Camera JPEG sharpening biases sub-pixel edge positions at the ~0.02
  px scale, which is comparable to the residual left after applying
  K = 11.48. A Gaussian pre-blur of the JPEG before edge measurement
  would tighten the bound but was not run in this session; the fit
  quality reported above is a conservative upper bound.
- The B channel has smaller applied CA (max `|applied_BG|` = 0.10 px
  vs 0.21 px for R), so its R^2 is lower even when the RMS residual
  is smaller. RMS is the more honest metric here.
- The direct refit's train R^2 = 0.808 on R vs 0.588 for the scale
  factor is real, but that gain is entirely overfit: LOGO R^2 goes
  to -6.11. There is no coefficient matrix inside this corpus that
  generalises better than the scale factor.
- The remaining per-body spread (K ranging 7-13) is unexplained. It
  is consistent with (a) the two bodies having genuinely different
  in-camera CA-correction gains, (b) demosaic-algorithm bias in
  rawpy AAHD interacting with the two bodies' colour filter arrays
  differently, or (c) affine-alignment residuals differing between the
  two bodies' JPEG-vs-raw crop offsets. Neither of those was pinned
  down in this session.

**Scripts and data this session:**

- `/tmp/rw2_tca/step19_01_measure_full_corpus.py`: extends session 7's
  per-file JPEG-vs-raw CA measurement from the six-file subset to all
  18 corpus files. Reuses `step18_01_measure_jpeg_ca.process_file`
  unchanged.
- `/tmp/rw2_tca/step19_02_scale_test.py`: K sweep, per-file RMS,
  optimal K by least squares.
- `/tmp/rw2_tca/step19_03_refit.py`: per-file poly4 fit then linear
  regression against words, with LOGO cross-validation. Fails on all
  8 coefficients under LOGO.
- `/tmp/rw2_tca/step19_04_direct_fit.py`: direct pixel-space 24-DOF
  fit with OLS and ridge. Train R^2 gains disappear under LOGO.
- `/tmp/rw2_tca/step19_05_scale_variants.py`: per-body K,
  cross-body K generalisation, per-file K spread.
- `/tmp/rw2_tca/step19_06_summary.py`: final decision printout.
- `/tmp/rw2_tca/step19_07_cross_body_verify.py`: per-(lens, focal,
  bin) cross-body sign and magnitude verification for the applied
  correction; produces the 55/57 and 11/12 numbers above.
- `/tmp/rw2_tca/step19_ca.npz`, `/tmp/rw2_tca/step19_scale_bins.npz`,
  `/tmp/rw2_tca/step19_refit.npz`, `/tmp/rw2_tca/step19_direct_fit.npz`.
- Log files: `/tmp/rw2_tca/step19_01.log`, `.../step19_02.log`,
  `.../step19_03.log`, `.../step19_04.log`, `.../step19_05.log`,
  `.../step19_06.log`, `.../step19_07.log`.

### SILKYPIX static analysis (session 9)

Panasonic ships SILKYPIX Developer Studio 8 SE with its cameras. The
plausible read of that shipping decision is that SILKYPIX applies whatever
Panasonic's own reference algorithm for 0x011b is. This session performed
static analysis on the SE 8 install to lift that algorithm, so it could
either confirm sessions 5-8's decode or replace it.

The short version is that the decode was *not* refuted, but neither was the
algorithm fully extracted. Session 8's coefficient matrix and `K ~ 11.48`
stay the recommendation. What the analysis did produce is a much better
understanding of what shape the extraction task has, so a future session
with more time can go further.

**Tool used.** radare2 install failed (sudo unavailable, no cached apt),
and Ghidra headless would have blown the 20-minute install budget
mentioned in the session brief. The DLL was inspected instead with the
combination `pefile` + `lief` + `capstone` that was already available in
the corpus venv. This is enough for section and RTTI walking, function-
boundary discovery via `.pdata` runtime-function entries, and targeted
disassembly, which is what the session needed. See
`/tmp/rw2_tca/step20_silky_re.py` and `.../step20_silky_re.log` for the
reproducible dump.

**What was disassembled.** `SILKYPIX64.dll` only. The `avx` variant, the
`x64/*.dll` support libraries, and `SILKYPIX_DS8SE.exe` were not needed
once the primary DLL was mapped and shown to contain the Panasonic
handling. The RTTI descriptor set in `.data` names 31 `IslZTiffExif*`
subclasses, including `IslZTiffExifPanasonic` (TD RVA `0x1a3a450`) and
`IslBRawReadPanasonic0` (TD RVA `0x1a62080`). Vtables were located via the
standard MSVC RTTI chain: type descriptor RVA -> 4-byte reference in
`.rdata` (that is the `+0x0c` field of a `CompleteObjectLocator`) -> COL
start at hit - 0x0c -> the 8-byte VA of that COL is found again in `.rdata`
-> the vtable begins 8 bytes further in. This is worth writing down
because casual "find references to the class-name string" searches turn
up almost nothing: MSVC uses the RTTI descriptor VA, not the string VA,
and does so as a 32-bit RVA embedded in 20-byte COL records, not as an
absolute pointer.

**Data-file inspection.** `DefaultLensInfo.spd` and `DefaultParameters.spd`
are both in an internal "ISL Multi purpose file format" (magic string at
file offset `0x100`, header table at `0x140`, encrypted or compressed
payload starting near `0x148`). Their sibling `.spx` files share a common
initial 24-byte header, then diverge. Nothing in these files can be
grepped for lens or model names. Decoding the container is beyond a
two-hour static-analysis budget and was not attempted; it would be a
separate project.

**Panasonic-unique virtual functions.** `IslZTiffExifPanasonic` has a
200-entry vtable. Comparing it against three sibling classes
(`IslZTiffExifCanon`, `IslZTiffExifSonyARW`,
`IslZTiffExifWithMakerNote`), Panasonic overrides seven slots:

```
slot 1   rva 0x004807d0
slot 3   rva 0x0048f750
slot 6   rva 0x0048ffd0
slot 8   rva 0x00480860
slot 12  rva 0x0048bfa0     (returns constant 0x6e; probably a format id)
slot 17  rva 0x0048fae0
slot 155 rva 0x0048bd80
```

Slots 3, 6, 17 and 155 are the substantive ones (multi-hundred-instruction
prologues with lots of local storage); the others are constructor / copy
helpers. Which of these implements the CA-tag readout was not established
by direct inspection.

**Panasonic private-IFD field layout.** Much more useful than the vtable
was a copy table found at file offset `0x17abdc4` (RVA `0x17accc4`). It is
an array of 8-byte entries of the form `{ u32 tag_id, u32 stub_rva }`,
running from tag `0x010f` through `0x0132`. Each `stub_rva` is a short
piece of code inside one large function (0x4c64b0..0x4c9cf2, ~14 KB) whose
job is `dst[+offset] = src[+offset]` per tag: the class's copy-
assignment operator. Reading each stub gives the class-field offset that
holds each Panasonic private-IFD tag:

```
tag 0x117  -> obj + 0x10550
tag 0x118  -> obj + 0x107c8..+0x107e0 (five words)
tag 0x119  -> obj + 0x12698..+0x126a8 (distortion, session-1 territory)
tag 0x11a  -> obj + 0x12920..+0x12930 (selector, on/off word)
tag 0x11b  -> obj + 0x129a8           (chromatic aberration payload)
tag 0x11c  -> obj + 0x12a20
```

Tag `0x11b`'s field lives at `obj + 0x129a8`. That is the anchor point for
every downstream question: where the field is *set* is the tag reader,
where it is *consumed* is the CA correction.

**Where 0x11b is written from the raw file.** Five functions in `.text`
address `obj + 0x129a8`:

```
0x004c64b0..0x004c9cf2   size 0x3842   hits 2   (the copy-assign above)
0x004c9d00..0x004cbae6   size 0x1de6   hits 1
0x004cf610..0x004d11c2   size 0x1bb2   hits 2
0x004f07a0..0x004f2052   size 0x18b2   hits 1
0x004f3ca0..0x0050994d   size 0x15cad  hits 9   (private-IFD parser)
```

The 88-KB function at `0x4f3ca0` is where the private-IFD walk lives.
It reads the 0x11b payload into its container field, but at that size and
with MSVC's optimizer having flattened everything into an indirect-jump
dispatch (no `cmp reg, 0x11b` compares survive in the emitted code , 
`0x11b` appears only as `mov reg, imm` loads elsewhere, not as a switch
key), lifting the exact `words[8], words[10], ..., words[27] x coefficients`
sequence out of it needs a decompiler, not linear disassembly. That
lift was not completed.

**Consumer side.** Search for constants that would give away the CA math
turned up nothing decisive:

- `11.48` (both f32 and f64) does not appear as a stored constant anywhere
  in the DLL. Nor does `11.484`, nor `1/11.48`. The `K ~ 11.48` scale from
  session 8 is therefore *not* a single code literal. It is either
  synthesised from a shift-and-scale (right-shift by three or four bits
  plus a fixed rescale) or, more likely, composed from several separately-
  applied constants that the empirical fit collapsed into one factor.
- `11.5` appears 12 times as f32 and once as f64, but those hits are
  scattered across unrelated code (JPEG quantisation, tone-curve knots)
  and none of them sit next to Panasonic-adjacent code.
- The four-radius Homeister zone table `{0.333, 0.667, 0.833, 1.0}` is
  not present as consecutive f32 or f64 doubles, in either MFT or
  full-frame-S form (`{2/7, 4/7, 6/7, 1}`). The values `0.333` and `0.667`
  each occur twice as f64, but never adjacent, never at 8-byte stride
  with matching neighbours. If SILKYPIX uses zone radii at all, it
  computes them on the fly rather than reading them from a table.

**Body-conditional logic.** Model strings (`DC-G9`, `DC-S5`, `DC-GH5`,
`DMC-*`, etc.) are all present and each is referenced by exactly one
site in `.text`. That is consistent with a per-model early-return in
some routing function, not with per-model CA tuning: the 7-13 K spread
seen in session 7 is far more likely to come from lens-database rows
inside `DefaultLensInfo.spd`, which we cannot read, than from any code
branch we can see.

**What this tells us.**

1. Nothing extracted from SILKYPIX *contradicts* sessions 5-8's decode.
   No stored constant showed up that says "the Homeister zone model is
   wrong" or "the six-word predictor set is a five-word set". The
   session-8 model can be shipped.

2. The `K ~ 11.48` factor is not one number in the DLL. Chasing it as a
   single decode constant is a dead end; it is an artefact of the sum of
   several fixed-point operations in the algorithm and possibly of a
   per-lens attenuation from the `.spd` database.

3. The B-plane high-order gap that session 5 could not close is *not*
   solved here. Without following the 88-KB private-IFD parser into its
   consumer, we don't know whether the `k_r2_B` / `k_r3_B` coefficients
   come from a different subset of the 32 words, from a mirror formula
   against R, or from `DefaultLensInfo.spd` entries. Session 8 fits it
   empirically from the JPEG; that empirical fit remains the best model
   until someone completes the decompiler-assisted read of the parser
   function.

4. No body-conditional logic worth adding to the darktable patch was
   found. Body strings are referenced once each, which fits a routing
   step and not a CA-tuning step.

**Honest bounds.**

- The full end-to-end algorithm (tag reader + checksum validator +
  coefficient extractor + polynomial evaluator + per-pixel application)
  was *not* extracted from the DLL. Without a decompiler, unwinding an
  88-KB indirect-jump-heavy function from linear disassembly is not
  realistic within a two-hour budget.
- `DefaultLensInfo.spd` and `DefaultParameters.spd` remain opaque. If
  SILKYPIX applies a per-lens attenuation on top of the RW2 correction , 
  which would neatly explain the `K` factor and the per-body spread , 
  the data for that attenuation is in these files, and lifting it
  requires either running the app under a debugger or reverse-engineering
  the ISL container format. Neither was done.
- The seven Panasonic-unique virtual functions are located but not
  labelled. Slot 3 (0x48f750) and slot 17 (0x48fae0) are large enough
  to plausibly be the tag-parse and CA-apply entry points respectively,
  but this is inference from size, not verified from behaviour.

**Recommendation for the darktable patch.**

Ship session 8's decode as it stands. That means the six-word predictor
set `[8, 10, 12, 20, 23, 27]`, the `C_R` and `C_B_lo` matrices, and the
`K = 11.48` fit against the camera JPEG. Nothing in the SILKYPIX binary
contradicts them, and no cleaner alternative was recoverable in the time
available. Do *not* add body-conditional scaling to the patch. The
per-body K spread that session 7 saw is a genuine phenomenon but there
is no evidence it lives in code as opposed to per-lens data, and
guessing at it would hurt more than it helps.

**Reproducer.** `/tmp/rw2_tca/step20_silky_re.py` (and
`.../step20_silky_re.log`) contains the enumeration, the vtable diff,
the tag->field-offset table, and the constant/table presence checks.
Nothing in this section depends on a SILKYPIX file being in the tree;
the script reads the DLL from `/tmp/rw2_tca/silky/` and produces the
log used above.

### SILKYPIX decompile (session 10)

Session 9 stopped at the 88 KB private-IFD parser boundary because
linear disassembly was not enough to lift its body. This session got a
decompiler working against the DLL and used it to answer, for each of
the five session-9 questions, whether the SILKYPIX binary contains
enough evidence to decide it. Two questions were settled outright, two
were partially answered with strong negative evidence, and one remains
open. Session 8's decode still stands as the shipping recommendation;
what tightened is our confidence that the polynomial evaluator is not
merely hidden behind a decompiler barrier -- it is structurally not
accessible from static class-hierarchy analysis without also lifting
the SILKYPIX plugin factory chain and the encrypted `.spd` container
format.

**Decompiler that worked.** Ghidra 12.1.3 headless (`analyzeHeadless`),
downloaded fresh from the NSA release. Path 1a (r2ghidra tag 5.5.0 to
match radare2 5.5.0) recompiled cleanly through preconfigure/configure
but failed at `R2Scope.cpp:19` on a `next_id` member the current
`R2Scope` header no longer declares. Not chased; skipped per session
brief. Path 1b landed on the first attempt: 570 MB zip in 60 s, unpack
in 5 s, import + auto-analysis of `SILKYPIX64.dll` in 17.5 min (32 min
of user-CPU across the analyser passes -- Stack analysis 331 s, x86
constant reference analyser 246 s, Decompiler switch analysis 143 s).
Once the project was saved, subsequent script runs open the analysed
program without re-running any pass, and are seconds to minutes.

Two Ghidra behaviours worth writing down for the follow-on agent:

- The 58 KB pcode function body at RVA `0x4f3ca0` exceeds the
  decompiler's default per-function budget. `decompileFunction(fn,
  300, monitor)` returned `process: timeout`; raising the timeout to
  900 s still did not complete. This is not a bug we should chase --
  the parser is large by design (one flat MSVC-optimised tag
  dispatcher) and the useful information lives in the functions it
  *calls*, which are all well under 8 KB and decompile in seconds.
- Ghidra's decompiler expresses struct-field access as
  `param_1 + 0x129a8` in most places but replaces the literal with a
  named local when the parameter gets typed. For a fresh project the
  literal form dominates. `grep 0x129a` was reliable enough to enumerate
  every touch point.

Reproducer scripts (Java, dropped into a Ghidra script path so
`-postScript` picks them up), all under
`/tmp/rw2_tca/ghidra_scripts/`:
`DecompileByRVA.java`, `FindFieldXrefs.java`, `FindConstXrefs.java`,
`ListSites.java`, `FindRefsToAddr.java`, `FindClassVtable.java`,
`GrepStrings.java`, `DumpTable.java`. The outputs are the `step21_*`
files in `/tmp/rw2_tca/`. Nothing was written back into the analysed
program.

**The 88 KB private-IFD parser is not the CA evaluator.** The nine
sites in `FUN_1804f3ca0` that touch `obj + 0x129a8` are all the *same*
pattern of three helper calls, decompilable to (paraphrased, not
verbatim):

```
memset(&this->field_12a20, 0, 0x19c0);   // helper FUN_1804d1c30
memset(&this->field_129a8, 0, 0x1588);   // helper FUN_1804d1b90
release_local_shared_ptr(&local[+0x70], rsi);  // helper FUN_1804c2d70
```

`0x4d1b90` is a two-line `memset(dst, 0, 0x1588)` wrapper; `0x4d1c30`
the same for `0x19c0`; `0x4c2d70` a "if owner pointers differ, dispose
old" refcount helper. Every one of the nine hits is a *reset/failure*
path: the CA container is zeroed and any per-call scratch is released
before the enclosing tag branch returns an error code. The successful
tag-body-write path must therefore go through a helper that does not
take `0x129a8` as an immediate -- either through a virtual call, or
through a temporary buffer that some later routine copies into the
container. Either way, the polynomial evaluator is *not* here.

**The CA container is 0x1588 bytes, not 64.** Session 9's "tag 0x11b's
field lives at obj + 0x129a8" is right about the anchor but understates
the size. The class's non-CA constructor at `0x4f07a0` clears the
region with a `memset(param_1 + 0x129a8, 0, 0x1588)` (RVA
`0x4f07a0:0x1599` in the decompiler output, cross-referenced by the
constants in `step21_consumers.c`). Immediately adjacent members are
`memset`ed too, with sub-block sizes `0x19c0`, `0x1750`, `0x1758`,
`0xc40`, `0x2fa0` and double/int fields; the nested layout at least
holds a secondary object pointer at `+0x70` (i.e. `obj + 0x12a18`), a
double at `+0xF0` (`obj + 0x12a98`), further sub-blocks at
`+0xF8`/`+0x178`/`+0x200`/`+0x278`, and a series of `0x89abcdef`
sentinel values written into the destructor at `+0x1a1cc` and adjacent
offsets. The raw 32-word tag payload is one input among many; the
container also caches decoded coefficients and derived tables that a
subsequent render step consumes.

**SILKYPIX does not run Rigo's checksum.** This is the single hardest
new finding of the session. `FindConstXrefs.java` on `0xFFEF` (the
`csum mod 0xFFEF` modulus of Rigo's `parseca.c`) returned three
functions, all far from the Panasonic RTTI cluster:
`is_wide_character_specifier<wchar_t>` (locale code),
`FUN_18101e468`, `FUN_18102a52c`. None sits in or near the private-IFD
parser, the copy-assign, the destructor, or any consumer of
`obj + 0x129a8`. Scanning the DLL for the multiplier `73` (`0x49`) is
too noisy to isolate, but the modulus is a specific prime that would
have to appear as an immediate operand in the reduce step; it does
not. Either SILKYPIX skips the four-checksum validator entirely, or
it computes a different checksum against a different modulus. Rigo's
paper is careful to say his algorithm was derived by observation, not
by disassembly; this session establishes empirically that Panasonic's
own reference implementation does not use it.

**The Panasonic CA correction is a plugin, not a C++ class.** A class-
name string `IslEISDevelopDemosaicPanaCA` exists at RVA `0x13eeb18`,
with two siblings `IslEISDevelopDemosaicPanaLC` (`0x13edced`) and
`IslEISDevelopDemosaicPanaDH` (`0x13edd0d`) for lens correction and a
third Panasonic-specific pass whose meaning is not yet clear. The
string appears exactly once as an operand, in a name-lookup switch at
`FUN_180546c30` case `0x1216` (i.e. `0x1216` is the module id for
PanaCA, `0x1202` for PanaLC, `0x120c` for PanaDH). Crucially:

- No RTTI type descriptor `.?AVIslEISDevelopDemosaicPanaCA@@` exists
  in the DLL (`FindClassVtable.java` on that name returned "no TD").
  The three PanaXX demosaic classes therefore have no vtable; they
  are not part of the same C++ hierarchy as `IslZTiffExifPanasonic`,
  `IslEISPanaRemoveBlack`, `IslEImageServerInputPanaRAW` or the other
  seven `.?AV*Pana*@@` RTTI descriptors this session enumerated.
- The class id `0x1216` appears four times in the whole DLL, none in
  a data-section table paired with a function pointer, and the only
  scalar-operand hit is a false-positive `__LINE__` arg
  (`FUN_1805b95d0("...isleisfilternr3.cpp", 0x1216)` at
  `0x62f857`, line 4630 of the source file). There is no static
  factory table that maps `0x1216` to a constructor.

So `PanaCA` is a plugin registered by *name* at runtime through a
factory system this session did not lift. The tag-0x11b decoder and
polynomial evaluator live inside a factory-instantiated module whose
entry point is not statically reachable from class-hierarchy walks or
scalar-constant scans. Lifting them needs either a runtime hook (attach
a debugger, log the factory's argument on plugin registration), or a
full RE of the SILKYPIX plugin-manager, which is a separate project.

**Property-key evidence.** The pipeline manager holds a property
dictionary keyed by 16-bit ids (`0xa050 = "RawData"`,
`0xa051 = "Distortion"`, `0xa052 = "ColorAberration"`,
`0xa053 = "Shading"`, ...; the table at RVA `0x1a119c8..0x1a11a80`).
`0xa052` is referenced by exactly one function, `FUN_18074e0f0` at
`0x74f148`, which calls `FUN_1810ceb30(handle, key_obj, radius,
0xa052)` and then, on success, sets pipeline state to enable a CA
correction stage. This is the *dispatch* site -- the pipeline manager
looks up whether a `ColorAberration` property is available on the
current image and, if so, arranges for the CA pass. It is not the
evaluator, but it confirms two things: (a) SILKYPIX's CA correction is
a first-class pipeline stage with its own property key, gated by the
same handle-lookup mechanism as distortion, shading and RawData; and
(b) the actual polynomial code sits behind another indirection past
`FUN_1810ceb30`, which is where the trail runs into the plugin factory
described above.

**A debug format string that is not called.** `.rdata:0x1617dc0` holds
`"%d, ColorAbeHeight:%d, ColorAbeR:%d, ColorAbeB:%d, (H:%d, R:%d, B:%d),
R:%f, B:%f\n"`. The shape of this format is informative -- integer
`ColorAbeR`/`ColorAbeB` values distinct from float `R`/`B` values would
mean the algorithm keeps raw sensor-side integer coefficients separate
from computed floating-point displacements -- but the string has *no
code references* anywhere in the DLL. It was compiled in and never
called; probably a dead diagnostic path left over from an internal
SILKYPIX development build. Cannot be used as a landmark.

**Companion `.spd` / `.spx` containers stay opaque.** Confirms session 9.
`DefaultLensInfo.spd` (116 KB), `DefaultParameters.spd` (7.5 MB) and
their sibling `.spx` files all open with a 256-byte identifier area
(`"SILKYPIX"`, version tag `"2009042001"` in the `.spd`s), followed at
offset `0x100` by the magic `"ISL Multi purpose file format." 0x1a`
and a header table at `0x140`. The payload beginning near `0x150` has
the byte-frequency profile of encrypted or compressed data; the same
initial 16-byte payload run (`ad 88 22 81 d1 74 ea 89 52 6a c3 5d 50
a5 43 95`) appears in both `DefaultLensInfo.spd` and
`DefaultLensInfo.spx`, suggesting a shared framing key rather than
per-file randomness. Whether this contains a per-lens attenuation that
would explain session 7's K = 7-13 body spread is a real open
question, but a full ISL-container RE is out of scope here.

**Reconciliation with session 8.** Five questions from Task 2, and
what this session was able to say about each:

1. *Word indices*: not answered from SILKYPIX. The parser does not
   read individual word offsets as scalar operands (it copies the whole
   tag payload into the container via memcpy-shaped helpers whose
   source pointer is in a register), and the polynomial evaluator that
   would touch the 6 words was not lifted. Session 8's
   `[8, 10, 12, 20, 23, 27]` is neither confirmed nor refuted.
2. *Polynomial form*: not answered from SILKYPIX. Same reason.
3. *Normalisation constants*: consistent with session 9. Values
   `11.48`, `11.484`, `1/11.48` remain absent as literals; `0x8000` and
   `0xCCC` are too common to be diagnostic; the specific radius
   sequence `{0.333, 0.667, 0.833, 1.0}` is not stored as consecutive
   doubles. K is not one number in the DLL.
4. *B plane derivation*: not answered. The dead debug string treating
   R and B symmetrically ("ColorAbeR:%d, ColorAbeB:%d ... R:%f, B:%f")
   is suggestive of two independent per-channel polynomials rather
   than a differential encoding of B against R, but a dead string is
   not evidence.
5. *Sensor-format switch*: not answered from SILKYPIX. Session 9's
   negative finding on model-string branches holds. The
   `FUN_18074e0f0` dispatcher does not carry a sensor-format switch;
   any format-dependent behaviour would sit inside the
   factory-instantiated `PanaCA` module.

**What changed vs session 9:**

- Session 9 said "the exact `words[8], words[10], ..., words[27] x
  coefficients` sequence needs a decompiler." That was necessary but
  not sufficient. With Ghidra available, we now know the polynomial is
  not in the parser at all, and is dispatched through a plugin factory
  chain, not through a `.text` control flow that can be walked from
  the parser.
- Session 9 pointed at slot 3 (`0x48f750`) and slot 17 (`0x48fae0`) of
  `IslZTiffExifPanasonic`'s vtable as "plausibly the CA-apply entry
  points" based on size. This session downgrades that guess:
  `IslZTiffExifPanasonic` is the *TIFF/EXIF reader*, not the pixel
  processor. The CA-apply entry lives in a different, non-RTTI class,
  reached through `FUN_18074e0f0 -> FUN_1810ceb30(key=0xa052)`. Slot 3
  and slot 17 remain unverified, but are no longer the priority path.
- Session 9 assumed Rigo's checksum was the file's admissibility
  gate. This session refutes that at the SILKYPIX end: the modulus
  `0xFFEF` is not in the parser or any Panasonic-adjacent code.

**What tightened vs session 9:**

- The CA container is a 0x1588-byte structure with rich internal
  layout, not just the 64 tag payload bytes. Any future decoder must
  respect that raw 0x011b words are only one input.
- The Panasonic pipeline uses three distinct demosaic-time modules
  (`PanaLC`, `PanaDH`, `PanaCA`), gated by property ids `0xa051`
  (distortion) and `0xa052` (colour aberration) rather than by the raw
  tag ids `0x0119`/`0x011b`. That is the layer at which the darktable
  path would ideally hook, and it is consistent with our existing
  design of a distortion+CA branch behind a single "Panasonic embedded
  metadata" method.

**Recommendation for the darktable patch.** Ship session 8. The reasons
have not changed since session 9's writeup, and this session did not
recover a cleaner alternative from SILKYPIX. Additionally:

- Do not require Rigo's four-checksum pass to succeed before applying
  the CA correction. SILKYPIX plainly does not run that check, and
  files that fail Rigo's checksum in the wild (from firmware versions
  Rigo did not sample) will still be corrected by the camera vendor's
  own software. Downgrade the checksum to a *diagnostic log line* -- if
  it fails, log it, but still parse the payload.
- Do not try to encode the per-body K spread. Nothing in this session
  or session 9 says K lives in code as opposed to per-lens data in
  `DefaultLensInfo.spd`, and we still cannot read that file.
- Keep the door open to a session 11 that runs SILKYPIX under Frida or
  a similar dynamic tracer to log (a) the factory's plugin
  registration for the `PanaCA` module, (b) the exact bytes passed to
  its `Process` entry point on a known RW2 file, and (c) the argument
  and return of `FUN_1810ceb30(..., 0xa052)`. That is the shortest
  route from here to the polynomial. It requires a Windows or Wine
  host and was out of budget for this session.

**Honest bounds.**

- The polynomial evaluator was *not* extracted. Static class-hierarchy
  and scalar-scan techniques exhausted here; the next step is dynamic
  tracing or a plugin-factory RE, neither cheap.
- Rigo's checksum being absent from SILKYPIX is a strong empirical
  finding, but "absent from SILKYPIX" does not equal "not the right
  checksum for tag 0x011b". Rigo verified his checksum by reproducing
  it against dozens of files across bodies; the possibility that
  Panasonic's own converter simply *trusts* the tag while third-party
  code has to validate it against corruption is not excluded by this
  session's finding. The recommendation above (log-only diagnostic) is
  conservative on both readings.
- No sensor-format branch was located. Session 9's negative finding
  stands. If the DC-S5 result reported by the developer really does
  need different knot ratios than MFT bodies, the switch sits inside
  the plugin factory or the `.spd` payload.
- The 5512-byte CA container structure was partially mapped from the
  constructor at `0x4f07a0`; that map is enough to say the container
  is more than the raw payload, but not enough to identify which
  offsets store decoded coefficients versus derived tables. That
  requires the evaluator function, which was not lifted.

**Files this session:**

- `/tmp/rw2_tca/ghidra_scripts/`: eight `.java` scripts that drive
  `analyzeHeadless` for targeted decompile, xref search, and data
  dumps. Independent of the corpus; can be run against any DLL.
- `/tmp/rw2_tca/step21_consumers.c`: Ghidra pseudocode for the four
  small functions that reference `obj + 0x129a8`
  (`FUN_1804c9d00`, `FUN_1804cf610`, `FUN_1804f07a0`, `FUN_1805661f0`).
  Constructor and destructor pattern; useful for reconstructing the
  CA-container layout. Not the evaluator.
- `/tmp/rw2_tca/step21_helpers.c`: three-line pseudocode for the
  memset/refcount helpers the parser calls
  (`FUN_1804d1b90`, `FUN_1804d1c30`, `FUN_1804c2d70`).
- `/tmp/rw2_tca/step21_sites.txt` and
  `/tmp/rw2_tca/step21_wide.txt`: raw disassembly windows around the
  nine `obj + 0x129a8` touches inside the 88 KB parser. These are the
  reset paths.
- `/tmp/rw2_tca/step21_camain.c`: pseudocode for `FUN_180546c30`,
  the class-name lookup switch (this is where the string
  `IslEISDevelopDemosaicPanaCA` comes back at case `0x1216`).
- `/tmp/rw2_tca/step21_ca_dispatcher.c`: pseudocode for
  `FUN_18074e0f0`, the property-key dispatcher that calls into
  `FUN_1810ceb30(..., 0xa052)`. This is the trailhead for the
  plugin-factory RE mentioned above.
- `/tmp/rw2_tca/step21_islecls.txt`: `IslEIS*` string enumeration.
- `/tmp/rw2_tca/step21_strefs.txt`: reference sites for the CA-related
  strings. All the human-readable "Chromatic Aberration ..." strings
  are dead (zero code references); only `ColorAberration` and
  `IslEISDevelopDemosaicPanaCA` are alive, and only through the
  registries described above.
- Ghidra project: `/tmp/ghidra_proj/SilkyRE/`. Reusable by future
  sessions without re-running the 17-min analysis.

### SILKYPIX plugin walk (session 11)

Session 10 identified the property-key dispatcher (FUN_18074e0f0) and
the immediate resolver it calls (FUN_1810ceb30 at RVA 0x10ceb30), but
did not lift what happens past the resolver. This session did: the
resolver is not just a lookup, it is the CA descriptor builder. The
polynomial evaluator turns out not to be a polynomial. SILKYPIX
represents the correction as a 9-knot piecewise curve
(IslZCnvPolyLine) fetched from a per-image property blob and combined
with a per-lens integer offset table, then stored in the pipeline for
a later render pass to sample.

**Scripts used.** Reused four of session 10's eight scripts:
`FindConstXrefs.java`, `FindRefsToAddr.java`, `DecompileByRVA.java`,
`GrepStrings.java`. Added two small scripts under the same directory
`/tmp/rw2_tca/ghidra_scripts/`:

- `ReadBytes.java`: dumps a byte range at a given RVA as hex + ASCII,
  used to walk the property-id table and read the FP constants pool
- `Disasm.java`: dumps N x86 instructions starting at a given RVA, so
  the loop count and the `pdVar3[0x11] = pdVar3[0xd]` tail assignment
  could be cross-checked against the decompiler output

Outputs written under `/tmp/rw2_tca/step22_*.{c,txt,log}`; nothing was
written back into the analysed program.

**The CA builder, RVA 0x10ceb30 (FUN_1810ceb30).** Called once from
the dispatcher at RVA 0x74f15b with `param_4 = 0xa052`. Signature:

```
FUN_1810ceb30(handle, out_polyline, R_half_diag, prop_id)
```

At entry the caller has already computed
`R_half_diag = 0.5 * sqrt((x2-x1)^2 + (y2-y1)^2)` (RVA 0x74f012..0x74f036,
`FUN_18100fa34` is sqrt), so `param_3` is exactly half of the raw-image
diagonal in sensor pixels. On MFT bodies that lands near 3276; on
full-frame Panasonic near 2854; session 6's N1 numbers were the same
quantity by another route.

The builder does three things:

1. Pull property `0xa020` off the handle via `FUN_1810ce250`, storing
   its contents as an int32 array of at least 11 entries into
   `local_2a0`. `0xa020` is a per-lens/per-image adjustment slot; its
   first seven int32s are consumed here.
2. Pull the requested property (`0xa052` when the dispatcher is
   asking for CA) off the handle as a raw int16 array. This uses a
   virtual call at vtable slot 0x990 on the handle, followed by
   `FUN_180395150` which does a plain length-prefixed memcpy into a
   local buffer. The check that gates the whole builder is
   `len_shorts > 14 && payload[0] == 7`: at least 15 shorts, and the
   first one is a fixed discriminator value 7. If either fails, the
   builder returns 0xffff without touching the output polyline.
3. Allocate a 9-entry `IslZBuffer<IslZVector2>` off `param_2` (each
   Vector2 is two doubles, so 18 doubles total) and populate it:

```
knot[0]     = (0.0, 1.0)
for i in 0..6:
    knot[i+1].x = (payload[i+1] + offsets[i]) / R_half_diag
    knot[i+1].y = (payload[i+8] - offsets[i]) / 1000.0
knot[8].x   = 2.0
knot[8].y   = knot[6].y       # tail-clamp uses knot 6, not knot 7
```

The knot-8 tail assignment is verified from disassembly (RVA
0x10ced5d..0x10ced68): the loaded quadword is `[R10 + 0x68]`, which
is offset 104 = 13\*8 bytes from the buffer base, i.e. knot 6's y
component, not knot 7's at offset 120. Whether this is intentional
(reserve the last data knot as extrapolation guard) or a compiler
artifact from the original source is not resolvable without the
source. Either way, it is what SILKYPIX runs, so any port to
darktable would want to match it.

The completed 9-knot vector is handed to `param_2` via a virtual call
at slot 0x18 (`(*out_polyline->vtbl[3])(out_polyline, knots, 9)`),
which is the `IslZCnvPolyLine::SetKnots` entry.

**Word indices, cited.** Payload shorts consumed by the builder,
indexed from 0:

```
payload[0]     signature/discriminator, must be 7
payload[1..7]  seven radius shorts,  contribute to knot[1..7].x
payload[8..14] seven value  shorts,  contribute to knot[1..7].y
```

Byte offsets of the value shorts are 0x10..0x1c on the payload
pointer, verified from the address form
`word ptr [RAX + RCX*0x2 + 0x10]` at RVA 0x10ced1b. Radius shorts sit
at byte offsets 0x02..0x0e. Nothing past `payload[14]` is read here.
The seven int32 offsets from property `0xa020` are consumed one per
knot at byte offsets 0..0x18 of that separate buffer, verified from
`R11` starting at 0 with a `LEA R11, [R11 + 0x4]` per iteration
(0x10ced48).

**Radius units.** The knot x-coordinates come out as
`sensor_pixel / half_diagonal_pixel`, so a knot at raw radius equal to
the half-diagonal lands at x = 1.0. The polyline is defined on
approximately [0, 2] with hard endpoints at x = 0 (y = 1) and x = 2
(y = knot[6].y). The image corner sits at x = 1 (radius = half
diagonal). Everything from the corner to x = 2 is extrapolation
padding, not addressable by a real pixel.

**Value units.** The knot y-coordinates are `(payload_short - offset) /
1000.0`. The offsets from `0xa020` shift the raw shorts before the
divide. The `1000.0` divisor is the double at RVA 0x1354ff0 (verified
by reading the eight bytes: `00 00 00 00 00 40 8f 40`, which decodes
to 1000.0 as IEEE-754). Once the polyline is applied per pixel, its
sampled value ends up on the order of 0.01--0.1 for real CA magnitudes,
which is the same order as session 5's `C_R @ words_R * poly4(r_norm)`
after the session-8 K = 11.48 divide.

**R vs B is not split at the builder.** The dispatcher calls the
builder three times with adjacent property ids -- `0xa052` for CA,
`0xa053` and `0xa054` for the two shading passes -- and stores each
resulting polyline in a slot inside `local_2d28` at offsets 0x08,
0x778, 0xee8. There is no second CA call, no second CA property, and
no CA-specific fan-out in the dispatcher. The single 9-knot polyline
built from `payload[1..14]` is the *entire* CA descriptor delivered
to the render side. The R and B split therefore has to happen later,
when the render step samples the polyline; the split is not visible
in the property-side code walked here. The (dead) debug format at
RVA 0x1617dc0 -- `"...ColorAbeR:%d, ColorAbeB:%d, ... R:%f, B:%f"` --
suggests the render side does keep two per-channel scalars (integer
and float), but the code that produces them was not located: it lives
inside one of the `IslEISDevelopDemosaicPanaCA` plugin methods, which
are still not statically reachable from name-lookup alone (the plugin
has no RTTI, and no scalar table pairs `0x1216` with a factory pointer,
as session 10 established).

**Property-id table.** The pipeline handle stores properties keyed by
16-bit ids. The tag payload for `0x011b` reaches the CA builder as
`0xa052`. The mapping between raw RW2 tag bytes and this property blob
happens inside the 88 KB private-IFD parser (`FUN_1804f3ca0`, session
10), on a copy path the decompiler declined to lift within its
timeout. What is clear from the layout the builder sees is: the raw
tag ends up materialised as a `short[]` where element 0 is the fixed
value 7 and elements 1..14 are the useful data; on a 32-word Panasonic
`0x011b` payload, this could be:

- `payload = raw_tag_shorts[0..14]` (leading window), or
- `payload = raw_tag_shorts[k..k+14]` for some k > 0, or
- a permuted/decoded subset of the 32-word tag

The builder itself cannot distinguish these; the mapping is in the
parser. What the builder *does* imply is that only 15 shorts of the
32-word tag drive the CA polyline, plus 7 int32s from `0xa020`. The
remaining 17 shorts either feed distortion (`0xa051`) and shading
(`0xa053`/`0xa054`) via the same builder pattern, or are unused, or
carry the checksum -- session 10's negative finding on Rigo's
`0xFFEF` modulus rules the last option out for this specific
checksum, but not for a different one.

**Reconciliation with session 8.**

- *Does the six-word set from session 8 match SILKYPIX's word usage?*
  Not directly. Session 8 uses `payload[8, 10, 12, 20, 23, 27]` on
  the assumption that CA needed six regressors. SILKYPIX's builder
  reads `payload[1..14]` -- fourteen shorts, all of them, no skipping
  and no words past index 14. Words 8, 10 and 12 overlap (SILKYPIX
  reads them as knot values 1, 3 and 5), so session 8 was partly
  right about *which* words carry CA information for the value axis.
  Words 20, 23 and 27 are *not* read by the builder. Either the
  parser rewrites the raw tag into a compact 15-short property in
  which words 20/23/27 of the raw tag become words 4/5/6 of the
  property blob, or session 8's fit against JPEG target simply
  latched onto whichever words happened to correlate with the
  measurable pixel shift. The current investigation cannot decide
  between these without lifting the parser.
- *Does the polynomial form match?* No. SILKYPIX's CA descriptor is a
  piecewise curve with seven interior knots plus two boundary knots,
  not a quartic polynomial. A quartic can approximate the polyline
  reasonably well over the [0, 1] domain (that is what session 8's
  R^2 = 0.588 on R at K = 11.48 is measuring), but the two are not
  the same object. In particular, SILKYPIX's representation has
  seven independent per-knot offsets from `0xa020` per lens, which a
  four-coefficient polynomial simply cannot express without absorbing
  them into higher-order terms, which is where session 8's LOGO R^2
  went negative.
- *Is K a real code constant or a numerical coincidence?* Neither
  purely. The literal 1000.0 is the value-axis divisor in the code
  (RVA 0x1354ff0). Session 7's median K = 11.48 does not appear in
  the code as a constant. But there is a plausible mechanistic
  reading: SILKYPIX's polyline y-values are on the order of
  `(short - offset) / 1000`, so around 0.03--0.1 for realistic CA;
  session 8's regression against pixel-space JPEG-measured shifts
  landed on scale coefficients whose median ratio to the raw session
  5 poly output happened to be 11.48. In other words, K = 11.48 is
  the specific numeric factor that converts session 5's polynomial
  in un-normalised units to the polyline-value-times-pixel-radius
  order of magnitude. It is not a code constant, but it is not
  unrelated to a code constant either.
- *Ship session 8 as is, tweaked, or replaced?* Ship session 8 as
  the shipping recommendation. This session found SILKYPIX's exact
  representation but did not lift the property blob's provenance,
  did not lift the R/B split at the render side, and did not lift
  the plugin's polyline-application code, so we cannot yet emit
  darktable coefficients that would parity-match SILKYPIX. Session
  8's model, with its 96% R sign-match and 0.028 px median RMS
  against JPEG-measured CA, remains the best defensible port until
  those three pieces are lifted. The right follow-up is not to
  refit; it is to (a) decode the parser's short-to-property mapping,
  (b) decode the polyline-application code inside PanaCA, and (c)
  re-express the darktable path as a 9-knot curve evaluator rather
  than a quartic polynomial.

**Recommendation for the darktable patch.** Two changes to what
session 9 and session 10 already recommended:

1. Keep session 8's polynomial as the shipping decoder for now, for
   the reasons above. It is the closest darktable-idiomatic form we
   can build without the two missing pieces (parser mapping and
   R/B application).
2. When the parser mapping and the R/B application are lifted, plan
   to switch the darktable path from `poly4(C_R @ words_R, r_norm) / K`
   to a monotone spline evaluator on the seven knots, with radius
   normalised to half the raw-image diagonal, and value quantum of
   0.001. That is the change that will match SILKYPIX bit-for-bit
   modulo camera JPEG's own sharpening bias. Until then, ship
   session 8 and log the checksum failure but do not gate on it.

**Honest bounds on what remained undecoded.**

- The mapping from raw `0x011b` bytes to the property `0xa052` blob is
  still inside `FUN_1804f3ca0`'s 58 KB body. If word[8]/word[10]/word[12]
  of the raw tag equal payload[8]/payload[10]/payload[12] of the
  property blob -- which is the simplest possible mapping -- then
  session 8's overlap words align exactly and SILKYPIX effectively
  confirms session 8's decode on those three words. If the parser
  permutes, we do not yet know which raw words end up where.
- The polyline-application code inside the PanaCA plugin is not
  lifted. R vs B differentiation therefore stays unresolved. The
  simplest hypothesis consistent with the (dead) debug format
  string is that the render step samples the same polyline twice
  with opposite signs; the second-simplest is that it samples
  once, negates for B, and adds a per-channel scale from a lens
  table; the third is that there is only R correction and B stays
  put. This session cannot distinguish these.
- The `0xa020` int32 offset table's origin is likewise not lifted.
  Its 7 int32 values shift both the radius axis (added to the short
  before divide by R_half_diag) and the value axis (subtracted
  before divide by 1000). Whether it is a per-lens correction or a
  per-body correction is not resolvable from the builder alone;
  finding its setter is a follow-up.
- Sensor-format switch: still not found. Nothing in the builder
  branches on body id or format. If MFT vs full-frame requires a
  different curve, the difference is expressed through the property
  payload itself (different words) or through the `0xa020` offsets,
  not through a code branch.
- The plugin factory that maps `0x1216 -> PanaCA` is still not
  lifted. Session 10 established there is no static factory table
  pairing the two; this session did not attempt a plugin-manager
  walk, and the property-key dispatcher is not the factory. Getting
  from `0x1216` to the polyline-application code still needs either
  a dynamic tracer or a full plugin-registration RE. Both remain
  out of scope for a static session.

**Files this session:**

- `/tmp/rw2_tca/ghidra_scripts/ReadBytes.java`,
  `/tmp/rw2_tca/ghidra_scripts/Disasm.java`: two new scripts, joined
  to session 10's eight.
- `/tmp/rw2_tca/step22_1810ceb30.c`: Ghidra pseudocode for the CA
  builder at RVA 0x10ceb30. This is the algorithm above.
- `/tmp/rw2_tca/step22_helpers.c`: pseudocode for `FUN_1810ce250`
  (fetches the `0xa020` int32 offset table) and `FUN_180395150`
  (copies the property short-array payload into a local buffer).
- `/tmp/rw2_tca/step22_10cdd70.c`: pseudocode for `FUN_1810cdd70`, the
  neighbouring int32-mantissa-plus-exponent property fetcher used
  by the dispatcher for `0xa050`, `0xa055`, `0xa056`, `0xa057`. Not
  the CA builder, but confirms the property-lookup pattern.
- `/tmp/rw2_tca/step22_polymath.c`: pseudocode for `FUN_180333090`
  (`IslZCnvPolyLine`'s three-argument combiner) and `FUN_180333880`
  (its single-point sampler). Consumer side, not producer side.
- `/tmp/rw2_tca/step22_332f90.c`: pseudocode for `FUN_180332f90`,
  the polyline copy/assign that hands the completed CA curve into
  the pipeline slot at `local_2da0`.
- `/tmp/rw2_tca/step22_proptable.txt`: raw dump of the property-id
  table at RVA 0x1a11800..0x1a11c00. Session 10's mapping of
  `0xa050 -> RawData`, `0xa051 -> Distortion`, `0xa052 ->
  ColorAberration` is what the code uses; the raw pointer-triples
  in this dump can be re-read for a strict property-id-to-name
  audit if a future session cares. The code passes numeric ids, not
  strings, so the mapping is a diagnostic aid rather than a
  correctness lever.
- `/tmp/rw2_tca/step22_propnames.txt`: raw dump of the property-name
  string pool at RVA 0x1617700..0x1617b00, cross-referenced by the
  table above.
- `/tmp/rw2_tca/step22_ceb30_disasm.txt`: x86 disassembly of the CA
  builder's loop body (RVA 0x10cec96..0x10ced82), used to verify
  the loop iteration count (7), the word-index bases (0x02 for
  radii, 0x10 for values), and the tail assignment
  `[R10 + 0x88] <- [R10 + 0x68]` = knot[8].y <- knot[6].y.
- `/tmp/rw2_tca/step22_const.txt`: the 8-byte double at RVA
  0x1354ff0, decoded as 1000.0. This is the value-axis divisor
  used at RVA 0x10ced33.

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

Full G9 corpus (nine files at f/5.6, tripod-free but static scene):
`/c/temp/tca/P136647{7,8,9}.RW2` (PL 12-60 at 12/25/60mm),
`P136648{1,2,3}.RW2` (L 45-150 at 45/97/150mm), `P1366484.RW2` (Lumix
42.5), `P1366485.RW2` (Sigma 30), `P1366486.RW2` (Sigma 16), plus
paired JPEG and 8-bit SILKYPIX SE TIFF for each.

Cross-body corpus on Panasonic DMC-GX80, 4592x3448, mirroring the G9
lens set at the same nominal focal lengths and f/5.6:
`/c/temp/tca/P126063{3..8}.RW2` (PL 12-60, L 45-150 zoom sweeps),
`P1260639.RW2` Sigma 16, `P1260640.RW2` Sigma 30, `P1260641.RW2` Lumix
42.5. GX80 N1..N4 = 2888, 2407, 1926, 963.

Do not check any of these files into the repo. Use them locally for the
RE work and cite them in reports without committing binary blobs.

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
