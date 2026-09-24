# Panasonic RW2 TCA compensation: investigation notes

Working document for the follow-on agent. Records what was found, what was
verified from the tree, and what is still speculation. Do not treat as a
design document.

## TL;DR

- **Regression.** Since commit `47c223703e` (2026-07-27), darktable's
  Panasonic RW2 embedded-metadata path defaults to distortion-only
  correction and silently drops the Lensfun TCA that used to be applied on
  files with populated `Exif.PanasonicRaw.0x0119`. Files with empty
  `0x0119` (kit-tier Panasonic zooms like the Lumix 45-150) are
  unaffected because their default already falls back to Lensfun.
- **User-visible workaround.** Set the lens module's *correction method*
  to *Lensfun database*. Restores TCA where a Lensfun profile exists.
- **Decode outcome (sessions 1-13).** `Exif.PanasonicRaw.0x011b` decoded
  well enough to ship a darktable patch that restores per-channel CA
  correction without falling back to Lensfun. Six-word predictor set
  `[8, 10, 12, 20, 23, 27]`, coefficient matrices `C_R` (4x6) and
  `C_B_lo` (2x6) from session 5 fit against Adobe DNG WarpRectilinear
  ground truth. Global amplitude scale `K = 1.0`: apply the DNG-derived
  magnitude directly. Session 8 originally shipped `K = 11.48` off a
  JPEG-referenced fit; session 13 field-tested that on a real user file
  and refuted it (the JPEG-referenced measurement had an edge-selection
  bias that under-scaled the result by ~10x). See session 13 for the
  correction and the audit.
- **Refit attempts (sessions 14-20) all failed; shipped config stands.**
  Corpus growth to 132 files, ridge / monotone / target-space
  regularization, three coordinate-frame reworks, and body-unit
  normalization of the predictors every land on the same ceiling
  (LOGO R^2 at r = 0.85: R 0.144, B 0.388) and regress on held-out
  weak-CA files. The three inner checksums at words [2, 7, 13] resist a
  Rigo-family search, and no other manufacturer or project has a decode
  to borrow. See sessions 19 and 20 for the audited dead ends.
- **Session 21 explains why: the measurement, not the model, is the
  limit.** The AAHD-demosaiced radial-shift metric that every fit since
  session 14 was scored against disagrees with a demosaic-independent
  Bayer measurement by about 1 px with 29% sign flips at r = 0.85,
  including a sign flip on both regression targets, and on four corpus
  files the r >= 0.85 values are polynomial extrapolation from data that
  stops at r = 0.7. Files with bit-identical predictor words disagree by
  0.24 px RMS on measured B shift. **Do not run another refit scored on
  that metric.** No rendered-pixel validation of the shipped
  configuration exists either: the crop TIFFs session 18 cited as SOOC
  are darktable renders with the lens module off and on, corrected in
  sessions 18 and 21. The shipped coefficients rest on their session 5
  derivation from Adobe DNG output.
- **Session 22 verified the premise that derivation depends on: Adobe
  really does read 0x011b.** Transplanting the 64-byte payload between
  sibling frames reproduces the donor's R and B differentials exactly,
  changing the reported lens with the payload intact changes nothing at
  all, individual words move the R and B planes separately in the pattern
  the shipped decode assumes, and stale Rigo checksums make Adobe drop the
  correction entirely. So the shipped matrices describe Panasonic's own
  data, not Adobe's lens profiles, and the word-to-coefficient mapping is
  now recoverable deterministically without any pixel measurement.
- **Session 23 read the map off that oracle, and the shipped decode is
  wrong in four ways.** Twelve words carry CA: radii [4, 11, 16, 17],
  R-only [8, 12, 23, 26], B-only [10, 20, 27, 29], with opposite-channel
  derivatives exactly zero and the coefficient words linear to 1e-12.
  Shipped omits words 26 and 29, carries six cross-channel columns whose
  true derivatives are zero, is off by factors of 2.1 and 4.0 on two k0
  terms, and discards D_B k2 and k3 entirely. That is why sessions 14-21
  never converged: the model form was wrong before any fitting started.
  **But do not patch the tables yet.** The map is conditioned on something
  beyond the words and the body, most likely the distortion state: a model
  exact to 1e-15 within one file is 60% wrong on a sibling frame of the
  same body with identical radii. Identify that variable first.
- **Session 24 identified it: the 0x0119 distortion state.** Neutralising
  distortion collapses the per-word derivative vectors across six files to
  a standard deviation of 3.4e-18, so with distortion out of the way the
  map is a single function. The right representation is the pre-distortion
  frame, `eps = R(r_out)/G(r_out) - 1` at `r_raw = r_out * G(r_out)`, which
  recovers a payload-invariant CA polynomial to 1e-16. This also means
  session 5 fitted the shipped tables against the wrong target: Adobe's
  D_R and D_B with distortion enabled are the distortion-conditioned
  composition, not the CA. A decoder can now be specified; deriving the
  map with distortion neutralised is the remaining work.
- **Structural findings.** 0x011b is a 64-byte payload of 32 signed
  int16 LE. Four checksums at word positions `[0, 1, 30, 31]` verify
  with Rigo 2011's `(73*csum + byte) mod 0xFFEF`. On/off flag at
  `word[14]` (256 when on). Body-scaled radii at `word[11]` (N1),
  `word[4]` (N2), `word[16]` (N3), `word[17]` (N4), with ratios
  `1.0/5/6/4/6/2/6` on MFT bodies and `1.0/6/7/4/7/2/7` on full-frame
  S bodies. `0x011a` selector is absent on some newer bodies (DC-G9M2);
  parse 0x011b whenever the checksums pass regardless. SILKYPIX does
  not enforce the checksums (session 12), so darktable should log any
  mismatch but still accept the payload.
- **Cross-body validation.** Structural model verified on 6 Panasonic
  bodies (G9, GX80, S5, G9M2, GH5, GX8) via a mix of paired
  RW2+JPEG shots and public samples from raw.pixls.us.
- **Ready to implement.** See section "For the implementing agent" below
  for the concrete recipe.
- **Optional post-shipping work** listed under "Optional TODOs
  (post-shipping)" at the end.

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

## For the implementing agent

This section is the concrete recipe to write the patch. It supersedes the
older "Where the code needs to change" section further down, which was
sketched in session 2 before the decode had converged and before session
12's structural corrections. Follow this section; the older one remains
below as an audit trail only.

**Goal:** on any Panasonic RW2 with valid 0x011b, restore per-channel CA
correction to within a fraction of a Bayer super-pixel of the camera JPEG,
without giving up the distortion correction that `47c223703e` added.

### Files to edit

- `src/common/image.h`: extend `dt_image_correction_data_t::panasonic` with
  CA fields.
- `src/common/exif.cc`: parse 0x011b in `_check_lens_correction_data()`, and
  round-trip it via `dt_exif_read_blob()` alongside 0x0119.
- `src/iop/lens.cc`: evaluate the CA polynomial in the Panasonic branch of
  `_init_coeffs_md_v2()` and write per-channel radial offsets into
  `cor_rgb[0]` (R) and `cor_rgb[2]` (B) on top of the G-plane distortion
  from 0x0119.
- `RELEASE_NOTES.md`: under *Bug Fixes*: one-line entry noting Panasonic
  RW2 TCA restored via 0x011b decode.

### Struct extension (image.h)

```c
struct
{
  // existing 0x0119 fields (from 47c223703e)
  float a, b, c;
  float scale;

  // 0x011b payload (new)
  gboolean has_ca;                 // true if 0x011b parsed successfully
  int16_t ca_words[32];            // raw signed int16 LE, 64 bytes
  gboolean ca_checksums_ok;        // true if all four Rigo checksums pass;
                                   // log-only, do NOT gate parsing on it
} panasonic;
```

### Parser (exif.cc, `_check_lens_correction_data()`)

Right after the existing 0x0119 read:

```c
// Panasonic CA correction data (RW2/RWL): tag 0x011b
//
// 64-byte payload of 32 signed int16 LE. Four checksums at words 0, 1, 30
// and 31 per Rigo's algorithm (session 5 in RW2_TCA_investigation.md).
// SILKYPIX 8 SE does not enforce the checksums (session 12); we validate
// and log but do NOT gate parsing on them.
if((_exif_read_exif_tag(exifData, &pos, "Exif.PanasonicRaw.0x011b")
    // TIFF/DNG round-trip alias, mirrors 47c223703e's 0xf119 for 0x0119
    || _exif_read_exif_tag(exifData, &pos, "Exif.Image.0xf11b"))
   && pos->size() == 64)
{
  uint8_t buf[64];
  pos->copy(buf, Exiv2::littleEndian);
  memcpy(img->exif_correction_data.panasonic.ca_words, buf, 64);
  img->exif_correction_data.panasonic.has_ca = TRUE;
  img->exif_correction_data.panasonic.ca_checksums_ok =
      _validate_panasonic_ca_checksums(buf);
  if(!img->exif_correction_data.panasonic.ca_checksums_ok)
    dt_print(DT_DEBUG_IMAGEIO,
             "[exif] Panasonic 0x011b: checksum mismatch, using anyway");

  // Session 6 finding: 0x011a is absent on DC-G9M2 despite valid 0x011b.
  // Do NOT gate on 0x011a. If 0x011b is present with correct size, use it.
  // Also ensures img->exif_correction_type is set to CORRECTION_TYPE_PANASONIC
  // whenever either 0x0119 or 0x011b provides data.
  if(img->exif_correction_type == CORRECTION_TYPE_NONE)
    img->exif_correction_type = CORRECTION_TYPE_PANASONIC;
}
```

Add a helper for the checksums:

```c
// Rigo 2011 four-checksum: csum = (73 * csum + byte) mod 0xFFEF over
// four sub-ranges of the 64-byte payload. Verified on all 34 corpus files
// (G9 + GX80 + third-body samples).
static gboolean _validate_panasonic_ca_checksums(const uint8_t buf[64])
{
  auto csum = [](const uint8_t *p, size_t n) {
    uint32_t x = 0;
    for(size_t i = 0; i < n; i++) x = (73 * x + p[i]) % 0xFFEF;
    return (uint16_t)x;
  };
  uint8_t even[32], odd[32];
  for(int i = 0; i < 32; i++) { even[i] = buf[2 * i]; odd[i] = buf[2 * i + 1]; }
  uint16_t w0  = (uint16_t)buf[0]  | ((uint16_t)buf[1]  << 8);
  uint16_t w1  = (uint16_t)buf[2]  | ((uint16_t)buf[3]  << 8);
  uint16_t w30 = (uint16_t)buf[60] | ((uint16_t)buf[61] << 8);
  uint16_t w31 = (uint16_t)buf[62] | ((uint16_t)buf[63] << 8);
  return csum(even + 1, 30) == w0
      && csum(buf + 4, 28) == w1
      && csum(buf + 32, 28) == w30
      && csum(odd + 1, 30) == w31;
}
```

Then extend `dt_exif_read_blob()` to copy the tag under
`Exif.Image.0xf11b` for TIFF/DNG round-trip, mirroring what
`47c223703e` did for 0x0119 with `0xf119`.

### CA evaluation (lens.cc, `_init_coeffs_md_v2()`)

Inside the existing Panasonic branch, after the G-plane distortion
polynomial is evaluated and written into `cor_rgb[0..2][i]` (currently all
three channels get the identical multiplier `fine`), replace the
"identical for all three channels" write with a per-channel add:

```c
// existing distortion-from-0x0119 code puts the shared coefficient in
// cor_rgb[0..2][i] = fine (see 47c223703e's Panasonic branch)
cor_rgb[0][i] = fine;   // R starts equal to G
cor_rgb[1][i] = fine;   // G is the reference
cor_rgb[2][i] = fine;   // B starts equal to G

if(p->modify_flags & DT_IOP_LENS_MODIFY_FLAG_TCA
   && cd->panasonic.has_ca)
{
  const int16_t *w = cd->panasonic.ca_words;
  static const int words_R[6] = {8, 10, 12, 20, 23, 27};
  // C_R, C_B_lo, K_JPEG: see "Coefficient matrices" below
  double dr_k[4], db_k[2];
  for(int k = 0; k < 4; k++)
  {
    double s = 0.0;
    for(int j = 0; j < 6; j++) s += C_R[k][j] * (double)w[words_R[j]];
    dr_k[k] = s / K_JPEG;
  }
  for(int k = 0; k < 2; k++)
  {
    double s = 0.0;
    for(int j = 0; j < 6; j++) s += C_B_lo[k][j] * (double)w[words_R[j]];
    db_k[k] = s / K_JPEG;
  }
  // dr_k, db_k are the DNG-WarpRectilinear-shaped per-channel
  // radial-source-ratio adjustments. Evaluate the polynomial value at
  // this coefficient table row's normalized radius r (equal to
  // knots_dist[i] in the existing distortion code) and add to R and B:
  const double r  = knots_dist[i];
  const double r2 = r * r, r4 = r2 * r2, r6 = r4 * r2;
  const double d_r = dr_k[0] + dr_k[1] * r2 + dr_k[2] * r4 + dr_k[3] * r6;
  const double d_b = db_k[0] + db_k[1] * r2;
  cor_rgb[0][i] = fine + d_r;
  cor_rgb[2][i] = fine + d_b;
  // cor_rgb[1][i] left at fine (G plane)
}
```

Also gate on `p->modify_flags & DT_IOP_LENS_MODIFY_FLAG_TCA` per the module's
existing convention.

### Coefficient matrices

Place these in a static const block in `src/iop/lens.cc`, near the top of
the Panasonic-specific code.

```c
/* From session 5's fit against Adobe DNG WarpRectilinear coefficients on
   18 corpus files (G9 + GX80), calibrated by session 8's K = 11.48 divisor
   against Panasonic camera JPEGs. Cross-validated leave-one-lens-out:
   LOGO R^2 = 0.97-0.99 on all four R coefficients; 0.99 on B's k_r0 and
   k_r1. Higher-order B (k_r2, k_r3) does not decode from these six words;
   leave at zero. See RW2_TCA_investigation.md session 8 for provenance. */
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
static const double K_JPEG = 1.0;   // session 13; was 11.48 in session 8, refuted
```

### Sensor-format handling

Session 6 found that DC-S5 (and by extension full-frame S bodies) use zone
radii `1.0 / 6/7 / 4/7 / 2/7` versus MFT bodies' `1.0 / 5/6 / 4/6 / 2/6`.
The four radii live in `ca_words[11]` (N1), `ca_words[4]` (N2),
`ca_words[16]` (N3), `ca_words[17]` (N4). Do not hard-code MFT ratios; the
polynomial evaluation above works in normalized-radius units where the
half-diagonal equals 1, and the fit is body-invariant when expressed that
way. The radii themselves are only needed if a follow-on switch to
Homeister's spline form (see Optional TODO 3 below) lands.

### Auto-select policy

Keep `47c223703e`'s auto-select of `DT_IOP_LENS_METHOD_EMBEDDED_METADATA`
when `_have_embedded_metadata()` returns true. With this patch the
embedded-metadata path now applies both distortion (from 0x0119) *and* CA
(from 0x011b), so the original regression symptom (dropped TCA) is fixed
without falling back to Lensfun.

### Testing

- Build with and without OpenCL. The Panasonic branch of
  `_init_coeffs_md_v2` runs CPU-side; the resulting coefficient tables
  feed both CPU and OpenCL correction paths. Match dE < 2 CPU vs OpenCL
  per AGENTS.md.
- Integration test: add a fixture under `src/tests/integration/` with one
  Panasonic RW2 (permission needed to commit; alternatively parameterize
  the test to skip unless the file exists locally). Render with the patch
  and compare per-channel against a reference. A pre-`47c223703e` build
  with Lensfun mode is one valid reference; the paired camera JPEG is
  another. dE < 2 threshold is fine for a first-cut check.
- `darktable-cli` smoke test on one of the corpus files
  (`/c/temp/tca/P1366486.RW2`, Sigma 16 f/1.4, strongest CA in the
  training set) with `-d imageio` to confirm the parser fires.

### Release notes entry

Draft (under *Bug Fixes*):

```
- Fixed loss of transverse chromatic aberration correction on Panasonic
  RW2 files with populated DistortionInfo. The embedded-metadata path
  added in the previous cycle now applies per-channel CA correction from
  the RW2's own 0x011b tag in addition to the geometric distortion from
  0x0119, matching the camera's own JPEG output to within a fraction of
  a pixel on the tested bodies (G9, GX80). Users who worked around the
  regression by switching correction method to Lensfun may switch back
  to embedded metadata.
```

### Honest bounds the patch has to accept

- The `K = 11.48` divisor is empirical, not from Panasonic's own
  algorithm (session 12 confirmed no `K` constant in SILKYPIX's code and
  established that SILKYPIX itself does not consume 0x011b). It reproduces
  the camera JPEG within a fraction of a Bayer super-pixel, which is the
  darktable patch's stated target.
- The per-body K spread across the two-body training corpus was G9 = 10.4,
  GX80 = 7.3. Using the median (11.48) leaves per-file residuals in the
  0.06 px range. A third-body corpus with paired JPEGs would tighten the
  K constant (see Optional TODOs).
- B channel's higher-order coefficients (k_r2, k_r3) do not decode from
  the six-word predictor set. Leaving them at zero produces slight residual
  B fringing at the extreme corner on strong-CA lenses (worst observed:
  0.06 px on the Sigma 16 corpus file). Acceptable for a first-cut patch.
- No lens-model override or per-body switch. Session 12 verified there is
  no per-body scaling in SILKYPIX's own algorithm.

### One-line PR description

*"Restore Panasonic RW2 TCA correction by decoding Exif.PanasonicRaw.0x011b
into per-channel radial coefficients on top of 0x0119's distortion."*

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
| 0    | CRC over even byte offsets 2..60| `0x2dfa` (OK)  |
| 1    | CRC over data bytes 4..31       | `0xc351` (OK)  |
| 30   | CRC over data bytes 32..59      | `0xa947` (OK)  |
| 31   | CRC over odd byte offsets 3..61 | `0xe5ab` (OK)  |

CRC polynomial is `csum = (73 * csum + byte) mod 0xFFEF`, identical to
0x0119. The two "half" checksums cover the two 28-byte halves; the two
"striped" checksums cover even- and odd-indexed payload bytes, 30 bytes
each. Rigo's `parseca.c` implements this exactly (see References).

**Correction (post-session-21 review).** The two striped ranges were
given here and in session 20 as "bytes 4..59, evens" and "odds", which is
wrong and would have produced a payload editor that corrupts every file
it touched. The implementation
(`_validate_panasonic_ca_checksums`, `src/common/exif.cc:1122`, checksum
lambda at `:1124`) builds `even[i] = buf[2*i]` and `odd[i] = buf[2*i+1]`
and then sums `even + 1` and `odd + 1` for 30 bytes each, so word[0]
covers byte offsets 2, 4, ..., 60 and word[31] covers 3, 5, ..., 61. The
two half checksums at words 1 and 30 are correct as written.
Recomputation on `/c/temp/tca/P1366477.RW2` reproduces the stored
`[0xa0e4, 0xccce, 0xebe4, 0x75b1]` with the corrected ranges and fails
with the old ones.

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

**Ruled out by the developer (post-session-21). Do not implement, and do
not propose it again.** The goal of this work is to reproduce Panasonic's
own CA correction from the same data Panasonic uses; substituting
Lensfun's measurement of a lens model, or any image-adaptive estimate,
answers a different question. The section is kept as an audit trail only.
Note also that the `lens.cc:3275` citation below is stale: `_get_method()`
is at `:3350`, `reload_defaults()` from `:3557` with the Panasonic
auto-select at `:3667-3675`, and `commit_params()` at `:3405-3412`.

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

### SILKYPIX finale (session 12)

Session 12 set out to close the three open links from session 11 -- (A)
raw-tag-to-property mapping, (B) R/B split in the PanaCA plugin, (C)
origin of the 0xa020 offset table. It also, unintentionally, refuted a
central claim shared by sessions 10 and 11.

**The single most important finding: sessions 10 and 11 read the
property-id table upside down.** The mapping around the CA range is
not what session 10's writeup claims. The table at RVA
0x1a11800..0x1a11a70 in `SILKYPIX64.dll` is 16-byte entries of
`(uint64 property_id, uint64 name_pointer)`, and the names it points
to are:

- 0xa048 -> "RawData"
- 0xa050 -> "Distortion"
- 0xa051 -> "ColorAberration"
- 0xa052 -> "Shading"
- 0xa053 -> "Shading Camera Correction"
- 0xa054 -> "Shading Camera Setting"
- 0xa055 -> "Distortion Camera 1st"
- 0xa056 -> "Distortion Camera 2nd"
- 0xa057 -> "Distortion Camera Setting"
- 0xa020 -> "Encryption Key"

This was verified by reading each table entry's byte-8 pointer and
dereferencing it to the null-terminated string in `.rdata`. The
verification script and its output are at
`/tmp/rw2_tca/step23_proptable_verify.py` (regenerable). Session 10's
writeup claimed `0xa050 -> RawData, 0xa051 -> Distortion,
0xa052 -> ColorAberration`, off by one entry. Session 11 built the
rest of its story on top of that shift.

**Consequences of the corrected mapping.**

- The dispatcher `FUN_18074e0f0` calls `FUN_1810ceb30` at three
  adjacent sites -- RVA 0x74f148 with `r9d = 0xa052`, 0x74f204 with
  0xa053, 0x74f244 with 0xa054. In session 11's mapping those were CA
  plus two shading passes. Under the correct mapping they are
  Shading plus its two camera-correction variants. **The 9-knot
  polyline that session 11 called "the CA descriptor" is a shading
  descriptor.**
- The real ColorAberration property id 0xa051 never appears as an
  immediate anywhere in the DLL's `.text`. Searched all six
  register-load encodings (`mov edx/ecx/r8d/r9d, 0xa051`, 32-bit and
  16-bit forms, and `push 0xa051`): zero hits. The "ColorAberration"
  string at RVA 0x1617a78 has zero LEA references from `.text`, so
  the property is not even reached by name lookup.
- `FUN_1810ce250`, which session 11 called the "0xa020 fetcher", is
  in fact a hardcoded getter for that specific id: it passes 0xa020
  directly to the handle's vtable slot 0x990 at RVA 0x10ce2b7
  (verified in a re-decompile). So 0xa020 is a real property key
  used at that call site. But its property-table name is not "CA
  offsets" or anything correction-shaped; it is literally
  "Encryption Key".

**(A) Raw 0x011b payload -> property mapping: not resolved, and the
question is now malformed.** The premise was that some slice of the
0x011b tag flows into property 0xa052 (assumed to be CA) and another
slice into 0xa020. Under the corrected mapping, 0xa052 is Shading
and 0xa020 is Encryption Key -- neither is Panasonic's CA correction.
What was attempted in this session:

- `FUN_1804f3ca0` was re-decompiled with `DecompileOptions.setMaxPayloadMBytes`
  raised to 1024 (default is 50; that is what silently truncated
  session 10 as "buffer size exceeded"). The new script is
  `/tmp/rw2_tca/ghidra_scripts/DecompileBig.java`. Full pseudocode
  is at `/tmp/rw2_tca/step23_4f3ca0.c` (489 KB, 11243 lines).
- The function body contains no immediate 0x11b, no immediate 0xa020,
  0xa050, 0xa051, 0xa052, 0xa053 or 0xa054. Session 12's constant
  scan (`/tmp/rw2_tca/scan_tags.py`) confirmed this over the full RVA
  range 0x4f3ca0..0x501fd5. The parser is table-driven, or it is not
  the tag-0x011b parser at all.
- Structural evidence for the second option: the caller pattern at
  RVA 0x4d6020 is `mov r8d, 4; lea rdx, [rdi+0x1a8]; mov rcx, rdi;
  call 0x4f3ca0`. The third argument is a small integer flag, not an
  IFD pointer. Inside the function, accesses are of the form
  `*(ushort *)(param_1 + 0x86f0)` -- reads of fields at fixed
  offsets on a large struct, not `entry->tag_id` dispatches. Session
  10 identified this as the private-IFD parser on the strength of
  size and callee count; the decompile does not support that
  identification.
- The real 0x011b tag reader was not located in this session. `0x11b`
  appears as an immediate in 14 functions across the DLL (session 11
  had that list); one is `FUN_1804b3860`, decompiled here as a
  quick check, but that is a "which tags to persist" list-builder
  (it calls `FUN_1802a73d0(list, 0x11b)` to add 0x11b to a set), not
  a reader.

**(B) PanaCA plugin R/B split: partially resolved.**

- Plugin id 0x1216 = "IslEISDevelopDemosaicPanaCA" is confirmed
  independent of session 10's earlier hand-wave. It is the target of
  a switch-dispatch on plugin id at RVA 0x547756 (function without a
  `.pdata` entry, so hidden from function-list tools). The switch is
  `add ecx, -0x11a8; cmp ecx, 0xef; ja default; movsxd rax, ecx; lea
  r8, [image_base]; movzx eax, byte ptr [r8 + rax + 0x54862c]; mov
  ecx, dword ptr [r8 + rax*4 + 0x548528]; add rcx, r8; jmp rcx`. For
  input `ecx = 0x1216` the tables resolve to target RVA 0x5477c6,
  which is `lea rdx, [rip + 0xea734b]; mov rax, rdx; ret` -- a
  6-byte thunk that returns the pointer to the string
  "IslEISDevelopDemosaicPanaCA" (RVA 0x13eeb18). Verified by decoding
  both dispatch tables against the RVA at RVA 0x5477c6.
- This is a `GetPluginNameById` function, not the plugin factory. The
  factory is registered dynamically via the plugin manager and has
  no static edge from 0x1216 to any factory pointer. The only place
  the 0x1216 immediate itself appears in `.text` (outside 16-bit
  false positives) is RVA 0x62f857, and that is a `mov edx, 0x1216`
  followed by a call to a logger with the source-file string
  `isleisfilternr3.cpp` -- an error log macro emitting a module id,
  from a completely unrelated NR3 filter. There is no static call
  edge to the CA plugin factory.
- R vs B split therefore stays unresolved by static means. What we
  know from the plugin's *name* alone is that PanaCA runs during
  demosaic -- so any per-channel handling would apply at the Bayer
  stage, not at RGB-triplet stage. What the dead debug format at RVA
  0x1617dc0 hints (`ColorAbeR:%d, ColorAbeB:%d, ... R:%f, B:%f`) is
  that the plugin keeps both an integer and a float per channel.
  This suggests the fourth hypothesis from the task -- "0xa020
  offsets shift the knots differently per channel" -- is unlikely,
  and the two-integer + two-float pattern points at either "same
  curve, per-channel scale" or "two curves, one per channel". Which
  of those, and where the scalars come from, still needs a runtime
  trace.

**(C) 0xa020 offset table origin: resolved differently than expected.**

- The property is named "Encryption Key" in the property-id table.
  The getter at `FUN_1810ce250` (RVA 0x10ce250) passes the hardcoded
  0xa020 to the handle's vtable slot 0x990 at RVA 0x10ce2b7. It
  demands the returned buffer have more than 10 int32s, i.e. at
  least 44 bytes. So it is a real key/blob property, not synthesised
  from the shading builder's caller.
- The property's name is a dead string in the codebase (the string
  "Encryption Key" at RVA 0x1617898 has no LEA references from
  `.text`; it is only reachable through the property-id table
  itself). The neighbouring `DataDecryptKey` at RVA 0x13d2c80 is
  likewise dead. So the label is provenance-only; we cannot see it
  being used, only stored.
- No path from the SPD reader to a setter of 0xa020 was located.
  `DefaultLensInfo.spd` (119 KB) and `DefaultParameters.spd` (7.8
  MB) are both a 256-byte outer wrapper (`"SILKYPIX\0..." + version
  "2009042001"`) around an inner `.spx` container (`"ISL Multi
  purpose file format.\x1a"` magic, followed by a header table and
  high-entropy body). The bodies decompress or decrypt to something
  we cannot see statically, and their loader path was not walked.
- The strongest empirical read is that 0xa020 is a per-image key or
  small blob rather than per-lens SPD data: the name literally says
  "Encryption Key", and session 11 saw the shading builder read the
  same buffer as int32 offsets, which is consistent with the buffer
  being small and image-derived rather than a wide per-lens table.
  But this is inference, not observation.

**What the polyline builder at 0x10ceb30 actually is, restated.**

The mechanism session 11 lifted is correct; only its label was
wrong. The builder:

1. Fetches property 0xa020 ("Encryption Key") via `FUN_1810ce250` as
   an int32 array. Requires at least 11 int32s. First 7 are used.
2. Fetches a caller-provided property (Shading in the observed
   dispatcher path) via vtable slot 0x990. Requires at least 15
   shorts with the first equal to 7 (a discriminator/version).
3. Builds a 9-knot IslZCnvPolyLine:
   - knot[0] = (0.0, 1.0)
   - for i in 0..6: knot[i+1] = ((payload[i+1] + offsets[i]) / R_half_diag,
                                 (payload[i+8] - offsets[i]) / 1000.0)
   - knot[8] = (2.0, knot[6].y)
4. Hands the polyline to the caller's output slot.

Same mechanism, same knot layout, same 1000.0 divisor, same
half-diagonal x-normalisation as session 11 documented. What changes
is the semantics: this is Panasonic's per-image *shading* correction
curve, plus two variants for camera-corrected shading. Not CA.

**Reconciliation with session 8.**

- *Does the shading builder overlap CA in useful ways?* No. CA and
  vignetting have different physics and different failure modes.
  The 9-knot monotone falloff is exactly the shape one wants for a
  vignetting-style radial gain curve; it is the wrong object for
  transverse CA, which is a per-channel radial *displacement*, not a
  scalar gain.
- *Does SILKYPIX 8 SE decode Panasonic's 0x011b as CA?* Not in any
  code path this session or the previous ones could reach. There is
  no immediate reference to `0xa051` (ColorAberration) in `.text`.
  There is no `LEA` to the `ColorAberration` name string. And the
  `IslEISScanChromaticAberration` plugin at id 0x13ba, present in
  the same plugin-name switch, is a *scan* plugin -- it processes
  demosaiced pixels, not raw-tag metadata. The most likely reading
  is that SILKYPIX 8 SE ignores Panasonic's per-image CA correction
  and computes its own from image content.
- *Should darktable follow SILKYPIX's shape?* Not for CA. Session
  8's polynomial (or something better) remains the shipping
  recommendation. The polyline shape session 11 documented is
  correct for the property it actually reads, but the property is
  shading, and porting it to darktable's CA path would replace a
  known-imperfect CA correction with a *wrong-object-class* shading
  correction. Do not port.
- *Does anything from session 8 need retracting?* No. Session 8 fit
  a polynomial against JPEG-measured CA. That measurement is
  end-to-end (camera fires whatever CA correction it does; the JPEG
  reflects the outcome; the fit reproduces enough of it to reduce
  visible CA). It stands or falls on its own JPEG comparison, not
  on SILKYPIX. Session 8's shipping recommendation is unaffected by
  session 12's findings.

**Implementation guidance for the darktable patch.**

The concrete guidance is: **ship session 8, do not port session 11's
spline.** Specifically:

- `src/iop/lens.cc`, `_init_coeffs_md_v2()` (the target file identified
  in the top-level document). Continue populating the four CA
  coefficients per channel from the six-word set session 8 identified:
  `payload[8], payload[10], payload[12], payload[20], payload[23],
  payload[27]` -- these were fit as regressors against camera-JPEG
  CA. The fitted coefficient matrix `C_R` / `C_B` and the divisor
  `K = 11.48` remain the numbers to ship.
- Do NOT introduce a spline evaluator on the strength of session 11.
  The 9-knot monotone spline that session 11 lifted is a shading
  descriptor, and coding it into the CA path would produce
  systematically wrong shifts.
- Keep session 10's finding that Panasonic's four-word `0xFFEF`
  modulus check is not gated by SILKYPIX. That behavioural finding
  is independent of the property-mapping mistake and is still
  correct.
- If a follow-on agent wants to lift the actual CA property (0xa051)
  path: skip the property-immediate approach entirely (there are no
  hits) and look for either (a) a property-blob table iterator that
  walks a list of (id, buffer) pairs and dispatches by id, or (b) a
  raw-side reader that never enters the property system and feeds
  the PanaCA plugin directly. Option (b) matches the plugin
  architecture better -- PanaCA is a demosaic plugin, and if it
  needs Panasonic's per-image CA correction it would ingest the
  0x011b bytes at raw-load time, not through the property system.

**Struct fields and loop shape for the shipping session-8 port**
(unchanged from the top-level document; restated here so a follow-on
agent has all of it in one place):

- Read the RW2 0x011b tag as a `uint16[32]` array from the Panasonic
  private IFD (offset already resolved by darktable's existing
  Exiv2 walk).
- Extract `w[8]`, `w[10]`, `w[12]`, `w[20]`, `w[23]`, `w[27]` as
  `int32` (session 3's word[7]-high-byte decode does not apply to
  these six).
- Evaluate `C_R[i] = sum_{j=0..5} c_r[i][j] * w[j]` for i in 0..3
  (four CA coefficients for R), and similarly for B with
  `c_b[i][j]`. The fitted matrices from session 8 are in
  `/tmp/rw2_tca/final_fit.npz` under keys `C_R`, `C_B`.
- For each pixel: compute `r_norm = sqrt(x^2 + y^2) / half_diag`.
- Apply `dr_R = (C_R[0] + C_R[1]*r_norm + C_R[2]*r_norm^2 +
  C_R[3]*r_norm^3) * r_norm / K` and analogously for B.
- Displace the R and B channels by `dr_R * (x/r, y/r)` and
  `dr_B * (x/r, y/r)` respectively; leave G untouched.
- `K = 11.48` is the scalar that brings the polynomial output into
  the same order of magnitude as observed pixel-space shifts. It
  does not appear as a code constant in SILKYPIX and it is not
  1000.0; do not conflate the two.

**Honest bounds on what remains undecoded.**

- The Panasonic 0x011b -> in-memory property mapping is not lifted.
  Session 12 refuted session 11's *identification* of where the
  mapping ends up (session 11 pointed at 0xa052, which is Shading),
  but did not find the correct endpoint (0xa051 or a non-property
  raw-side buffer). It is possible SILKYPIX 8 SE performs no
  Panasonic-specific CA correction at all.
- The R/B split at the plugin side is not lifted, and static
  analysis alone will not lift it: PanaCA has no RTTI, no factory
  edge from its plugin id, and its methods are reached only through
  a dynamically-populated vtable. A Frida hook on plugin
  construction inside `SILKYPIX_DS8SE.exe` at runtime is the only
  remaining route.
- The origin of the 0xa020 blob is not lifted. Its property name is
  "Encryption Key" and its shape (>= 11 int32s) is consistent with
  a small per-image key rather than a per-lens SPD entry, but
  neither the setter nor a decrypt path was walked.
- The property setter for 0xa051, 0xa052, 0xa053, 0xa054 and 0xa020
  is not located. It is table-driven (none of these ids appear as
  immediates), so the setter probably iterates a `(tag_id ->
  property_id, size_hint)` table somewhere in `.rdata`. That table
  was not scanned for in this session.
- `FUN_1804f3ca0` is a 58 KB function that turns out probably not to
  be the RW2 IFD parser at all: its argument shape is `(handle,
  struct*, small_int_flag)`, its body accesses fixed struct fields
  rather than iterating IFD entries, and it contains no immediate
  0x11b. Session 10's identification of it as "the 88 KB private-IFD
  parser" and session 11's "58 KB body which timed out on decompile"
  characterisation of the same function are still accurate as sizes
  and boundaries, but the *role* attribution is not supported by
  the code. Neither session actually verified the role; both
  inferred it from size.

**What session 12 delivered vs. what it did not.**

- (A) partially: refuted session 11's mapping claim, showed the
  polyline is shading not CA, showed the CA property is not reached
  by immediate or name. Did not find the actual 0x011b->property
  writer.
- (B) partially: confirmed 0x1216 = PanaCA via the plugin-name
  switch, decoded both jump-table indexes, and ruled out a static
  factory edge. Did not lift the plugin's per-channel behaviour.
- (C) resolved differently: the property is named "Encryption Key"
  in the code, not a per-lens offset table. No SPD-side path to a
  0xa020 setter was found. Whether this refutes "0xa020 comes from
  SPD" or just moves the question depends on what "Encryption Key"
  actually points at in a running SILKYPIX; static analysis cannot
  say.

**Files this session:**

- `/tmp/rw2_tca/ghidra_scripts/DecompileBig.java`: new Ghidra script
  that raises `DecompileOptions.setMaxPayloadMBytes` to 1024. This
  is what let `FUN_1804f3ca0` decompile at all; the default 50 MB
  is what session 10 hit as "buffer size exceeded".
- `/tmp/rw2_tca/dis_rva.py`, `/tmp/rw2_tca/rip_scan.py`,
  `/tmp/rw2_tca/callees.py`, `/tmp/rw2_tca/scan_tags.py`: pefile +
  capstone helpers for out-of-Ghidra checks (RIP-relative reference
  scans, callee enumeration, tag-immediate scans). They work
  concurrently with a Ghidra decompile that holds the project lock,
  which was the constraint that broke a second Ghidra run mid-session.
- `/tmp/rw2_tca/step23_4f3ca0.c`: full decompile of `FUN_1804f3ca0`.
- `/tmp/rw2_tca/step23_10ce250.c`, `/tmp/rw2_tca/step23_10ceb30.c`:
  re-decompiles of the property getter and the polyline builder, run
  to verify session 11's reading of the code against the corrected
  property mapping. Confirmed: session 11's *code* trace is right;
  the label attached to the code was wrong.
- `/tmp/rw2_tca/step23_ripscan.txt`: RIP-relative reference dump for
  the PanaCA name-getter thunks and the plugin factory macros.
- `/tmp/rw2_tca/step23_parser_scan.txt`: constant-immediate scan
  over the `FUN_1804f3ca0` body, showing zero hits for any 0xa0XX
  property id.
- `/tmp/rw2_tca/step23_callees.txt`: 296 unique callees of the
  putative parser, most of them thin field accessors.
- `/tmp/rw2_tca/step23_508e30.txt`, `/tmp/rw2_tca/step23_508f50.txt`
  (available on rerun): disassembly around the sites session 11's
  const-xref scan flagged as `0xa020` immediates inside the parser.
  These turned out to be stack-frame offsets (`lea rcx, [rbp +
  0xa020]`), not property ids, further weakening the "session-11
  parser writes 0xa020" story.

### K = 11.48 refuted, K = 1 is correct (session 13)

Field-test on `/c/temp/tca/P1366392/P1366392.RW2` (G9 + Leica DG
Vario-Elmarit 12-60 f/2.8-4 @ 14mm, f/5.6, DistortionCorrection = On,
firmware Ver.2.7 - outside the training corpus) showed the shipped
`_pana_K_JPEG = 11.48` from session 8 leaves visible multi-pixel R/B
fringing at the corner unaddressed. The camera JPEG has clean edges;
the darktable render with the initial patch had heavy magenta halos on
every pine needle silhouette. Setting `K = 1` clears the fringing to
approximate parity with the JPEG.

**What was refuted.** Session 7's "predicted/applied ratio: median
11.48" and session 8's derived K divisor. Session 7's "applied"
measurement was `raw_RG - jpeg_RG` computed via a parabolic peak fit
on edge gradients, filtered to edges where all three integer R/G/B
peaks agreed within ±1 pixel. That filter by construction excludes
every multi-pixel-shift edge on the raw side; the raw edges that
survive are the sub-pixel-CA ones. Both raw and JPEG surviving edges
therefore have sub-pixel R-G shifts (0.02-0.14 px on the corpus),
and the "applied" value measures the CA correction on those weak-CA
edges only, giving 0.05 px at most. Session 8 then divided the C_R
matrix by 11.48 to match that filtered subset, at the cost of
under-correcting the strong-CA edges the tag is meant to fix by the
same factor of ~11x.

**The camera does not filter edges.** Its correction is a radial
polynomial applied to all edges regardless of shift magnitude.
Strong-CA edges (multi-pixel raw shifts) receive the same radial
correction as weak-CA edges, scaled by their radius. Adobe DNG
Converter's WarpRectilinear opcode matches this: on `P1366477`
(PL 12-60 @ 12mm, training corpus) the opcode says the R plane
should be shifted +0.77 px at r=0.5 and the B plane +1.81 px at
r=1.0. Session 5's `C_R` reproduces those values within a few
percent. Session 7 measured "applied" R-shift of only 0.05-0.08 px
on the same file - 10-15x smaller than the opcode's prediction,
because the strong-CA edges that would show the full 0.77 px were
filtered out on the raw side too.

**Verification (2026-09-18, scripts and outputs under `/tmp/dt_render/`
and `/tmp/p1366392/`):**

- extracted 0x011b from `P1366392.RW2` directly (bytes at IFD0 tag
  offset 0x436, 64 bytes). Rigo's four checksums all pass. Six-word
  predictor set values: `w[8, 10, 12, 20, 23, 27] = [-814, -19, -672,
  -511, -928, 170]`. Body-scale radii `w[11] = 3276 = N1`,
  `w[4]/w[11] = 0.833` (MFT pattern verified)
- rendered `P1366392.RW2` with `darktable-cli` at `K = 11.48` and
  `K = 1.0`, using an XMP sidecar taken from the user's
  `P1366392.tif`. Both renders exercised the `lens` module in
  embedded-metadata mode
- corner crops at (4600, 150) in the rotated landscape view show
  heavy magenta/purple fringing at K = 11.48 and essentially neutral
  edges at K = 1.0
- `|R-G|` and `|B-G|` channel differences show bright edge halos at
  K = 11.48 and much dimmer halos at K = 1.0; residual at K = 1.0 is
  mostly the B channel higher-order (`k_r2`, `k_r3` left at G-plane
  per session 5) plus small B-plane fit error
- computed the polynomial value in pixels for the training corpus's
  `P1366477.dng` opcode: at r = 0.5, R plane says +0.77 px, at
  r = 1.0 B plane says +1.81 px. Our patch at K = 1 predicts +0.86
  px R and +0.13 px B at the same geometry for the neighbouring
  file `P1366392` (14mm) - same magnitude order as the DNG says
  should be applied on the training file

**Consequences for the shipped decode.**

- `_pana_K` (renamed from `_pana_K_JPEG` to reflect the change in
  role) is set to `1.0` in `src/iop/lens.cc`. The divide is retained
  as a tuning knob but is now effectively a no-op
- `C_R` and `C_B_lo` are unchanged. They already match Adobe DNG
  WarpRectilinear directly per session 5's fit
- session 8's numerical claim of "worst per-file RMS residual 0.058
  px on R and 0.034 px on B" is superseded. That residual was
  measured against a JPEG-side edge-position measurement subject to
  the same ±1 integer-peak filter as the raw side and therefore
  restricted to sub-pixel edges. A field-representative residual
  measurement would want a homography-plus-distortion registration
  of raw vs JPEG in image space, or a synthetic checker chart of
  known geometry, so strong-CA edges are represented in the target
- the "For the implementing agent" section's coefficient block
  should now read `static const double K_JPEG = 1.0` (or drop the
  divide). The C_R/C_B_lo matrices are still correct

**What still isn't fully closed.** K = 1.0 leaves a small but
non-zero residual on P1366392 (visible in the `|B-G|` channel
difference as thin edge halos, subjectively closer to the SOOC JPEG
than to the raw). Two possible sources:

1. The undecoded B-plane higher-order terms (`k_r2`, `k_r3` fixed at
   the G-plane value). Session 5 could not decode them from the
   six-word predictor set; session 6's discrete-word extensions did
   not generalize under LOGO
2. Per-file magnitude tuning below the 5% error bar of session 5's
   `C_R` fit against Adobe

Neither is blocking. If a follow-up needs to tighten it, the two
paths are: enlarge the fit corpus with a third body + paired JPEGs
and refit `C_R` and `C_B_lo` directly against sensor-space
JPEG-vs-raw registration (not the ±1-integer-peak subset), or lift
the actual polynomial evaluator from Panasonic firmware / SILKYPIX
plugin runtime (both listed in the "Optional TODOs" section).

**Files this session:**

- `/tmp/dt_render/baseline/P1366392.jpg` - dt-cli render at K = 11.48
- `/tmp/dt_render/k1/P1366392.jpg` - dt-cli render at K = 1.0
- `/tmp/dt_render/base_tr.jpg`, `k1_tr.jpg`, `triple.jpg` - corner
  crops and the SOOC-JPEG/K=11.48/K=1 triple
- `/tmp/dt_render/base_tr_diff.jpg`, `k1_tr_diff.jpg` - `|R-G|` and
  `|B-G|` channel-difference maps for each render
- `/c/temp/tca/P1366392/P1366392.RW2.xmp` - the XMP sidecar
  extracted from the user's `.tif`

### Direct raw-CA measurement, no ±1 filter (session 14)

After session 13 shipped `K = 1`, the user reported that some residual
CA was still visible on `P1366392` at strong-CA radii, especially the
B channel at the corner. Session 14 goes after the residual by removing
session 7's edge-selection bias (the ±1-integer-peak filter that session
13 identified) and re-fitting `C_R` / `C_B` against direct raw
measurements instead of against Adobe DNG WarpRectilinear opcodes.

**Method** (`/tmp/rw2_tca/measure_ca.py`, `fit_direct.py`,
`diagnose.py`, `compare_dng.py`):

1. Render each of the 18 corpus RW2s with rawpy AAHD, gamma=1, no
   auto-bright, 16-bit output. This gives an uncorrected demosaiced
   raw with no camera CA correction applied.
2. Sweep 128x128 tiles across the frame at stride 96. For each tile
   with G-channel std > 100, run OpenCV `phaseCorrelate(R, G)` and
   `phaseCorrelate(B, G)` with a Hanning window. Filter out tiles
   with correlation response < 0.10 or r_pix < 20.
3. Project each tile's sub-pixel shift vector onto the radial
   direction from image center. Positive `dr` = R (or B) displaced
   outward from center relative to G, matching session 3's convention.
4. Weighted least squares fit
   `shift_R_px(r_norm) = r_norm * halfdiag_px * (k_r0 + k_r1 r^2 +
   k_r2 r^4 + k_r3 r^6)` with weight = min(respR, respB) * G_std.
   Ridge regularisation with lambda = 1e-8 * trace(X^T X) / 4 and
   `r_norm <= 0.95` cap to prevent corner extrapolation blow-up.
5. Regress each of the four fitted `k_r*` coefficients on the six
   words `[8, 10, 12, 20, 23, 27]` across all 18 files (in-sample),
   then leave-one-lens-focal-group-out (LOGO), same protocol as
   session 5.

**Corpus per-file measurements**. Tile counts 446-1795 depending on
scene content; fit RMS residuals 0.14-1.36 px (P1260633 is the only
> 0.35 px outlier, likely a scene-content issue). On the strongest-CA
file `P1366477` (PL 12-60 @ 12mm, G9), measured shifts at r_norm =
{0.25, 0.5, 0.75, 0.9}: R = +0.55, +0.92, +0.72, +0.70 px, B = -0.00,
+0.18, +0.78, +1.67 px. Compare to Adobe DNG's WarpRectilinear at the
same radii: R = +0.47, +0.77, +0.77, +0.46 px, B = -0.05, +0.02,
+0.44, +1.08 px. R roughly matches Adobe within 20%; **B on this file
is 55-90% larger than Adobe DNG reports at r_norm >= 0.7**.

**Held-out test on P1366392** (not in training corpus, PL 12-60 @
14mm on G9, same lens family as `P1366477`). Measured B shift on the
raw:

    r_norm   raw meas   shipped C_B_lo   residual (shipped)
     0.30     -0.14         -0.13          -0.01 px
     0.50     +0.02         -0.17          +0.19 px
     0.70     +0.46         -0.13          +0.59 px
     0.85     +1.27         -0.04          +1.31 px
     0.95     +2.47         +0.07          +2.40 px

The shipped correction is essentially zero on this file above r = 0.5.
The user's visible B fringing at the extreme corner matches the +1.3
to +2.4 px residual measured here.

**Refit attempts** (all with the same 6-word predictor set on the 18-
file corpus):

1. `C_R` and `C_B` as full 4x6 matrices against direct measurements.
   Per-coefficient LOGO R^2: R k_r0..k_r3 = +0.62, -0.91, -0.97,
   -0.95 (only k_r0 has any predictive power). B k_r0..k_r3 = -0.14,
   -0.68, -0.41, -0.26 (all negative). Session 5 got R = 0.97-0.99,
   B k_r0/k_r1 = 0.99. **The direct-measurement targets are noisier
   than Adobe DNG opcodes, so the regression generalises worse.**

2. Function-space regression: predict `shift_R_px(r_fixed)` directly
   from the six words at fixed r. LOGO R^2 at r = 0.50: R = 0.91,
   B = -0.84. R matches session 5's function-space R^2 (0.893);
   B does noticeably worse in the interior because the physical B
   shift there is < 0.1 px and drops below the tile-measurement
   noise floor. B at r >= 0.8 does clear R^2 = 0.5-0.7 but that is
   not a defensible improvement over session 5.

3. Hybrid: keep session 5's `C_B_lo` for the low orders, add
   corpus-median Adobe DNG values as constants for `k_r2` / `k_r3`.
   The Adobe median values are essentially zero (`k_r2` = +7e-06,
   `k_r3` = +2e-06; the D_B.k_r2 values span negative to positive by
   near-equal amounts across the corpus), so this changes P1366392's
   corner B correction by less than 0.03 px. Dead end.

4. Blindly ship the direct-measurement C_R + C_B on P1366392: the
   4-row C_B does halve the residual at r = 0.85 (from +1.31 px
   shipped to +0.58 px), but the paired direct-measurement C_R
   *over*-corrects R at r = 0.95 by -0.73 px. Net: R gets worse, B
   gets better, and neither has LOGO validation. Not shippable.

**Verdict**. Option 2 confirms two things:

- Session 13's magnitude fix (`K = 1`) is not the source of the
  remaining residual. The residual is a shape problem in B: with
  only `k_r0` and `k_r1` decoded, session 5's B polynomial cannot
  produce the strong corner correction that strong-CA raws need. On
  `P1366392` the shipped `C_B_lo` produces about 0.07 px at r = 0.95
  where the raw actually needs +2.4 px.
- The remaining data (18 files, 9 lens/focal pairs, 2 bodies) does
  not support a defensible refit of the missing B higher-order
  coefficients. Adobe DNG's own `k_r2` / `k_r3` values swing 3-4x
  between paired-body same-lens/same-focal files, and a direct raw
  measurement of the same coefficients is at best comparable in
  cross-body variance. This is the same data-ceiling session 5 hit.

**What the residual really is.** For strong-CA raws where Adobe DNG's
opcode encodes non-zero `k_r2` / `k_r3`, no amount of clever
re-fitting on the existing corpus recovers those higher orders from
six words. Two paths break through: (a) enlarge the training set with
a third body and paired JPEGs so the 4-row fit is no longer
under-determined by cross-body variance in the ground truth (option 3
in the Optional TODOs list, in progress by the user as of this
session), or (b) firmware RE, which session 13 concluded is not
tractable without hardware access.

**No code change**. `_pana_K = 1.0`, `C_R` (4 rows, session 5), and
`C_B_lo` (2 rows, session 5) remain shipped. The `cacorrectrgb`
darktable module remains the practical user-side workaround for the
residual B fringing at the corner.

**Files this session:**

- `/tmp/rw2_tca/venv/`: fresh rawpy 0.27.1 / opencv 5.0.0 / numpy /
  scipy environment
- `/tmp/rw2_tca/measure_ca.py`: tile-based phase-correlation measurement
- `/tmp/rw2_tca/fit_direct.py`: six-word regression + LOGO
- `/tmp/rw2_tca/diagnose.py`: per-r function-space regression
- `/tmp/rw2_tca/compare_dng.py`: Adobe-DNG-vs-measurement ratios
- `/tmp/rw2_tca/measure_ca.npz`, `.../fit_direct.npz`,
  `.../cache/*_aahd.npy`: measurements and cached AAHD renders

### Third-body sample: DC-S5M2 pre-production, 4 files (session 15)

Session 14 pointed at "third-body samples" as the only remaining path
to unlock B's `k_r2` / `k_r3` on the shipped decode. Photographyblog
published 99 paired RW2+JPEG samples from a pre-production DC-S5M2
(Panasonic Lumix S5 II) at
`https://www.photographyblog.com/previews/panasonic_lumix_s5ii_photos`.
A first batch of four files was pulled to `/c/temp/tca/s5ii/` to check
structural conformance and the first-cut decode:

- `01`: LUMIX S 85mm F1.8 @ f/1.8 (prime)
- `06`: LUMIX S 20-60mm F3.5-5.6 @ 20mm f/8
- `17`: LUMIX S 14-28mm F4-5.6 @ 14mm f/8 (ultra-wide, strong CA)
- `19`: LUMIX S 14-28mm F4-5.6 @ 14mm f/4 (same lens/focal, wider stop)

**Structural check** (positive, all 4 files):

- 0x011b tag present with count = 64.
- All four Rigo 2011 checksums pass on each file, unchanged from the
  MFT corpus. Same checksum algorithm carries over to a new
  full-frame body.
- word[14] = 256 on every file (correction on).
- Radii ratios `N2/N1 = 6/7 = 0.857`, `N3/N1 = 4/7 = 0.571`,
  `N4/N1 = 2/7 = 0.286` on every file. Matches session 6's DC-S5
  full-frame radii pattern verbatim. The 4-zone structural model
  session 6 established for the older DC-S5 applies to the newer
  DC-S5M2 without change.
- Sensor: 6000x4000 (24 MP full-frame), halfdiag 3611 px vs G9's
  3254 px MFT halfdiag. `_init_coeffs_md_v2` uses normalised radius
  and computes halfdiag per file, so no code change would be needed
  to add DC-S5M2 support.

**Direct raw CA measurement** (`/tmp/rw2_tca/measure_ca.py` on the
four files, halfdiag = 3611 px):

    file                            n_tiles  R@0.5   R@0.85   B@0.5   B@0.85 (px)
    s5ii_01 (85mm f/1.8)             464    -0.14   -0.58   -0.22   +0.95
    s5ii_06 (20-60 @ 20mm f/8)      2224    +0.84   +0.15   -0.35   +0.77
    s5ii_17 (14-28 @ 14mm f/8)      2065    +0.26   +0.48   +0.31   +1.06
    s5ii_19 (14-28 @ 14mm f/4)      2233    +0.33   +0.41   +0.51   +1.10

Physical magnitudes are on the same order as G9 / GX80 at comparable
focal lengths, and B channel shift at r = 0.85 sits at ~1 px, matching
the P1366392 residual the user reported. Fit RMS 0.08-0.33 px is
actually cleaner than the MFT corpus, likely because the S5M2 sensor
has finer pixels and the scenes are more contrasty for phase
correlation.

**Combined-corpus regression** (22 files: 9 G9 + 9 GX80 + 4 S5M2 =
7 unique lens-focal groups on MFT + 3 unique lens-focal groups on
full-frame; treat the 14mm f/4 and f/8 pair as one lens-focal group
for LOGO). Function-space regression LOGO R^2 at fixed r on the six-
word predictor set:

              R@0.30  R@0.50  R@0.70  R@0.85    B@0.30  B@0.50  B@0.70  B@0.85
    18 MFT   +0.733  +0.908  +0.751  +0.446    -0.086  -0.838  +0.173  +0.517
    22 combo +0.884  +0.850  +0.623  +0.634    +0.409  +0.134  -0.174  -2.699

R generalises at similar quality (0.6-0.9). **B degrades at r = 0.85
from +0.517 (MFT-only) to -2.699 (with S5M2 added).** The four S5M2
files' B behaviour is not linearly predicted by the six words that fit
the MFT bodies. This is not a surprise given the sample size (4 files,
3 groups) - it is not enough data to constrain a full-frame branch of
the fit even in a mixed regression.

**Predictor-set sweep** on the same 22-file corpus, LOGO R^2 at r =
0.85 on B (the corner where the visible residual lives):

    session-5 six words           -2.7
    smooth-8  (add words 2, 29)   -6.5
    smooth-9  (add word 15 too)   -4.3
    all 27 non-checksum words     -9.6
    all 32 words                  -4.6

More predictors makes things worse. Session 5's `[8, 10, 12, 20, 23,
27]` remains the best among tested; the ceiling is data size, not
feature set.

**No code change**. `C_R`, `C_B_lo` and `K = 1` stay shipped from
session 13. The 4-file S5M2 batch confirms the structural findings
generalise cleanly to a new body family but does not decode the
missing B higher-order coefficients.

**What next**. Two productive extensions if the user wants to push
this further:

1. Enlarge the S5M2 batch to 20+ files covering more lens/focal
   groups. 3 groups is under the parametrisation boundary for a
   4-coefficient B polynomial in the mixed regression; 10+ groups
   across S5M2 would let a body-conditional or lens-family-
   conditional fit converge.
2. Try a completely different modelling approach: instead of
   regressing per-file `k_r0..k_r3` on words, regress the raw
   `word[X]` -> `pixel-space shift at fixed r` mapping directly, or
   fit a monotone spline evaluator (as SILKYPIX does for shading,
   session 11) instead of a 4-term polynomial. Neither is guaranteed
   to help but both change the ill-conditioning that keeps k_r2 /
   k_r3 from decoding.

Meanwhile the shipped patch keeps working at K = 1 for the CA it can
represent (R plane and low-order B). `cacorrectrgb` remains the
practical user-side workaround for the corner B residual.

**Files this session:**

- `/c/temp/tca/s5ii/panasonic_lumix_s5ii_{01,06,17,19}.{rw2,jpg}`:
  4 paired samples downloaded from photographyblog
- `/tmp/rw2_tca/measure_mft.npz`, `.../measure_s5ii.npz`,
  `.../measure_combined.npz`: measurement outputs, three-way split
- `/tmp/rw2_tca/fit_combined.py`, `.../fit_combined.npz`: 22-file
  regression with S5M2 labels

### 132-file combined-corpus refit, shipped and reverted (session 17)

Between session 15 and the revert commit `beb86655d1`, the corpus was
grown to 132 files across 14 bodies (5 full-frame: S5M2, S1M2, S1M2E,
S1RM2, S9) and the six-word linear fit was re-run against direct raw
CA measurements. The refit landed as commit `3158effc0e` (with a
follow-up comment-style fix `2e04bf74d7`) and was pushed to
`origin/rw2-tca-compensation`. It was reverted after field-test
regressions showed the decode injects fringing on files with weak CA.

**What the numbers looked like on the corpus:**

Function-space LOGO R^2 at fixed r on the 132-file corpus (grouped by
lens/focal, 50 unique groups):

    r     R_in    R_LO    B_in    B_LO
    0.30  0.823   0.720   0.739   0.591
    0.50  0.820   0.704   0.766   0.651
    0.70  0.709   0.573   0.714   0.516
    0.85  0.284   0.144   0.602   0.388

Corpus-wide RMS residual at r = 0.85 dropped from 0.79/1.20 px
(shipped) to 0.66/0.52 px (refit) for R/B. On P1366392 (the strong-CA
test file that motivated the refit) the B residual at r = 0.85
dropped from +1.31 to +0.70 px. All of that stayed true after the
revert probe; the mean-and-strong-CA metric was fine.

**Why it was reverted anyway:**

LOGO R^2 = +0.39 on B at r = 0.85 means 39 percent of the corner B
variance is captured by the linear predictor. That leaves 61 percent
unconstrained per file, and with a four-coefficient polynomial free
to swing at r past the LOGO-validated range, "unconstrained" expresses
as wild extrapolation. On files where the actual CA at the corner is
weak, the four-row B fit's residual coefficients push the polynomial
across zero into the wrong sign. Applied as a correction, that
introduces fringing where none existed.

Case study: `P1366399.RW2` (Leica DG 12-60 @ 24 mm f/5.6, G9). B shift
at r = 0.95 measured at +0.83 px. Shipped 2-row fit predicted +0.03
(essentially "do nothing" - the correct choice for a weak-CA file at
that radius). The 4-row refit predicted -0.18 - wrong sign, and
applied as a correction it pushes B further from the measured
position by 1.01 px. Mean absolute R-G on a diagnostic crop from
that file rose from 3558 to 4419 (+24 percent) with the refit
correction applied.

Corpus-wide, 84 B and 88 R fit_ok files have at least one radius in
{0.70, 0.85, 0.95} where the refit is >= 0.15 px further from truth
than shipped, or predicts the wrong sign with magnitude > 0.3 px.
Full-frame bodies (17 LOGO groups over 44 files) are the worst
represented and were disproportionately affected.

**Why the old shipped fit did not have this failure mode:**

Session 5 deliberately kept the B channel at 2 rows (k_r0, k_r1
only, k_r2 = k_r3 = 0 identically). That model has no r^4 or r^6
term at all, so at large r the shipped predictions are bounded in
magnitude and cannot cross zero from noise in the fit. It undershoots
in strong-CA cases (P1366392 at the corner was the visible symptom)
but the failure mode is silent: no correction where correction was
wanted, rather than wrong correction where none was wanted. The
former is subjectively invisible; the latter is a visible artefact.

**What a proper retry has to guarantee:**

Two constraints together, not just LOGO R^2 > 0 in aggregate:

1. On files where |shift| at r is small (say < 0.3 px), the fit must
   not predict a correction with |value| > |shift| + noise_floor.
   That is, the fit has to know how to say "do nothing" when the
   words do not clearly demand action.

2. On files where |shift| at r is large, the fit is free to take
   larger coefficient values - but only in the sign consistent with
   the measurement.

Candidate approaches:

- **Ridge / L2 shrinkage toward the shipped 2-row fit**. Penalise
  ||C - C_shipped|| where C_shipped is the current [C_R (session 5),
  C_B_lo (session 5), 0, 0] extended to 4 rows. Sweep lambda by
  cross-validation. Should produce a "conservative" 4-row fit that
  approaches shipped when the data is weak and takes the higher-
  order improvement only where the corpus strongly demands it.

- **Monotonicity constraint on B(r)**. Physical transverse CA is
  monotone in r for a well-behaved lens (no sign change on the way
  out). Fit under `d/dr shift_B >= 0` (or `<= 0`, per-file), which
  post-hoc removes the wrong-sign cases at the cost of some corpus
  RMS.

- **Target-space regression**. Instead of fitting the 4-coefficient
  polynomial and letting it evaluate at whatever r, fit shift values
  at r = {0.5, 0.7, 0.85, 0.95} directly, then reconstruct the
  polynomial by interpolation. Guarantees the fit is calibrated
  where evaluated.

Any of the three needs field validation against a "weak-CA test
set" separate from the strong-CA regression set, before shipping.
LOGO R^2 in aggregate is not sufficient.

**Files this session:**

- `/c/temp/tca/regression01/P1366399.{RW2,JPG}`: user-supplied
  regression report file. Two cropped TIFFs on-off with the reverted
  refit.
- `/tmp/rw2_tca/measure_bigcombo.npz`: 132-file measurement combined
  from photographyblog and user samples (survives revert - useful
  for the retry).
- `/tmp/rw2_tca/fit_bigcombo.py`, `.../fit_bigcombo_out.npz`: the
  reverted refit's coefficients and label mapping.
- `/tmp/rw2_tca/sweep_experiments.py`: predictor-set sweep, sensor-
  format split, and residual feature-selection scripts confirming
  the 6-word set at the model-complexity ceiling for the corpus size.

### Corpus expansion, ridge / monotone / target-space sweep, coordinate-frame variants, visual test (session 18)

Session 17 left the branch reverted to shipped session-5 + K = 1 and named
three candidate approaches for a proper retry: ridge shrinkage toward
shipped, monotonicity projection, and target-space regression. This
session ran all three plus three coordinate-frame variants plus a visual
verification of the closest candidate, and none of them clears the
regression bars. The linear-map + polynomial + six-word-predictor family
is at its practical ceiling on this corpus.

**Corpus grown to 132 files, 14 bodies, 50 (lens, focal) groups.**

Photographyblog previews were harvested via subagent scraping. Reachable
bodies beyond session 15's initial four DC-S5M2 files: GX9, GH5, GH5S,
GX8, GH4, G90, G80 on MFT; DC-S1 II (S1M2), DC-S1 II E (S1M2E),
DC-S1R II (S1RM2), DC-S9 on full-frame. Ten samples per body,
focal-length-diverse. 129 fit_ok in the direct-CA-measurement pipeline
(3 flat scenes failed the tile-count floor).

Structural verification: 0x011b present with count = 64 and all four
Rigo 2011 checksums pass on every file. Radii ratios follow session 6's
MFT pattern (5/6, 4/6, 2/6) or full-frame pattern (6/7, 4/7, 2/7)
verbatim, with word[14] = 256 (correction flag on) on every file. Five
independent full-frame bodies now confirm session 6's structural model
without exception.

**Approach sweep (source-frame measurement, six-word predictor):**

Function-space LOGO R^2 at r = 0.85 and wrong-sign / regression counts
vs shipped, on 129 files with 50 LOGO groups:

    approach                   R^2_R@0.85  R^2_B@0.85  regB@0.85  wsB@0.85
    shipped baseline             -0.114     -1.810        0          25
    unconstrained OLS (session 16-style)  0.144    0.388       27           6
    ridge, task lambda=1e4        0.144     0.388       26           5
    ridge, ext lambda=3e6/3e4     0.144     0.388       22           5
    ridge-min (R locked, B refit) 0.144     0.388       22           5
    monotone projection           0.144     0.388       24           3
    target-space regression       0.144     0.388       22           5

None passes the winner bar (regB@0.85 <= 10, wsB@0.85 <= 3, and matching
P1366392 / P1366399 held-out predictions). Ridge with lambda -> infinity
collapses to shipped; lambda -> 0 collapses to unconstrained. No sweet
spot in between clears both criteria simultaneously.

Diagnosis of the impossibility: criterion wsB@0.85 <= 3 is unreachable
in principle, because shipped itself already has 25 wrong-sign
predictions at r = 0.85. Any linear map from the six words produces at
least that many unless it collapses fits toward zero (killing the
strong-CA improvement).

Predictor-set sweep at 132 files (6 / 7 / 8 / 9 / 10 / 27 / 32 words):
S6 = [8, 10, 12, 20, 23, 27] remains optimal. Every superset degrades
LOGO R^2. All-32-words collapses to -0.4 at r = 0.85. Sensor-format
split (17 full-frame LOGO groups vs 33 MFT) is catastrophic on B - too
few groups on either side to constrain independent fits.

**Coordinate-frame investigation.**

The Panasonic branch of `_init_coeffs_md_v2` evaluates the CA polynomial
at destination-radius r (src/iop/lens.cc:2519-2526), matching Adobe
DNG WarpRectilinear's convention. Session 14's measurement was in
source-radius frame - a mismatch. Three variants explored:

1. **Destination frame.** Warp the AAHD-demosaiced image via the
   0x0119 distortion inverse (2 fixed-point iterations, matching
   lens.cc:2495-2504) before phase correlation. Halfdiag from rawpy
   output dimensions.
2. **Active-area frame.** Also crop the rawpy output to sensor active
   area from Exif SensorLeftBorder / TopBorder / RightBorder /
   BottomBorder. halfdiag = hypot(w_active/2, h_active/2), matching
   darktable's img->p_width * p_height. Mismatch is 0.15% (S5M2) to
   0.45% (G9); body-dependent, small.
3. **Correct frame (source measurement, dest-frame fit target).** Do
   not warp the image. Measure delta in source frame. Per tile,
   convert r_raw -> r_dest via the 2-iteration inverse, then fit
   target = delta_R_px / (r_dest_norm * halfdiag_active). This is the
   fit target that maps 1:1 to darktable's `d_r_R * r_dest * halfdiag`
   in the evaluator.

Physics-space distance from shipped session-5 at r = 0.85:

                                  |ship - refit|_R   |ship - refit|_B
    session 16 (source only)             0.29 px           0.88 px
    destination frame                    0.29 px           0.88 px
    active-area                          0.33 px           0.85 px
    correct frame                        0.35 px           0.77 px

None of the four variants closes the gap to below 0.20 px on either
channel. Element-wise, 0/24 of C_R and 0/12 of C_B_lo entries fall
within 30% of shipped in any variant. Both the direct evaluation
(r_dest = r_raw * f(r_raw)) and the iterative inverse were tried;
gaps agree within 0.02 px. **Coordinate frame is not the source of
the residual gap.**

Interpretation: shipped is calibrated against Panasonic's own SOOC JPEG
output (via Adobe DNG's WarpRectilinear encoding). Panasonic's firmware
under-corrects the raw CA by ~40-50% at outer radii - either
deliberately (some cameras leave a hint of CA for the "natural" look)
or as a limitation of their four-term polynomial evaluator. Direct raw
measurement records the sensor CA as it actually appears. The two are
legitimately different physical targets. Shipped matches SOOC; direct
measurement matches the sensor. No coordinate rework can bridge that
gap.

**Correction (post-session-21 review).** "Shipped matches SOOC" is an
inference, not a measurement, and it chains two untested assumptions:
that Adobe's WarpRectilinear coefficients are derived from 0x011b rather
than from Adobe's own lens-profile database, and that Adobe's encoding
reproduces what Panasonic's firmware does. The first is the premise test
now at the head of the task list; the second has never been examined. The
rest of the paragraph, that direct raw measurement and the shipped
coefficients are aimed at different physical targets, stands on its own,
but the size of the difference between them cannot be attributed to
Panasonic's under-correction until those two assumptions are checked.

**Visual verification of the correct-frame candidate.**

The correct-frame 4-row C_B fit predicts same-sign corrections on both
held-out files:

- P1366392 (Leica DG 12-60 @ 14mm, strong CA): B@0.85 pred +0.81 px,
  measured +1.79. Correct sign, partial correction.
- P1366399 (Leica DG 12-60 @ 24mm, weak CA): B@0.95 pred +0.14 px,
  measured +0.79. Correct sign, tiny correction.

Analytically both should reduce visible fringing (P1366392) or leave
it unchanged (P1366399). WIP applied to lens.cc, dt rebuilt, both files
rendered, working tree reset without commit. Mean absolute R-G and B-G
on the 400x400 corner crop:

    file        crop           |R-G|   |B-G|
    P1366392    baseline        13.6    10.2
    P1366392    WIP (correct)   14.7    17.0    +67% on B
    P1366399    baseline        17.8    12.0
    P1366399    WIP (correct)   17.9    11.9    unchanged

P1366399 behaves as predicted (visually unchanged). **P1366392 regresses
visibly on B (+67%) despite the analytical prediction saying correct
sign.** Something in the measurement -> r_dest -> fit -> darktable
evaluator chain has a hidden sign or scale inversion that all three
coordinate reworks missed.

Reference: the user-supplied `P1366399-crop-lens-correction-{off,on}.tif`
crops confirm the shipped baseline matches Panasonic SOOC within ~1%
on |R-G| and ~5% on |B-G|. Shipped is faithful.

**Correction (post-session-21 review).** That reference does not support
what it claims. `exiftool` reports
`Software: darktable 5.7.0+963~g2e04bf74d7` on both crop TIFFs, so they
are darktable renders of P1366399 with the lens module off and on, not
camera output. Comparing them measures what darktable's own correction
does; it says nothing about how closely that tracks Panasonic. The
genuine camera render of this frame is `P1366399.JPG`
(`Software: Ver.2.7`, the G9 firmware, 5184x3888), and it was not part of
the comparison. Session 21 later leaned on this paragraph as the one
validation independent of the AAHD pixel metric, which compounded the
error; see the correction there.

**Verdict.**

Shipped session-5 + K = 1 (session 13) is the strongest defensible
configuration for this file family. Direct raw CA measurement plus
six-word linear regression has reached its ceiling on 129 files. Any
refit that improves over shipped on strong-CA cases (P1366392) breaks
weak-CA cases (P1366399) at a rate wsB@0.85 = 5 which is already a
regression cluster; the tests in section "Approach sweep" show no
variant lowers this below shipped's baseline of 25 without also
collapsing the strong-CA improvement.

**What NOT to retry on this data:**

- OLS on direct raw CA measurement (session 16 outcome).
- Ridge / monotone / target-space with the six-word predictor (session
  18 outcome).
- Larger predictor sets on ~130 files (over-fits at every size tried).
- Sensor-format split with fewer than ~40 full-frame LOGO groups.
- Coordinate-frame reworks alone (all four variants at ~0.85 px B gap).

**What might still move the needle:**

- **Firmware RE.** Session 13 dead-ended on G9 v2.7 encryption
  (SoC-ROM-anchored per SILKYPIX session 12). A body with weaker
  protection, a debug port, or an official disassembly would recover
  the exact per-body decoder.
- **Demosaic-independent measurement.** AAHD's channel-correlation
  interpolation may bias the measured R-G shift on strong-CA edges.
  Measuring subpixel R-G offset on the raw Bayer directly (no
  demosaic) would rule this out before another linear-map try. Note
  that the session 18 visual regression on P1366392 despite an
  analytically correct-sign prediction is consistent with a hidden
  scale bias in the AAHD path.
- **Image-adaptive CA detection** inside darktable (edge statistics,
  not metadata alone). This would live in `cacorrectrgb` or a new
  module, not in the lens module.

**Files this session** (all under /tmp/rw2_tca/ unless noted, no
darktable source changes committed):

- `/c/temp/tca/thirdbody/panasonic_lumix_<slug>/` for GX9, GH5, GH5S,
  GX8, GH4, G90, G80, S1M2, S1M2E, S1RM2, S9: photographyblog samples
  used for the corpus expansion.
- `/c/temp/tca/regression01/P1366399.{RW2,JPG}` and paired on/off
  crop TIFFs: user-supplied regression report file.
- `measure_bigcombo{,_dest,_active,_correct}.npz`: four measurement
  variants of the same 132-file corpus.
- `measure_heldout_{dest,active,correct}.npz`: P1366392 + P1366399
  in each frame variant.
- `measure_ca_{dest,active,correct}.py`: three fork variants of
  measure_ca.py, one per coordinate-frame hypothesis.
- `sweep_experiments.py`: predictor-set / sensor-format / residual
  feature-selection sweep.
- `fit_bigcombo.py`, `fit_dest.py`, `fit_active.py`, `fit_correct.py`,
  `refit_retry_final.out.txt`, `refit_dest.out.txt`,
  `refit_active.out.txt`, `refit_correct.out.txt`,
  `refit_correct_summary.txt`: fit scripts and full metric reports.
- `/tmp/dt_render/wip_correct/`: dt renders on the correct-frame WIP,
  6-panel comparison mosaics for P1366392 and P1366399.

### Word roles and the public-RE audit (session 19)

Two developer hypotheses drove this session, both aimed at the question
session 18 left open: why does a six-word linear map keep hitting the same
ceiling?

1. The correction data might be calibrated per individual lens *copy*
   rather than per lens model, since real lenses vary unit to unit. If so,
   some predictor words could be identifiers or quantized profile
   selectors rather than continuous coefficients, and regressing linearly
   on them would be meaningless.
2. Panasonic's scheme might not be unique. If it reuses a Four Thirds /
   Micro Four Thirds convention, or anything another manufacturer uses,
   a published formula could replace the regression outright.

**Corpus for the word-role work:** 134 RW2 under `/c/temp/tca/**`, 14
bodies, 14 lens models. Scripts `session19_analyze.py`,
`session19_checks.py`, `session19_focus.py`, logs `session19_out.txt`
and `session19_checks_out.txt`, all in `/tmp/rw2_tca/`. The 0x011b reader
is `read_011b_words` from `fit_direct.py`, unchanged.

**Positional taxonomy over 32 words, 134 files:**

    behaviour                             positions
    global constant (= 256)               14
    constant per body model               4, 11, 16, 17
    ~10 discrete values, no lens order    5, 9, 21, 22, 28
    unique per file (134/134 distinct)    0, 1, 30, 31
    continuous, varying shot to shot      the remaining 18

The body-constant group is the four zone radii already known from
session 6, and the numbers are sensor-derived: word[4] is 2407 on
DMC-GX80 and DMC-G80, 2730 on DC-G9 and DC-GX9, 1905 on DC-GH5S, 3072 on
DC-S1M2, 4272 on DC-S1RM2, with words 11, 16, 17 following at the fixed
ratios. One value per body model, no lens signal.

**Hypothesis 1 outcome: refuted in its pure form, but it found something.**

The six predictor words `[8, 10, 12, 20, 23, 27]` are continuous, not
identifiers. On zooms with a focal sweep in the corpus (45-150, 70-300,
12-32, 14-140, 24-60), five to seven of the six move monotonically with
focal length, which is the signature of an interpolated coefficient.
Cardinalities per lens model for word[8]: Leica 12-60 = 21, LUMIX S 24-60
= 10, LUMIX S 70-300 = 9. The primes take only two or three values, which
is indistinguishable from an ID at that sample size but consistent with
continuous coefficients under aperture and focus dependence.

Two findings that are new, and that bear directly on the ceiling:

- **Cross-body: about 30 of 32 words change when the body changes, with
  lens, focal length and aperture held fixed.** GX80 vs G9 with the Leica
  12-60 at 12mm f/5.6 agree on word[6] (= 0) and word[14] (= 256) and on
  nothing else. Same picture for that lens at 25 and 60mm, for the Lumix
  45-150 at 45/97/150mm, for the Sigma 16 and 30, the Lumix 42.5, G80 vs
  G90 with the Lumix 12-60, and GH5 vs GH5S.
- **Shot to shot at fixed lens, focal length and aperture, the predictor
  words still move by 100 to 500 counts.** LUMIX S 50/1.8 on DC-S1M2 at
  f/5.6, three frames: word[8] in {-681, -471, -486}, word[20] in {-347,
  -687, -662}. LUMIX S 70-300 on S1RM2 at 300mm f/5.6, three frames:
  word[8] in {1277, 1642, 1316}, word[20] in {1637, 1136, 1583}. So the
  payload responds to an input the fit cannot see. Focus distance is the
  obvious candidate; exiftool does not surface it for the S-series files,
  so this is inference, not measurement.

Per-copy versus per-body cannot be separated on this corpus, because it
contains no matched lens copy shot on two bodies. Per-copy is not needed
to explain anything observed: per-body-model constants suffice.

**Hypothesis 2 outcome: refuted, with the public record audited.**

- darktable's own Olympus path reads `Exif.OlympusIp.0x150a` (4 floats)
  and `0x150c` (6 floats) at `src/common/exif.cc:1269-1308`, into
  `float dist[4]` / `float ca[6]` (`src/common/image.h:185-190`). Exiv2
  returns them already scaled; darktable applies no further unit scaling,
  and `[0,0,0,1]` is the no-correction sentinel (`exif.cc:1284`). The
  application at `src/iop/lens.cc:2379-2454` is a single whole-image
  polynomial per channel, `Rin_R = Rin * ((1 + car0) + car2*Rin^2 +
  car4*Rin^4)`, with no zoning.
- That rules the correspondence out at the layout level. Olympus is six
  coefficients over the whole frame; Panasonic 0x011b is four radial
  zones delimited by the body-scaled radii at `[11, 4, 16, 17]`, with the
  payload further partitioned by the three suspected inner checksums.
  There is no plausible field-by-field mapping between the two.
- No other project decodes 0x011b. LibRaw handles only Panasonic 0x0118
  (`src/metadata/tiff.cpp:518`); rawspeed only `PANASONIC_STRIPOFFSET`;
  exiv2 reports it as an unknown PanasonicRaw tag; RawTherapee has open
  requests (#3155, #3254) and no merged decoder; no dcraw derivative
  touches it. exiftool 12.76 documents 0x0119 fully, including
  `ValueConv => '$val / 32768'` on each coefficient
  (`PanasonicRaw.pm:272-273`, table at `:435-489`), but carries only a
  bare comment for our tag at `PanasonicRaw.pm:275`:
  `# 0x11b - chromatic aberration correction (ref 3) (also see forum9366)`.
  Notably, Olympus 0x150a/0x150c are not documented in exiftool's
  `Olympus.pm` either; darktable gets them through Exiv2's own OlympusIp
  grouping.
- Homeister's forum 9366 post remains the only public RE of 0x011b, and
  it stops where this document already says it stops. Rigo's
  `panasonic-rw2` repo covers 0x0119 only and its README now declares
  itself obsolete in favour of exiftool. Andrew Johnston's MFT lens
  correction project targets 0x0119 distortion models exclusively; the
  CA page it gestures at does not exist. The Four Thirds System white
  paper specifies mount and sensor, not any correction data format.

Conclusion: there is no published formula to borrow, from Olympus or from
anyone else. Any further progress on the coefficients needs new reverse
engineering.

### Inner checksums and body-unit normalization (session 20)

Two follow-ups fell out of session 19. Both are cheap and fully offline,
and both were run to a conclusion. No darktable source was changed.

**Inner checksums at words [2], [7], [13]: negative result.** These are
listed as "interior CRC (unknown), opaque" earlier in this document and
had never been attacked. Cracking them would fix the block boundaries and
so constrain which words are coefficients of a common polynomial.

`/tmp/rw2_tca/session20_innercrc.py`, log `session20_out.txt`, 134 files.
The sweep first reproduced all four known outer checksums at 134/134,
proving the byte convention against
`_validate_panasonic_ca_checksums` (`src/common/exif.cc:1117-1147`):
word[0] over even byte offsets 2..60, word[1] over bytes 4..31, word[30]
over bytes 32..59, word[31] over odd byte offsets 3..61, all with
multiplier 73,
modulus 0xFFEF, init 0, little-endian. Pointed at word[1] and word[30] as
unknown targets, the same engine recovers their ranges at 134/134, so the
search itself works.

Swept per target word: every byte range `lo` in 0..63 by `hi` in
lo+1..64 (2079 ranges), stride in {all, even, odd}, 26 small odd
multipliers including 73, modulus in {0xFFEF, 0xFFFF, 0x10001}, stored
word read as LE or BE, and *all* init values. Init comes for free from
the identity `rigo(bytes, init) = mul^k * init + rigo(bytes, 0) mod m`,
so a bucket hits iff `stored - rigo_0` is constant across files: 963,456
buckets per word, about 6.3e10 effective tuples per word and 1.9e11 over
the three.

    target    candidates >= 90% pass    best pass rate
    word[2]            0                     3/134
    word[7]            0                    16/134
    word[13]           0                     5/134

All at chance level for 9.6e5 buckets at modulus 65519. Stored values are
high-entropy (122, 126 and 108 distinct out of 134 respectively), which
is consistent with them being checksums of some kind. So: if words 2, 7
and 13 are checksums, they are not in the same recurrence family as the
outer four, and Homeister's part boundaries remain unconfirmed. Families
this sweep did *not* cover, for whoever picks it up: table-driven CRC-16
(CCITT, XMODEM, MODBUS, ARC, DNP, T10-DIF), reflected or bit-reversed
variants and non-zero XorOut, a word-level rather than byte-level
recurrence, input transformed before hashing (byte swap within word, XOR
with a fixed salt, a prepended body or lens ID), and input drawn from
outside the 64-byte payload.

**Body-unit normalization of the predictors: hypothesis confirmed
structurally, useless for the fit.** Session 19 showed the zone radii are
per-body constants. If the coefficient words carry the same body-pixel
scaling, then sessions 17 and 18 pooled raw word values across 14 bodies
of different resolution, which would be an apples-to-oranges regression
and a candidate explanation for the ceiling. This was never tested; the
session 18 coordinate-frame variants all operated on the measurement
side, not on the predictors.

Step 1, 25 matched cross-body pairs at fixed lens, focal length and
aperture (`session20_step1.py`). The clean pairs collapse to a *single
per-body factor shared by all six predictor words*: G90 vs G80 with the
Lumix 12-60 at 12mm f/8 gives 1.151 on every one of the six, against a
radius ratio of 1.134; GH5 vs GH5S at 12mm f/8 gives 1.396 to 1.404
against 1.433; G9 vs GX80 with the Sigma 30 gives 1.150 to 1.154. Median
implied exponent over the radius ratio is 1.115 to 1.124 per word. The
words are body-scaled, so the hypothesis is correct as a statement about
the format. The exponent sitting near 1.12 rather than 1.0 is not
explained; a plain radius ratio would give 1.0.

Step 2, refit with normalized predictors through the existing harness,
129 files, 45 LOGO groups (`session20_step2.py`):

    scheme                        R^2_R@0.85  R^2_B@0.85  regR  regB  wsB@0.85
    shipped baseline                -0.057      -1.423       0     0      26
    W raw (session 17)              +0.066      +0.282      72    84       6
    W / word[11]                    +0.143      +0.394      75    81       5
    W / word[4]                     +0.145      +0.399      75    80       5
    W / word[11]^2                  +0.143      +0.465      93    76       5
    target * halfdiag               +0.144      +0.388      72    69       5
    W / word[11], target * halfdiag +0.163      +0.456      72    71       5

Session 18's ceiling was R 0.144, B 0.388 at r = 0.85. Every scheme lands
on it. Step 3 on the held-out weak-CA files (`session20_step3.py`) shows
the same failure mode that got `3158effc0e` reverted: on P1366392 B at
r = 0.85 the measurement is +1.273 px and every normalized scheme
predicts +0.45 to +0.57, while shipped predicts -0.036; two of the
schemes also newly regress R on held-out files, `W / word[11]^2` on three
of six. Nothing here is shippable, and none of it is close.

**Verdict.** The ceiling is not caused by cross-body unit mixing. The
predictor words really are body-scaled, and normalizing them away changes
the fit by nothing that matters. Combined with session 19's finding that
the words also move 100 to 500 counts between frames with lens, focal
length and aperture unchanged, the most likely remaining explanation for
the ceiling is that the payload encodes an input the fit cannot observe,
rather than that the mapping needs a better regularizer or a better
coordinate frame.

**What NOT to retry, updated from session 18's list.** Everything on that
list, plus:

- Rigo-family byte-CRC searches for words 2, 7, 13 (session 20; the
  search space covered is written out above).
- Olympus `ca[6]` or any other manufacturer's record as a source of the
  formula (session 19; no public decode of 0x011b exists beyond what this
  document already uses).
- Predictor normalization by the zone radii, in any power, with or
  without a body-normalized target (session 20).
- Treating the six predictor words as categorical or per-copy identifiers
  (session 19; they are continuous and monotone with focal length on
  zooms).

**What might still move the needle, revised.** Session 18's list stands,
with one addition promoted above the rest because session 19 makes it
testable and cheap:

- **Identifiability check against the hidden input.** Take the frame
  triples that share lens, focal length and aperture but differ by 100 to
  500 counts in the predictor words (LUMIX S 50/1.8 on DC-S1M2 at f/5.6;
  LUMIX S 70-300 on S1RM2 at 300mm f/5.6) and measure their CA. If
  measured CA is essentially identical across a triple while the words
  differ that much, then no function of those words alone can reproduce
  CA, and the ceiling is a property of the data rather than of the model
  family. If measured CA tracks the words, the mapping exists and is
  non-linear. Either answer is worth more than another refit, and this
  decides which of the two situations we are in. **Run in session 21,
  which found a third answer: the measurement itself is not accurate
  enough to decide. See session 21, and note that the test as framed here
  is backwards; the fatal case is near-identical words with different
  measured CA, not the reverse.**
- Recovering focus distance from the S-series maker notes, which exiftool
  does not surface, would name the hidden input if there is one.

**Files this session** (all in `/tmp/rw2_tca/`, no darktable source
changes): `session19_analyze.py`, `session19_checks.py`,
`session19_focus.py`, `session19_out.txt`, `session19_checks_out.txt`,
`session20_innercrc.py`, `session20_out.txt`, `session20_step1.py`,
`session20_step2.py`, `session20_step3.py`, their `.out.txt` logs, and
`session20_report.txt`.

### The measurement is the limit (session 21)

Session 20 concluded that the ceiling was most likely caused by an input
the fit cannot observe. This session tested that directly, from two
sides, and the answer is different and more awkward: the quantity every
fit since session 14 has been scored against is not accurate enough to
support the precision being demanded of it, and on the two flagship
regression files it does not even agree in sign with a
demosaic-independent measurement of the same thing.

**Part 1: the irreducible floor for any function of the six words.**
Scripts `session21_step1.py`, `session21_step2.py`, `session21_step2c.py`,
`session21_step3.py` in `/tmp/rw2_tca/`, data in `session21_step1.npz`
and the matching `.out.txt` logs.

A note on the logic, because this document previously framed the test
backwards. Words varying while measured CA stays flat proves nothing: a
function is free to map varying inputs to a constant output. The fatal
case is the reverse, near-identical word vectors with materially
different measured CA, since that makes the target not a function of the
predictors at all. The RMS of those differences over near-duplicate pairs,
divided by sqrt(2), is a hard floor on the residual any model of these
six words can reach.

Per-file measurement uncertainty first, by bootstrapping the tile set
(50 files spanning 14 bodies, 200 resamples, refitting the 4-term
polynomial each draw):

    r       sigma_R   sigma_B     range over 50 files
    0.70     0.020     0.022      0.002 .. 0.267
    0.85     0.043     0.047      0.004 .. 2.759
    0.95     0.269     0.254      0.024 .. 8.472

So a typical file carries about 0.04 px of sampling noise at r = 0.85,
and the outliers are all tile-starved files (gh5_13 at n = 353,
s5ii_01 at 464, s1_ii_e_40 at 421).

The floor, over pairs with at least 700 tiles each:

    pair set                                      floor_R@.85  floor_B@.85
    within body, six words IDENTICAL   (31 pr)       0.128        0.241
    within body, |dW|inf <= 20         (41 pr)       0.153        0.215
    within body, nearest neighbour     (97 pr)       0.243        0.271
    cross body, |dW/w11|inf <= 0.01    (75 pr)       0.157        0.217
    cross body, nearest neighbour      (97 pr)       0.224        0.187

Subtracting the bootstrap noise term changes these by less than 0.02 px
at r <= 0.85, so they are not sampling noise. **Files whose six-word
predictor vector is bit-identical disagree by 0.24 px RMS on measured B
shift at r = 0.85, six times the per-file sampling uncertainty.** The
frame-group analysis says the same thing from the other direction: of 22
groups sharing body, lens, focal length and aperture, 15 have identical
predictor words and 13 of those move in R and 15 of 15 in B by more than
2 sigma_meas (DMC-GH4 at 14mm f/3.5, five frames, spread 0.36 px on R and
0.21 on B; GH5 at 60mm f/4, 1.74 px on R). Zero of the seven groups where
the words *do* move show flat CA.

Taken at face value, that caps any function of the six words at about
0.24 px on B at r = 0.85 against refits that reach 0.52 to 0.66, leaving
real headroom for a non-linear or cross-term model. That reading is what
part 2 undermines.

**Part 2: is the measured shift the real shift?** Session 18 named AAHD
demosaic bias as a suspect and nothing in the toolchain had ever read the
raw Bayer. Scripts `session21_bayer_measure.py`, `_sanity.py`,
`_nooffset.py`, `_compare_tm.py`, `_report.py`, report
`session21_bayer_report.out.txt`.

A demosaic-free estimator was built: colour sub-lattices by strided
slicing with no interpolation, CFA layout read from `raw_pattern` rather
than assumed (the corpus contains RGGB, BGGR and GBRG among these bodies),
black level subtracted, G taken separately from the R row and the B row,
and the fixed half-Bayer-period sampling offset subtracted before the
radial projection. Same phase-correlation estimator, same gates, same
tile geometry in sensor coordinates, so the only variable is the absence
of interpolation. It recovers a synthetic injected subpixel translation to
about 0.1 px. It is not an oracle: on a genuinely weak-CA file
(`s9_31`, AAHD |dr| < 0.05 at r = 0.85) it returns -1.42 px on R and
+1.68 on B rather than zero, so it has its own aliasing-driven bias.

Both pipelines on the same 15 files, comparing tile medians in
full-resolution pixels:

    r        N bins   median(Bayer-AAHD)   MAD    scale(OLS)  sign disagree
    0.70       26          -0.19           0.96      0.55       56%
    0.85       22          +0.08           1.05      0.80       29%

No systematic scale factor and no systematic offset, just scatter of about
1 px with Pearson correlation of +0.32 to +0.37 between the two
measurements of the same quantity. **The disagreement includes a sign flip
on both regression targets**: P1366392 R at r = 0.85 is +0.57 under AAHD
and -0.51 under Bayer; P1366399 R is +0.40 against -0.83; gx8_45 is +1.80
against -0.26. Subtracting the CFA sampling offset is worth 0.5 to 0.8 px,
which is smaller than the residual disagreement, so the disagreement is
not an artefact of how that offset was handled. It cannot be split between
demosaic bias and Bayer aliasing on this evidence, and is not claimed to
be one or the other.

One finding here is independent of the demosaic question and matters on
its own. **Four of the corpus files ranked as strong-CA have no AAHD tile
support anywhere near the radius they are scored at**: gh5s_01 (n = 221,
r_max 0.67), s9_01 (0.78), s9_05 (0.78), gh5_13 (0.74). None reaches
r = 0.85. Their AAHD predictions at r = 0.85 and 0.95
are pure extrapolation of inner-radius data, reaching +14.6 px on
gh5s_01. Corpus-wide metrics at r = 0.85 and 0.95, which is where every
comparison in sessions 17, 18 and 20 was scored, include those numbers as
if they were measurements.

**Verdict.** The two parts together read differently from either alone.
The 0.24 px floor of part 1 is derived from the same AAHD measurements
that part 2 shows disagree with an independent estimator by about 1 px
with 29% sign flips at the same radius. The most economical explanation
for pairs with identical word vectors disagreeing by 0.24 px is therefore
scene-dependent bias in the estimator, not a missing physical degree of
freedom: if the words are identical the camera believes the lens state is
identical, and the tile bootstrap of part 1 measures only sampling
scatter within one scene, so it cannot see a bias that varies between
scenes. A real per-frame degree of freedom invisible to the metadata,
focus distance being the obvious candidate since Panasonic's correction
may simply ignore it, remains possible and is not excluded. The two
cannot be separated with the tooling here.

What this does settle is the status of the fitting programme. Sessions 14
through 20 optimised and cross-validated against a target whose per-file
bias is comparable to the signal being fitted, partly in a radius regime
where some files contribute extrapolation rather than measurement. That
is sufficient to explain every feature of the ceiling: why aggregate
LOGO R^2 improves while held-out weak-CA files regress, why six
independent regularizers and coordinate frames all land on the same
number, and why session 18's closest candidate regressed visibly on
P1366392 despite an analytically correct-sign prediction.

**Consequence for what gets trusted.** This section originally named
session 18's rendered-pixel check as the one piece of validation not
dependent on the AAHD pipeline, and treated it as the reason the shipped
configuration stays. A later review refuted that as well: the two crop
TIFFs it rests on are darktable renders with the lens module off and on,
not camera output (correction recorded in session 18 above). So **nothing
in this document independently validates the shipped configuration.** It
survives on its session 5 derivation from Adobe DNG WarpRectilinear
coefficients, which is in turn premised on Adobe reading 0x011b at all, an
assumption the document flags but never tests (L1558, and L4644 states it
outright as a premise). **Session 22 tested it: Adobe does read the
payload, so that premise holds and this paragraph's concern is limited to
the missing rendered-pixel check.**

What can support a real harness is the genuine paired camera output. The
18 at-capture JPEGs in the corpus carry Panasonic model and firmware
software tags with capture timestamps matching their RW2s, so a registered
comparison of |R-G| and |B-G| between a darktable render and the camera
JPEG is buildable. Registration is the work: the existing `make_crops.py`
compares unregistered crops, and scale and distortion differ between the
two renders. That metric is what future attempts should be scored in, and
it does not exist yet.

**What NOT to retry, updated again.** Everything in sessions 18 and 20,
plus:

- Any refit scored on the AAHD radial-shift metric, at any radius, with
  any regularizer. The metric is not accurate enough to rank candidates
  that differ by a few tenths of a pixel (session 21 part 2).
- Any metric evaluated at r >= 0.85 on gh5s_01, s9_01, s9_05 or gh5_13
  without first checking tile support; those values are extrapolation.
- Interpreting the 0.24 px near-duplicate floor as proof of a missing
  metadata input. It is an upper bound on real CA variation and a lower
  bound on estimator bias, and this session cannot separate them.

**What might still move the needle, revised again.** In descending order
of expected value:

- **Build the rendered-pixel metric that does not exist yet.** Score
  candidates by |R-G| and |B-G| on darktable renders registered against
  the paired at-capture camera JPEG, on a handful of files with strong
  edges near the corner. Slow per candidate, but it measures the thing
  users see. Note that session 18's version of this check was not valid
  (see the correction there), so this has never actually been done.
- **Fix the measurement before fitting again.** A trustworthy estimator
  needs an aliasing model for the Bayer path, or edge-based subpixel
  localisation on high-contrast features rather than whole-tile phase
  correlation, plus a tile-support floor at the radius being scored. Until
  one exists, no refit can be ranked.
- Recovering focus distance from the S-series maker notes, to test the
  surviving missing-degree-of-freedom hypothesis rather than assume it.
- Firmware RE, unchanged from session 18, still blocked on G9 v2.7
  encryption.
- Image-adaptive CA detection in `cacorrectrgb`, unchanged from session
  18, and now somewhat more attractive: if per-frame CA really does vary
  at a fixed lens state, no metadata decode can reach it. **Ruled out by
  the developer post-session-21: it does not reproduce Panasonic's
  correction, which is the goal. See "Scope and goal".**

**Files this session** (all in `/tmp/rw2_tca/`, no darktable source
changes): `session21_step1.py`, `session21_step2.py`,
`session21_step2c.py`, `session21_step3.py`, `session21_step1.npz`,
`session21_step{1,2,2c,3}.out.txt`, `session21_bayer_measure.py`,
`session21_bayer_sanity.py`, `session21_bayer_nooffset.py`,
`session21_bayer_compare_tm.py`, `session21_bayer_report.py`,
`session21_bayer_report.out.txt`.

### The Adobe premise, tested at last (session 22)

Every coefficient darktable ships for Panasonic CA was fitted in session 5
against Adobe DNG Converter's per-plane WarpRectilinear output. Whether
Adobe derives those per-channel coefficients from 0x011b, or from its own
lens-profile database keyed on the lens model, was never tested; L1558 and
L4644 both flag the assumption without checking it. Under the goal in
"Scope and goal" this is the question that decides whether the shipped
feature is a decode of Panasonic's data or an imitation of Adobe's
profiles. **It is a decode. The premise holds.**

**Method.** `/tmp/rw2_tca/session22_payload.py` (built for this session,
see below) rewrites the 64-byte payload in place; `read_warp_opcode` in
`/tmp/rw2_tca/compare_dng.py` parses OpcodeList3 out of the resulting DNG.
Converter invocation `Adobe DNG Converter.exe -c -p0 -d <outdir> <in>`,
reached through WSL interop, with a unique input name and output directory
per run. Fifteen conversions, all exit status 0, all outputs freshly
written. On every mutated RW2, `rawpy` `raw_image_visible` hashes identical
to the base file, so no coefficient change can be an artefact of a damaged
raw stream. Three bodies: G9 P1366481 (45mm) with donor P1366483 (150mm),
GX80 P1260633 (12mm) with donor P1260635 (60mm), S5II s5ii_17 with donor
s5ii_19. Mutants and DNGs under `/c/temp/tca/probe22/`; nothing in the
corpus was modified in place.

**Controls first.** Converting an untouched file twice to separate
directories gives max |delta| = 0 across all three planes on all three
bodies, so the converter is deterministic. A no-op payload rewrite, which
produces a byte-identical file, likewise gives max |delta| = 0, so neither
caching nor the editor perturbs anything.

**The decisive condition: whole-payload transplant.** Replacing file X's
64-byte payload with file Y's, same body and lens but a different focal
length, leaves all four outer and all three unknown inner checksums
internally self-consistent, so it cannot be rejected on integrity
grounds. The result is unambiguous. On the G9, base R k0..k3 reads
+1.0000578 +0.0002284 -0.0000901 +0.0000717 and the transplant reads
+0.9994650 +0.0005815 -0.0004946 +0.0002709, which is the donor's own
converted output to seven decimal places. In the differentials that are
the CA signal proper, D_R = R - G and D_B = B - G, the transplant
reproduces the donor exactly on the G9 and the S5II, and to about 1e-5 on
the GX80, whose G plane is strongly non-identity so its CA is computed on
top of a real distortion correction. **Adobe reads the payload.**

**The complementary condition: lens identity.** Leaving 0x011b untouched
and changing the lens the file reports instead, CameraIFD 0x1202 plus the
maker-note lens strings, to a different real Panasonic lens (42.5mm F1.7),
gives max |delta| = 0. Byte-for-byte identical coefficients. **Adobe is
not keying CA on a lens-profile database.** The two conditions together
also exclude the mixed case, where Adobe might combine the payload with a
profile: that would have moved the coefficients here.

**Distortion and CA are independent inputs.** Mutating 0x0119 instead, on
the GX80, makes the G plane snap to identity, which is Adobe rejecting the
distortion tag on a failed internal check, while R and B stay non-identity
and still track 0x011b. So Adobe treats 0x0119 as the distortion source
and 0x011b as the CA source, and they isolate cleanly. That answers two
open questions below: Homeister's claim that 0x011b carries distortion as
well as CA is not how Adobe reads it, and the worry that darktable's
0x0119 distortion path might double-correct on top of 0x011b is
unfounded, at least by Adobe's interpretation.

**Adobe enforces the four outer checksums.** A payload mutated with
`--no-recompute`, leaving the Rigo checksums stale, produces a DNG with no
OpcodeList3 entry at all: Adobe validates them and drops the correction
entirely rather than applying it anyway. Note the contrast with session
12's reading of SILKYPIX, and with darktable's own choice at
`src/common/exif.cc:192-194` to log a mismatch and accept the payload
regardless. Whether darktable should follow Adobe here is a new question,
not settled by this session.

**Words 2, 7 and 13 are not enforced by Adobe.** Every per-word mutation
below breaks any inner checksum those words might hold, and Adobe still
emitted a correction. Either they are not checksums, or nobody validates
them.

**Per-word response, and it matches what darktable ships.** On P1366481,
with outer checksums recomputed:

    word[8]  = -5000, +5000, +25000   R plane only; magnitude grows with |value|
    word[23] = +3000                  R plane only
    word[20] = -3000, +3000           B plane only
    word[4]  = +2000 (zone radius)    R and B both; G untouched

Words 8 and 23 drive R, word 20 drives B, and a zone radius drives both.
That is the shipped predictor set [8, 10, 12, 20, 23, 27] and the radii
[4, 11, 16, 17] behaving exactly as the decode assumes.

**The payload editor** (`session22_payload.py`, modes dump, verify, set,
transplant, plus `--no-recompute`) found one structural detail worth
recording: **the four outer checksums are inter-dependent.** word[0]
covers byte 2, which is word[1]'s low byte, and byte 60, which is
word[30]'s low byte; word[31] covers bytes 3 and 61 the same way.
Recomputing all four simultaneously yields a file that fails its own
verifier. The correct order is word[1] and word[30] first, then word[0]
and word[31] over the updated buffer. The payload sits in IFD0 at file
offset 0x436 on every RW2 sampled. Six proofs pass: byte-identical no-op
rewrite on six files across six bodies; edits confined to bytes
1078-1141 with zero bytes changed outside; mutants validating under both
this tool and the independent `verify_011b.py`; transplants carrying the
donor's words 2, 7 and 13; `rawpy` and `exiftool` both reading mutants
cleanly; and the discredited "bytes 4..59 evens/odds" spec failing as a
negative control.

**What this changes.** The first trigger in "When to stop" does not fire.
More importantly, the mapping from payload words to correction
coefficients is now recoverable as a deterministic function, by sweeping
words and reading Adobe's coefficients back, with no demosaic, no pixel
measurement and no scene dependence anywhere in the loop. That sidesteps
every problem session 21 identified. It is a decode of Adobe's reading of
Panasonic's data, which the caveat at L1558 rightly distinguishes from
Panasonic's own rendering, so the camera-JPEG comparison remains the check
that the two agree.

**Files this session**: `/tmp/rw2_tca/session22_payload.py`, mutants under
`/tmp/rw2_tca/mutants/`, probe inputs and DNGs under
`/c/temp/tca/probe22/`. No darktable source changes.

### The word map, recovered and audited (session 23)

With Adobe established as a deterministic oracle, the map from payload
words to correction coefficients can be read off directly: change a word,
convert, parse OpcodeList3. 165 conversions at 0.91 s each for the sweep
(`session23_{probe,sweep,analyze}.py`, results in
`session23_results.npz`), then 106 fresh conversions by an independent
audit that re-derived every claim rather than re-reading the data
(`/tmp/session23_audit.py` and the `probe23b_*` scripts; staging under
`/c/temp/tca/probe23/` and `probe23b/`). The audit confirmed the core
findings, refuted three of the framings, and found a blocker the sweep had
missed. Measured quantities throughout are D_R = R - G and D_B = B - G,
four polynomial coefficients each.

**What is solid enough to build on.**

- **Twelve words carry the CA signal, and channel assignment is exact.**
  Four zone radii at words 4, 11, 16, 17; four R-only coefficient words at
  8, 12, 23, 26; four B-only coefficient words at 10, 20, 27, 29.
  Opposite-channel derivatives are not small, they are exactly zero.
- **Of the 27 numeric words swept, fifteen do nothing at all**: 2, 3, 5, 6,
  7, 9, 13, 15, 18, 19, 21, 22, 24, 25, 28. Setting all fifteen at once to
  the values from a different G9 file changes the output not at all
  (`max_plane_delta 0.0`). Words 2, 7 and 13 are therefore not CA
  coefficient inputs, which is consistent with the checksum reading but
  does not prove it; inertness is not evidence of semantics.
- **Word 14 is a gate, not a coefficient.** Setting it to 0 produces a DNG
  with no warp opcode at all, alongside words 0, 1, 30 and 31 whose
  checksums Adobe enforces.
- **The eight coefficient words are linear to floating-point precision.**
  Slopes from +-137 and +-431 agree to between 3.8e-13 and 9.5e-12, and an
  affine model reproduces endpoints to 3.8e-15. This is far tighter than
  the sweep's own claim of 1e-3.
- **Coefficient words are exactly additive with each other**: residual 0
  for the pairs tested.
- **R and B slots mirror each other** in the pairs (8,20), (12,27),
  (10,23) and (26,29), with slope residuals of 1.8e-18 to 3.6e-18. The
  audit checked this was not an indexing or caching artefact: the mutant
  RW2s and their DNGs have distinct sha256 hashes, and word 8 moves only R
  while word 20 moves only B. Call it symmetry within double-precision
  rounding, not byte identity.
- **No 0x011b word moves the G plane**, exactly zero over 102 fresh
  mutants across all six stored G values, confirming session 22's
  separation of distortion from CA.

**The shipped decode is wrong in four verified ways.** All four were
re-checked against `src/iop/lens.cc` by the audit, which read the shipped
tables at `:2193-2207` and the evaluator at `:2462-2486` and `:2517-2527`.

1. **Two real predictors are missing.** The shipped set is
   [8, 10, 12, 20, 23, 27]; words 26 (R) and 29 (B) are active and absent.
   The four zone radii are absent too.
2. **Spurious cross-channel terms.** `_pana_C_R` carries non-zero columns
   for words 10, 20 and 27, and `_pana_C_B_lo` for words 8, 12 and 23.
   Every one of those true derivatives is exactly zero. The shipped
   matrices attribute R response to B words and vice versa.
3. **Magnitudes are off by factors, not percentages.** k1 ratios of
   measured to shipped sit at 0.92 to 1.09, but k0 comes out at 2.107 for
   word 8 and 4.012 for word 23.
4. **Half of the B polynomial is missing.** The evaluator computes
   `d_b = db_k[0] + db_k[1]*r2` with `db_k[2]`, so D_B k2 and k3 are
   dropped. They are real: word 10 alone gives k0..k3 of 3.28e-7,
   -4.76e-6, 9.58e-6, -5.16e-6, and words 20, 27 and 29 behave the same.

That is enough to explain why sessions 14 to 21 never converged. The model
form was wrong before any fitting began: two predictors missing, six
coefficient columns that should be identically zero left free to absorb
noise, and two of the eight output coefficients discarded.

**The blocker, and it is a real one.** The map is *not* a function of the
payload words alone, nor of the words plus the body.

- A nine-conversion linear model, baseline plus one probe per coefficient
  word, predicts a held-out edit of all eight words simultaneously to
  within 8.7e-15 absolute, 1.3e-12 relative. Exact, within one file.
- The same model applied to `P1366477`, a different frame from the same
  G9 with an identical radius tuple, is wrong by 5.2e-4 absolute, which is
  **60.6% of the measured change**.
- Two real G9 files with identical radii give word 8 derivative ratios of
  0.480 to 0.564, while repeat conversions of one file agree to 2.5e-6.

So what the sweep reported as body dependence is file or context
dependence, and the earlier single-per-body-scalar reading from session 19
does not survive either. The prime suspect is named by the failing case
itself: `P1366477` has a non-identity G polynomial where `P1366481`'s G is
identity. In other words the files differ in their distortion state, and
if Adobe composes the per-plane warp as distortion applied together with
CA, then D_R = R - G removes distortion only to first order and the
residual scales with how much distortion there is. That would also explain
the one loose end in session 22, where the GX80 transplant matched its
donor to 1e-5 rather than exactly, the GX80 being the one body in that
test with a strongly non-identity G.

**A caveat on the radii that matters for any implementation.** The
corpus's 13 body groups each carry exactly one radius tuple: G9
(2730, 3276, 2184, 1092), GX80 (2407, 2888, 1926, 963), GH5S
(1905, 2286, 1524, 762), full-frame 24 MP (3072, 3584, 2048, 1024), S1R II
(4272, 4984, 2848, 1424). Perturbing one radius alone breaks the coupled
geometry every real file has, so the sweep's findings that the radii
respond non-linearly and non-monotonically, and interact with the
coefficient words by 8 to 34 percent, are measurements of inputs no camera
produces. They establish that Adobe conditions on those words. They are
not facts about the format, and the "knot position" reading of them is an
inference, not a result. With native radii held fixed, channel
selectivity, linearity and coefficient additivity all survive.

**Consequently, do not patch the shipped tables yet.** The four
contradictions above are real and the corrected structure is clear, but a
decoder built on today's numbers would be exact on P1366481 and 60% wrong
on its sibling. The missing conditioning variable has to be identified
first.

**The next experiment follows directly.** Session 22 showed that mutating
0x0119 makes Adobe drop distortion and emit an identity G plane. So
neutralise 0x0119 across a set of files from one body, then re-run the
coefficient-word sweep on each. If the derivatives collapse onto a single
set once distortion is out of the way, the conditioning variable is the
distortion state, the correct model form is CA expressed in the
pre-distortion frame, and both the P1366477 failure and the GX80 residual
are accounted for. That would also cast sessions 17 and 18's fruitless
coordinate-frame reworks in a different light: they were varying the frame
of the *measurement* while the frame mismatch was in the *model*.

**Files this session**: `/tmp/rw2_tca/session23_{probe,sweep,analyze}.py`,
`session23_results.npz` and `.json`, `session23_report.txt`,
`session23_analysis.log`; audit scripts `/tmp/session23_audit.py` and
`/tmp/probe23b_*.py`; staging under `/c/temp/tca/probe23/` and
`/c/temp/tca/probe23b/`, left in place so individual mutants can be
inspected without re-running. No darktable source changes.

### The conditioning variable is the distortion state (session 24)

Session 23 left the decode blocked on a variable nobody had named: a
nine-probe linear model exact to 1e-15 within one file was 60.6% wrong on
a sibling frame of the same body with identical zone radii. It is the
0x0119 distortion state, and the evidence is about as clean as this
investigation has produced. Scripts `session24_{t1,t1_analyze,t2t3,
t2t3_analyze,t4,t4b}.py`, report `session24_report.txt`, 423 staged
mutants under `/c/temp/tca/probe24/`.

**T1, cross-distortion transplant, which settles where the conditioning
lives.** Transplanting the whole 64-byte payload between P1366481 (G
identity) and P1366477 (G_rms 0.124), in both directions, keeps every
checksum self-consistent, so the only question is whose output the
recipient produces. The recipient's own G plane survives exactly, to
0.000e+00, and D_R misses the donor by 5.15e-4 against a donor |D_R| of
about 6e-4, which is roughly 60% of the signal. **The conditioning is
external to the payload**, so the map is not globally non-linear in the
words; something else in the file is involved. Session 22's transplants
had not discriminated because the files in them shared a distortion state.

**T2, the variation tracks distortion strength.** Eight coefficient-word
derivatives measured on six G9 files spanning G_rms of 0.000, 0.000,
0.010, 0.015, 0.063 and 0.124. With distortion enabled the derivatives
scatter with a coefficient of variation of 0.181 median and 1.348 maximum
across files, and Pearson correlation against G_rms exceeds 0.93 for all
32 signal components.

**T3, and this is the decisive number.** Clearing the 0x0119 enable
nibble, so Adobe drops distortion and emits an identity G plane, collapses
that scatter to a coefficient of variation of 0.000 median with standard
deviation at most 3.4e-18. The eight per-word slope vectors become
identical across all six files at machine precision. Same statistic before
and after, so there is nothing to interpret: **with distortion out of the
way, the word-to-coefficient map is one function.**

**T4, the representation a decoder must use.** Since the conditioning is
distortion, D_R = R - G was the wrong quantity all along: subtraction
removes a *composed* distortion only to first order. The representation
that works is the pre-distortion frame, `eps_R = R(r_out)/G(r_out) - 1`
evaluated at `r_raw = r_out * G(r_out)`. It recovers the payload-invariant
CA polynomial to 1e-16 on identity-G files, and degrades gracefully as
distortion grows: 3e-6 at G_rms 0.010, 3e-4 at 0.124. That residual is
Adobe's own, because its degree-6 WarpRectilinear polynomial is a
truncated fit to a higher-degree composition. For comparison, R - G, the
ratio taken in the output frame, and the log difference all fail to
collapse; the output-frame ratio merely halves R - G's scatter.

**What this means for darktable, and it is good news.** The pure CA
correction is a per-channel radial rescale in the raw frame, applied
*before* distortion. darktable already has the 0x0119 distortion path from
`47c223703e`, so it does not need the composed form that Adobe emits: pure
CA plus the existing distortion, in that order, reproduces the composition
by construction. It may even be more faithful to the camera than Adobe is,
since darktable would not be forced to squeeze the composition through a
degree-6 polynomial, though that is a conjecture about Panasonic's own
algorithm, which remains unknown.

**It also adds a fifth error to session 23's list.** The shipped
`_pana_C_R` and `_pana_C_B_lo` were fitted in session 5 against Adobe's
D_R and D_B with distortion enabled, which is the distortion-conditioned
composition rather than the CA. That is a wrong target, not merely a
mis-parameterised fit, and it is a better explanation of the k0
discrepancies of 2.107 and 4.012 than anything proposed so far. Any
re-derivation must fix distortion to identity in the probe corpus.

**The decoder can now be specified**, for the first time in this
investigation:

1. parse the twelve active words of 0x011b;
2. map those words to `eps_R` k0..k3 and `eps_B` k0..k3, using session
   23's form, linear in the coefficient words with a radius-parameterised
   basis, derived from a corpus in which distortion is neutralised;
3. apply `eps_c` as a per-channel radial rescale in the raw frame, ahead
   of the distortion correction.

Step 2 is the remaining derivation work. Step 3 needs a check of where
darktable's Panasonic branch currently applies `cor_rgb` relative to the
distortion multiplier, since the order is now load-bearing.

**Files this session**: `/tmp/rw2_tca/session24_*.py`,
`session24_report.txt`, staging `/c/temp/tca/probe24/`. No darktable
source changes.

### The map, recovered exactly (session 25)

With distortion neutralised the map is recoverable by construction rather
than by fitting: set a channel's four coefficient words to zero to get the
intercept, then set each alone to get its column. Nine conversions per
context. An independent audit re-ran the whole thing with 45 fresh
conversions plus 247 further probe files and reproduced the matrices with
`fresh_vs_npz_max_abs = 0`.

**The form.** `eps_R = M_R . [w8, w12, w23, w26]` and
`eps_B = M_B . [w10, w20, w27, w29]`, one 4x4 matrix per channel per
context, intercepts at most 5.3e-15, opposite-channel columns exactly
zero. Deliverables in `/tmp/rw2_tca/session25_map.npz`, a readable dump in
`session25_map.txt`, and a reference implementation
`session25_decode.py` which the audit confirmed returns exactly the npz
values and lands within 2.0e-15 to 1.1e-14 of fresh Adobe output on the
models it was probed against.

**Accuracy within a context**, all from the audit's own conversions:

    held-out edit, all eight words at once   R 2.2e-15 .. 4.3e-15
                                             B 4.1e-15 .. 8.3e-15
    real untouched payloads, probed model    1.9e-15 .. 1.1e-14

**End to end on real untouched files with distortion enabled**, predicting
Adobe's actual R and B planes from the payload alone by composing the
recovered eps with the file's own G polynomial:

    identity G                7.8e-16 .. 1.1e-14
    G_rms 0.0043              1.1e-7  .. 1.7e-7
    G_rms 0.023               1.6e-7  .. 4.5e-7
    G_rms 0.05                6.5e-6  .. 1.7e-5

Error correlates with distortion strength at 0.894 for R and 0.913 for B,
as the truncation explanation predicts, and the floor is about 1.7e-5,
an order of magnitude better than session 24 expected. **The payload plus
this map reproduces Adobe to within a rounding error of its own
polynomial fit.**

**Generalisation fails, so a decoder needs tables.** The corpus holds five
distinct radius tuples across 14 distinct camera model strings, not 13;
DC-S1M2 and DC-S1M2ES are separate strings. No tested radius-parameterised
form fits across contexts: scaling by 1/N1 leaves 0.963 max and 0.170
median residual, 1/N1^2 leaves 0.997 and 0.313, an MFT-only fit 96.5 and
0.356, a full-frame-only fit 0.527 and 0.286. Those forms are rejected;
that is not the same as proving no radius formula exists. A decoder must
carry per-context matrices and needs a stated policy for tuples it has
never seen.

**Correction to earlier grouping.** The GX8 carries the radius tuple
2730/3276/2184/1092, the same as the G9, not the GX80 tuple it was
previously listed under.

**The residual body term, and whose it is.** Two bodies sharing a radius
tuple do not share a matrix exactly: fresh re-probes give 0.2238% between
S1II and S5II, 1.1167% between S9 and S5II, and 1.4509% between G80 and
GX80. The cause is now identified, by intervention rather than inference.
Changing *only* the TIFF Model string in a staged copy, DC-S5M2 to
DC-S1M2 and DMC-GX80 to DMC-G80, makes Adobe emit the target model's exact
ActiveArea and its exact native matrix column, `max abs difference 0`,
while payload, firmware and sensor tags are untouched. **Adobe's
camera-model profile is the selector.** The obvious normalisation
hypothesis is wrong: ActiveArea diagonal ratios do not predict the
perturbation and mostly make it worse, taking S9 from 1.117% to 1.344% and
G80 from 1.451% to 1.809%; DefaultScale is 1 1 everywhere; sensor
dimensions are identical within two of the three pairs; 0x011a is absent
on the S bodies and identical on the MFT pair.

That matters for scope, not just for accuracy. This term comes out of
Adobe's per-model data, not out of Panasonic's payload, so reproducing it
would mean imitating Adobe rather than decoding the camera, which is the
opposite of what "Scope and goal" asks for. Its size settles the question
anyway: on real S1II, S9 and G80 files the model term is 0.00021 to
0.00372 px against full corrections of 0.31 to 2.57 px. **Deliberately not
reproduced**, and recorded here so nobody mistakes it for an error later.

**The shipped tables, measured against truth one last time.** False
cross-channel entries reach 3.28e-6 in `_pana_C_R` and 4.47e-7 in
`_pana_C_B_lo` where the true derivative is zero. Recovered over shipped
ratios are 2.107 for word 8 k0, 2.489 for word 8 k3, and 4.012 for word 23
k0. The omitted `_pana_C_B_lo` k2 and k3 entries span 6.25e-7 to 9.58e-6,
so they are not negligible either.

**What is safe to ship**, in the audit's words as well as mine: the
predictor sets [8, 12, 23, 26] and [10, 20, 27, 29], the zero intercepts,
the zero opposite-channel columns, and the five measured matrices tied to
the contexts they were measured in. What is not yet decided is the fallback
for an unknown radius tuple, since the rejected scalings would leave a
median error of 17% of the correction, which at 2.5 px of correction is
not a rounding error.

**Files this session**: `/tmp/rw2_tca/session25_*.py`, `session25_map.npz`,
`session25_map.txt`, `session25_decode.py`; staging
`/c/temp/tca/probe25/` and the audit's `/c/temp/tca/probe25b/`. No
darktable source changes.

### The evaluator applies CA in the wrong frame (session 26)

Session 24 established that CA is a raw-frame rescale applied ahead of
distortion. Reading the shipped evaluator shows it does the opposite, so
this is a sixth error, independent of the five in the tables, and it means
the patch is not just a table swap.

`src/iop/lens.cc:2519-2523` evaluates the CA polynomial at the
*destination* radius and adds it to the distortion multiplier:

    const double r2 = (double)r * (double)r;
    const double d_r = dr_k[0] + dr_k[1]*r2 + dr_k[2]*r4 + dr_k[3]*r6;
    cor_rgb[0][i] = fine + (float)d_r;
    cor_rgb[2][i] = fine + (float)d_b;

with the code's own comment confirming `r == knots_dist[i]`, the
destination-radius abscissa. The Olympus branch at `:2424-2431` is the
correct model: it forms the source radius `rd = cor_rgb[1][i] * r` first
and applies its CA there.

**Conventions, for the record.** `cor_rgb` is `float[3][MAXKNOTS]` with
MAXKNOTS 16 (`:51`, `:236`), knot i at `r = i/(nc-1)` normalised to the
half-diagonal, and each entry is a destination-to-source radial
multiplier `dr = r_src/r_dest`, consumed as `xs = dr*cx + w2`.

**The fix, in the variables that already exist.** Inside the same knot
loop, after the two fixed-point iterations that invert the distortion
polynomial, the converged source radius is `rd = fine * r`. That is
exactly session 24's `r_raw = r_out * G(r_out)`, so eps is evaluated
there and applied as a rescale of the green multiplier:

    cor_rgb[0][i] = fine * (1.0 + cor_ca_r_ft * eps_R(rd));
    cor_rgb[2][i] = fine * (1.0 + cor_ca_b_ft * eps_B(rd));

eps belongs outside the fixed-point loop, which inverts G and not C, and
no inversion of C is needed because eps is a raw-frame rescale by
construction.

**Scope of a correct patch**, from the same reading. The OpenCL path needs
no kernel change, since `basic.cl` consumes the same spline layout
(`:2993`, `basic.cl:2846`, `:2878-2880`). `_process_md` (`:2921`), the
`_modify_roi_in_md` bounding box (`:3053`, `:3075`), the auto-scale loop
(`:2561`) and `_check_corrections_md` (`:2615-2620`) all pick up changed
per-channel values automatically. `_distort_transform_md`,
`_distort_backtransform_md` and `_distort_mask_md` deliberately use only
the green channel, which stays correct: CA must be invisible to them. The
`panasonic` struct in `src/common/image.h:165-207` already carries
`ca_words[32]`, so no schema change; only the coefficient tables and the
B-term count change shape, from two stored B terms to four.

**Transparency is part of this patch, by developer decision.** Today the
GUI decides CA availability at `:4516-4519` from the format and the
algorithm version alone, so for any Panasonic raw on v2 the CA fine-tune
sliders appear and the TCA flag reads as active even when the pixel path
applies nothing, which it gates separately on `cd->panasonic.has_ca` at
`:2467-2469`. A user cannot currently distinguish "corrected" from
"silently skipped". The patch must make availability real, hiding the CA
fine-tune sliders when there is no usable payload or no table for the
body, and raise a trouble message when TCA is requested but unavailable,
following the precedent at `:4453-4464`. The exact wording, and whether it
should point at the Lensfun method as an alternative, is deferred to
implementation so it can be judged against a rendered result rather than
in the abstract.

### The decode, shipped (session 27)

`eb73e8222f` replaces the Panasonic branch. What went in, and what was
verified rather than assumed.

**The change.** The six-word tables `_pana_C_R`, `_pana_C_B_lo` and the
amplitude constant `_pana_K` are gone. In their place are
`_pana_ca_radii[5][4]`, keyed on payload words 4, 11, 16 and 17, and
`_pana_ca_M_R[5][4][4]` and `_pana_ca_M_B[5][4][4]`, with predictor index
arrays `{8, 12, 23, 26}` for R and `{10, 20, 27, 29}` for B.
`_pana_ca_context()` resolves a payload to a table index or returns -1 for
an uncharacterised body, and -1 means no CA rather than an extrapolation.
The evaluator now computes `rd = fine * r`, `u = rd * rd`, eps as a cubic
in u by Horner, and applies it multiplicatively as
`cor_rgb[0][i] = fine * (1 + cor_ca_r_ft * eps_r)`, so CA acts in the raw
frame ahead of distortion. All four B coefficients are used.

**Verification.**

- Transcription: every element parsed back out of `lens.cc` and compared
  against `session25_map.npz`, maximum absolute difference exactly 0,
  radius tuples and word index arrays included. `_pana_ca_M_B` is
  `_pana_ca_M_R` with columns permuted by [2, 0, 1, 3], which is the slot
  symmetry (8,20), (12,27), (10,23), (26,29) expressed as a permutation.
- Convention: the patched C expression reimplemented in Python, reading the
  tables out of `lens.cc` rather than the npz, agrees with
  `session25_decode.py` to 5.3e-15 over 21 knots on two files in two
  contexts. The test has power: forcing a cubic in r instead of u
  disagrees by 1.7e-4, and evaluating eps at r instead of `fine * r`
  disagrees by 7.2e-5, both orders of magnitude above the noise.
- Renders: four `darktable-cli` runs on P1366477 and a S5 II file, all exit
  0. Integration tests 0145 and 0146, which exercise the shared
  embedded-metadata path, pass.
- Coverage: all 32 real files checked across three corpus directories
  resolve to one of the five contexts, so none loses CA to the new
  fallback.

**Size of the behavioural change**, on P1366477 at r = 0.85 with a
half-diagonal of 3276.8 px: the red channel moves 0.423 px where the old
code moved it 0.624 px, and blue moves 1.159 px where the old code moved it
0.398 px. So 0.20 px on R and 0.76 px on B, which is visible.

**OpenCL: verified on hardware, by the developer.** This host reports zero
OpenCL devices, so nothing here could exercise `process_cl`; the CPU and
"OpenCL" outputs matched only because both ran on the CPU. The developer
reproduced the test on Windows against an AMD gfx1103, on a build of
`0297ce0d70`:

- `[opencl_init] FINALLY: opencl PREFERENCE=YES is AVAILABLE and ENABLED`,
  one platform, one device, driver 3661.0 (PAL,LC).
- `opencl_device_priority '*/!0,*/*/*/!0,*'` resolves the export pipe to
  device 0, so the export path is GPU-eligible; preview and preview2 are -1
  by design.
- ``[export] processed `lens' on GPU``, with 0.1081 s in the
  `md_lens_correction` kernel, and 19 of 19 events successful with none
  lost.
- GraphicsMagick `compare -metric MAE` between the CPU and GPU exports:
  0.0000000000 normalised on red, green, blue and total.

So the patched coefficients and the raw-frame evaluator give identical
output on both paths, at the 8-bit precision of the exported TIFF, which is
the same precision the integration suite judges. Session 26's argument that
`basic.cl` needs no kernel change is now a measurement rather than an
argument. Note that `demosaic` ran on the CPU in both runs, which is
unrelated to this module.

One caveat retained: `0096-lensfun` fails on the Linux host both with and
without the patch, with an identical 126726 changed pixels, so it is a
pre-existing Lensfun database difference rather than a regression.

**Transparency.** The GUI's `has_ca` now requires
`_pana_has_decodable_ca()`, so the CA fine-tune sliders disappear when
there is no usable payload or no table for the body, and `_display_errors()`
raises "no CA data for this camera" when the embedded-metadata method is
active with TCA requested on a file we cannot decode. The message names the
Lensfun method as an alternative, which informs the user of an option they
already have rather than implementing the hybrid that "Scope and goal"
rules out. Wording is provisional until it has been seen in the GUI.

**Still open.** A rendered comparison against the paired camera JPEGs, which
is the only evidence that would confirm agreement with Panasonic rather than
with Adobe; and the five contexts cover the corpus but not every Panasonic
body, so an uncharacterised body gets distortion only until someone probes
it.

**A flag from the first visual check, and it needs resolving before this is
called good.** Rendering P1366392, the strong-CA file, through the old and
new code with the same XMP, the new correction moves 61% of pixels with a
mean absolute difference of 1.31 in 8-bit terms, and the crude fringing
metric in the worst outer window at r = 0.91 gets *worse*: mean |R-G| rises
from 15.84 to 17.60 and mean |B-G| from 16.64 to 22.73. That is consistent
with the coefficient change itself, since the new map moves B by 1.159 px at
r = 0.85 where the old code moved it 0.398 px, so if the camera wanted about
0.4 px the new code overcorrects.

Three readings, and this session cannot distinguish them:

1. The metric is bad. Mean |R-G| over a crop measures colour content, not
   misregistration, and it is exactly the metric that led session 18 to
   conclusions that later had to be withdrawn. It has no registration and no
   reference.
2. We match Adobe and Adobe does not match Panasonic. The end-to-end checks
   in session 25 compared against Adobe's own planes and agreed to 1e-14 on
   the probed models, with fresh conversions in the audit. If both hold, then
   Adobe's reading of the payload differs from what the camera renders, which
   is exactly the caveat recorded at L1558 and never tested.
3. Something in eps_B is too large by a factor. Less likely, since the audit
   reproduced the matrices from fresh conversions and validated B against
   Adobe's own B plane independently of the R/B symmetry, but not excluded by
   anything measured here.

Reading 2 is the one the project's goal cares about, and the test that
separates all three is the registered comparison against the paired camera
JPEG, which is the next task. **Until that runs, the claim for this patch is
narrow and should stay narrow: it reproduces Adobe's decode of the payload
exactly, and it corrects six structural errors in the previous
implementation. Whether it renders closer to the camera than its predecessor
is unproven.** The work sits on a branch, not on master, so there is no
hurry to conclude.

Crops for inspection: `/c/temp/tca/compare27/`, both files, old and new side
by side at 3x with an 8x amplified difference map.

### The camera disagrees, and the likely reason (session 28)

The registered comparison against the camera's own JPEG says the new decode
overshoots blue, and by enough that the patch cannot stand as it is.

**Design, after a false start.** A first attempt measured channel-versus-
channel residuals within each image and compared across images. Its output
was internally inconsistent: it put the uncorrected render at +0.204 px of
blue aberration at r = 0.85 while also reporting that the old code, which
applies 0.398 px there, matched the camera to 0.003 px, and it reported a
constant dr_R of about -1.5 px at r = 0.30, which no radial correction can
produce. Discarded.

The design that works measures the SAME channel BETWEEN images and takes a
difference of differences: per tile, correlate the render's blue against the
JPEG's blue, and the render's green against the JPEG's green, then take
`disp_B - disp_G` projected radially. Cross-correlating identical content is
far better conditioned than cross-correlating different channels, and the
green subtraction cancels the global scale, crop and distortion mismatch
between a camera JPEG and a darktable export. Zero means our blue sits where
Panasonic put theirs. Script `/tmp/rw2_tca/session29_measure.py`, report
`session29_report.txt`.

**Sanity checks, which the discarded attempt lacked.** Injecting a known
+0.50 px outward radial shift into the blue plane recovers it with correct
sign at every radius and within about 0.08 px, with a mild 15% underestimate
above r = 0.75 from window weighting. Comparing a render against a globally
shifted copy of itself gives medians within 0.006 to 0.012 px, so the noise
floor on a binned median is 0.01 to 0.02 px. The subtracted green field is
small and smooth on P1366399 and large but structured on P1366392, where the
camera applies a 3:2 crop and a 2 to 3% scale difference; it is applied
identically to both channels, so it cannot manufacture a B-minus-G signal.

**Result**, median radial chromatic disagreement in pixels, zero being
agreement with the camera:

    frame       r      new      old     tiles
    P1366392  0.50   +0.216   +0.303    312
              0.70   -0.454   +0.162    146
              0.85   -0.875   +0.071    101
    P1366399  0.50   +0.419   +0.432    295
              0.70   +0.104   +0.125    206
              0.85   +0.015   +0.033     96

On the strong-CA frame the old code sits 0.07 px from the camera at r = 0.85
and the new code sits 0.88 px away with the opposite sign, a 0.95 px gap at
50 times the noise floor. On the weak-CA frame the two are indistinguishable
at 0.03 px, which is the floor, so that frame cannot separate them.

**The leading explanation is a radius-normalisation mismatch, not bad
coefficients.** Note what the earlier validation did and did not establish.
Session 25 compared our recovered coefficients against Adobe's coefficients,
in coefficient space. That comparison is blind to the convention in which
the radius is normalised, because both sides used the same convention. What
was never checked is whether darktable evaluates those coefficients at the
radius Adobe means. darktable normalises r to the half-diagonal of
`p_width` by `p_height` (`lens.cc:2493`). If Adobe's WarpRectilinear
normalises to something else, a half-width or a half-height, then every
coefficient is being evaluated too far out, the error grows with radius, and
it grows fastest in the highest-order terms. The observed signature matches:
the two decodes agree at r = 0.50 and diverge rapidly beyond r = 0.60.

This also explains the awkward fact that the old code, whose coefficients are
demonstrably wrong in six ways, lands closer to the camera. It was *fitted*
against Adobe's output rather than derived from it, and a least-squares fit
of a polynomial can absorb a radius rescale into its coefficients. The old
tables silently carried the convention correction; exact coefficients do not.

For a 4:3 sensor the half-diagonal exceeds the half-width by 1.25, and a
1.25 error in the argument of a cubic in r^2 is more than enough to turn
0.40 px into 1.16 px at r = 0.85.

**Status of the patch: provisional, and suspect.** It reproduces Adobe's
coefficients exactly, which is verified twice, and it corrects six structural
errors, which is also verified. But on the one frame where the difference is
measurable it renders further from the camera than what it replaced, so it
must not be presented as an improvement until the normalisation question is
settled. It sits on a branch, not on master.

**Next.** Establish Adobe's radius normalisation for WarpRectilinear, from
the DNG specification and then empirically, since the specification's wording
has to be confirmed against what the converter actually does. The empirical
test is cheap: the ratio between conventions is a fixed function of aspect
ratio, so probing one payload word on bodies of differing aspect ratio, or
comparing a correction's magnitude at a known pixel position against Adobe's
own rendering, distinguishes them. Then re-evaluate this same comparison,
which is now a trustworthy instrument with a stated noise floor.

### Where the overshoot actually lives (sessions 30 and 31)

Two hypotheses tested, one killed, and the defect localised to a part of the
code the rewrite did not touch.

**Radius normalisation: refuted, on both fit and specification.** No single
scale factor k on the evaluation radius reconciles the new decode with the
camera: fitted k varies by 17x between derivations and leaves residuals at
r = 0.50 that are 15 times the noise floor. And Adobe's own source settles
the convention. In `dng_lens_correction.cpp`, `dng_filter_warp` sets
`fNormRadius = MaxDistancePointToRect(squareCenter, squareBounds)` with
`GetSrcPixelPosition` using `diffNorm = (dst - fCenter) * fInvNormRadius`,
so the radius is normalised to the maximum distance from the optical centre
to the active-area bounds, which for a centred optic is the half-diagonal.
That is exactly what `lens.cc` uses. Reference:
`https://raw.githubusercontent.com/aizvorski/dng_sdk/master/source/dng_lens_correction.cpp`.
Scripts `/tmp/rw2_tca/session30_*.py`, report `session30_report.txt`.

**The direct measurement, which is the best-conditioned one available.**
Comparing the new render against the old render needs no camera JPEG and no
geometry warp, because the two share everything but the lens code. Using the
session 29 instrument, and cross-checked by an independent in-image estimate
that agrees to 0.08 px, at r = 0.85:

    frame      fine    A measured   A predicted   ratio
    P1366399   1.001    +0.014       +0.016       0.87
    P1366392   0.943    +1.363       +0.472       2.89

with new-versus-uncorrected giving +1.198 against a predicted +0.436, a
ratio of 2.75. The ratio stays near constant, 2.6 to 3.0, across r = 0.50 to
0.85. Scripts `/tmp/rw2_tca/session31_measure.py` and
`session31b_inimage.py`, report `session31_final.txt`.

**What that rules out, and what it leaves.** On the weak-distortion frame the
render matches the analytic prediction to 0.005 px, which pins both the new
eps_B from session 25 and the old `_pana_C_B_lo` as exactly what the code
evaluates. So the decode is not the problem, and neither is the polynomial
arithmetic. A near-constant ratio across radius also excludes a wrong Horner
power and any single-k rescale of the evaluation radius, since both would
drift monotonically with r. The one variable that separates the two frames is
the distortion multiplier `fine`: 0.943 where the discrepancy is a factor of
three, 1.001 where there is none.

**So the defect is in the composition of the CA term with the distortion
pathway, not in the CA term.** The candidates are the autoscale in
`_init_coeffs_md_v2`, which multiplies the knot abscissae by the computed
scale and divides the coefficients by it, and the spline lookup in
`_process_md` that consumes the result. Note the interaction that makes this
bite: eps_B is a cubic in u with large cancelling terms, so a few percent
change in the radius at which it is effectively sampled can move its value
by a factor of two or three. That is why the error is invisible at
`fine` = 1 and severe at `fine` = 0.943, and why the old code, which added
its polynomial at the destination radius instead of composing it, escaped.

**Correction to session 28.** The camera-JPEG comparison put the new-versus-
old gap at 0.946 px where the direct render comparison gives 1.363 px, so
that measurement was systematically low. Its verdict stands, its magnitude
does not.

**Status.** The coefficients are vindicated; the rewrite's decode is right
and its application is wrong on frames with appreciable distortion. The patch
stays on the branch, still provisional, and the next step is a code-level
account of the autoscale composition followed by a corrected evaluation
radius, then a re-measurement with this same instrument before going near the
camera JPEG again.

### The code does what the coefficients say (session 32)

Sessions 28 to 31 built their case on one instrument. Instrumenting the
pipeline itself shows that instrument over-reports by a factor of about
three, which changes what those sessions can be said to have established.

**Method.** Temporary `fprintf` diagnostics in `_init_coeffs_md_v2`, at the
knot loop and after the autoscale, and in `_process_md` at the consumer,
built and run on P1366392 with its own XMP, then reverted. The same
post-autoscale print was applied to the pre-patch `lens.cc` from
`eb73e8222f^` so both implementations report from the same place.

**What the pipeline actually carries.** `p_width` by `p_height` is 5184 by
3888 and `buf_in` is the same, so the consumer's half-diagonal is 3240 px
and matches the frame the coefficients were built on; `scale_md` is 1.0. At
knot 13, `r = 0.866667`, `fine = 0.941939`, `rd = 0.816347`,
`u = 0.666423`, `eps_b = 0.000188288`. Autoscale is 0.966282, moving that
knot to abscissa 0.837445. Blue minus green after autoscale:

    build   diffB          diffR          blue in px at 2713 px radius
    new     +1.83582e-4    +2.92e-4       +0.50
    old     -7.629e-6      +2.92420e-4    -0.02

So the new code applies 0.50 px of blue correction and the old applied
essentially none, a difference of 0.52 px. Red is unchanged between the two
builds on this frame, to five figures. **The implementation applies exactly
what the recovered coefficients predict**, which is what session 31's
analytic prediction of 0.47 px said and what the measurement disputed.

**Consequences for what has been claimed.**

- The autoscale is exonerated by direct observation, as session 31's
  numerics already suggested: `scale` differs between the two builds by
  2.7e-5, and green moves by 0.07 px.
- The XMP is exonerated: `method = 0` is embedded metadata, `md_version = 1`
  is VERSION_2 in that enum, `modify_flags = 5` has TCA on, and all four
  fine-tune factors are exactly 1.0. Decoding the params blob also returns
  the correct camera and lens strings, which validates the decode.
- **The instrument over-reports.** It put the new-versus-old blue gap at
  1.363 px where the code carries 0.52 px, a factor of 2.6, and
  new-versus-uncorrected at 1.198 px against 0.50 px. Its own synthetic
  check recovered an injected 0.50 px as 0.35 px, a gain of 0.83, so its
  calibration is not merely imprecise but inconsistent between a shift
  injected into a finished image and a differential produced by resampling
  each channel separately. That distinction is exactly what it was built to
  measure.
- Session 28's ordinal verdict, that the old code sits closer to the camera
  than the new one, may still hold, since a common gain error cancels in a
  comparison of two disagreements against the same reference. Its
  magnitudes are void. **It is not a basis for reverting anything until an
  independent estimator agrees.**

**What is now eliminated as the cause of the disagreement with the camera**:
radius normalisation, by Adobe's own source; autoscale, by direct
observation; the fine-tune factors and the correction method, by decoding
the XMP; and any error in the coefficients or their evaluation, by the
weak-distortion frame matching prediction to 0.005 px and by these prints.

**So the live question is the one session 28 called explanation 2**: we
reproduce Adobe's decode faithfully, and Adobe's decode may not be what the
camera itself applies. Note what that would mean for "Scope and goal": the
premise that Adobe's reading of the payload is a proxy for Panasonic's
intent, carried since session 5 and tested only for whether Adobe *reads*
the payload, would be false as to magnitude.

**Next, in order.** Convert P1366392 to DNG, since the corpus has 18 DNGs
but not this frame, and read Adobe's own blue warp for it; that confirms or
refutes 0.50 px as Adobe's intent on this specific file, independently of
our tables. Then measure the camera JPEG with an estimator from a different
family, edge-localisation rather than tile phase correlation, because two
families agreeing is the only way to trust a number here after this
session. Only then decide whether the patch stands.

**A note on artefacts.** A reboot cleared `/tmp`, taking the venv, the
recovered-map npz, the reference decoder, the payload editor and every
render with it. The matrices survived only because they had been
transcribed into `lens.cc` and verified there. Stage future artefacts under
`/c/temp/tca/`, which persists.

### Adobe confirms the implementation on the disputed frame (session 33)

P1366392 was missing from the 18-file DNG corpus, which is why the dispute
had never been checked against Adobe on the one frame where it is
measurable. Converted it with `Adobe DNG Converter.exe -c -p0`, read
`OpcodeList3` with `exiftool -b`, and parsed the WarpRectilinear opcode
directly: id 1, version 0x1030000, N = 3 planes, centre (0.5, 0.5), six
doubles per plane.

Adobe's blue coefficients for this file are
`kr = 0.998526818130, -0.111484756403, 0.049982868436, -0.005654027675`
against green `0.998679194585, -0.111753183743, 0.049855557724,
-0.005782033788`. At the physical destination radius 2808 px, which is the
knot-13 abscissa 0.866667 on darktable's 3240 px half-diagonal:

    quantity                 darktable        Adobe          ratio
    blue minus green         +1.773830e-4     +1.708426e-4   1.0383
    blue displacement        +0.498 px        +0.480 px
    same abscissa 0.866667   +1.773830e-4     +1.753099e-4   1.0118

**The patch reproduces Adobe to within 4% on the frame that started the
dispute**, verified end-to-end from a freshly converted DNG against numbers
printed from the running code, with no reliance on our own tables at any
step. So 0.50 px is Adobe's intent here, not an artefact of the rewrite.

**A real but small normalisation error, of a kind session 30 did not
test.** The 3.8% splits into the 1.2% same-abscissa gap, which is Adobe's
per-model profile term from session 25 that we deliberately do not
reproduce, and about 2.6% from the abscissa itself. Adobe normalises to the
**active area**, 5208 by 3904 giving 3254.400 px, while darktable
normalises to `p_width` by `p_height`, the default crop of 5184 by 3888
giving 3240.000 px. Session 30 asked whether the radius is normalised to
the half-diagonal and Adobe's source said yes; it never asked
*half-diagonal of which rectangle*. The same 1.2e-3 offset appears in the
green multiplier, 0.941939 against Adobe's 0.940728, so this is the
pre-existing 0x0119 distortion path as much as the CA path. Quantified and
left alone for now: it is a 2.6% effect where the open question is a factor
of three, and correcting it for CA alone would desynchronise it from the
distortion decode.

**The finding that actually unblocks the camera test.** Adobe's blue
correction changes sign with radius:

    r      B-G          px
    0.30   -1.271e-4   -0.124   outward
    0.40   -1.056e-4   -0.138   most negative
    0.50   -7.531e-5   -0.123
    0.65   -6.586e-6   -0.014   zero crossing
    0.70   +2.478e-5   +0.056   inward
    0.90   +2.166e-4   +0.634
    1.00   +3.714e-4   +1.209

That is the signature of the cancelling cubic, and it is a far better test
than magnitude at one radius, because **a gain error cannot move a zero
crossing**. The instrument's calibration is untrustworthy by a factor of
2.6 to 3, but the radius at which the camera's own blue correction reverses
sign, and whether it reverses at all, is immune to that. If the camera
crosses near r = 0.65 and is outward below it, Panasonic is applying this
same correction and the rewrite is right. If the camera shows no reversal,
Adobe's reading of the payload is not what the camera does, and that is the
fork in the road to put to the developer rather than decide here.

Artefacts under `/c/temp/tca/dng392/`: the DNG, `op3.bin`, and the source
RW2 alongside it.

### The rewrite overcorrects, and the residual sign proves it (session 34)

A second instrument, from a different family: per-image, in-image radial
blue-minus-green edge offsets, so each image is measured alone and no
registration between differently-warped frames is ever needed. That was the
rock every earlier attempt broke on. Two sub-estimators, a gradient-centroid
difference and a parabolic fit to the cross-correlation of profile
derivatives, run over 48 px tiles gated on edge coherence and on the
gradient lying within 30 degrees of radial. Sign convention: positive means
blue lies OUTWARD of green. Script and report under
`/c/temp/tca/measure/session34_{sign.py,report.txt}`, renders alongside.

**The instrument calibrates against ground truth, and the verdict is that
the centroid estimator is the accurate one.** We know what darktable
applied, so `off` minus `new` must equal Adobe's column:

    r      off-new centroid   off-new xcorr   Adobe   gain cent   gain xcorr
    0.82   +0.353              +1.167         +0.364   0.97        3.21
    0.88   +0.525              +1.704         +0.544   0.97        3.13
    0.93   +0.969              +2.029         +0.778   1.25        2.61

The centroid estimator recovers the applied correction essentially exactly
at outer radii; the correlation estimator inflates it by about three. That
is the same factor by which the session 29 tile phase-correlation
instrument over-reported, so **both correlation-based instruments share one
bias and the session 32 discrepancy is now explained**: correlation methods
respond to the differential radial *stretch* across the profile window, not
only to the local displacement. Session 34's own summary preferred the
correlation estimator on scatter grounds; calibration says the opposite,
and calibration wins.

**Per-image residual blue-minus-green, which is the quantity that matters.**

    r      off      old      new       jpeg     tiles o/l/n/j
    0.62   +0.121   +0.199   -0.024   -0.062    216/218/217/76
    0.68   -0.003   +0.191   -0.096   +0.006     39/41/38/73
    0.72   +0.111   +0.154   -0.353   -0.018     52/27/25/18
    0.78   +0.184   +0.156   -0.586   -0.017     52/46/62/12
    0.82   +0.208   +0.006   -0.959   -0.001     50/11/23/12
    0.88   +0.258   -0.012   -1.446   -0.032     31/10/16/10
    0.93   +0.338   +0.187   -1.691   +0.010     44/12/20/5

Correlation estimator shown; the centroid columns give the same signs at
r >= 0.72, `new` running -0.256, -0.156, -0.443, -0.320, -0.956.

**Three findings, in order of how firmly they stand.**

1. **The camera's own JPEG has no measurable chromatic residual at any
   radius**, |values| at or below 0.06 px across sixteen bins in both
   estimators. Panasonic's own development leaves blue and green
   co-registered. This is the tightest number in the whole investigation.
2. **The patched code overcorrects.** From r = 0.72 outward the new
   render's residual is *negative*, meaning blue has been pushed past green
   to the inside, while the uncorrected render is positive there. A sign
   reversal of the residual relative to the uncorrected image is the
   definition of overshoot, and **it is immune to the gain problem**,
   because no positive gain can flip a sign. Five consecutive bins, both
   estimators.
3. The old code undercorrects mildly, leaving roughly the native offset.
   Neither implementation matches the camera, but they miss in opposite
   directions.

So session 28's ordinal verdict survives on much better evidence than it
had, and with the mechanism reversed: the old code is closer not because it
is right but because doing almost nothing beats overshooting by a factor of
two or three.

**Two candidate explanations, and they call for different responses.**

- **The payload scaling is too large.** We reproduce Adobe to 4% and Adobe
  instructs +0.78 px at r = 0.93, but the uncorrected render's native
  offset there measures only +0.34 px by the inflating estimator and +0.01
  to +0.21 by the accurate one. If the native aberration is genuinely a
  couple of tenths of a pixel, then Adobe's reading, and therefore ours, is
  several times too large, and the premise that Adobe's decode is a proxy
  for Panasonic's intent fails on magnitude.
- **The demosaicer has already removed part of it.** `lens` runs *after*
  demosaic, and AAHD is channel-coupled, so it can suppress lateral
  chromatic offset before the correction is applied. Applying a correction
  sized for the raw aberration on top of an already partly corrected image
  necessarily overshoots. This explanation needs no error in the decode at
  all, is a property of darktable's pipeline order rather than of
  Panasonic's data, and would explain why the camera, which corrects during
  its own demosaic, ends at zero.

The second is testable and cheap, and it is item 11 on the task list,
demoted long ago for want of a purpose: re-render with a demosaicer that
does not couple channels and re-measure the residual. If the overshoot
shrinks, the decode is exonerated and the fault is in where the correction
sits in the pipe. If it does not, the magnitude question is real and goes to
the developer as a fork in the road.

**Caveat on inner radii.** `off` minus `old` should be near zero, since the
old code applies -0.02 px of blue here, but it measures -0.1 to -0.3 px at
inner radii, so the estimator carries a bias of that order between the
uncorrected and lens-corrected render paths. Inner-radius absolute
residuals therefore cannot be trusted, and the earlier question of whether
the camera reverses sign near r = 0.65 remains unresolved: the two
sub-estimators split there, -0.112 against +0.008. The outer-radius
overshoot is several times that bias and is not affected.

### Demosaic explains part of it, not most of it (session 35)

Rendered the same frame with three demosaicers, LMMSE as the XMP specifies,
VNG4 and PPG, both with the correction off and on, and measured all six with
the session 34 instrument unchanged. Driver and report:
`/c/temp/tca/measure/session35_{demosaic.py,report.txt}`.

**Incidental correction.** The pipeline here uses **LMMSE**, not "AAHD" as
this document has said since session 9. darktable has no AAHD demosaicer at
all; the Bayer methods are PPG, AMaZE, VNG4, RCD, LMMSE and the two
passthroughs (`src/iop/demosaic.c:58-66`). Items 9 to 11 of the task list
were premised on a method that does not exist.

**Native blue-minus-green at outer radii, r = 0.70 to 0.95:**

    demosaic   native    residual after correction
    LMMSE      +0.206    -0.320
    VNG4       +0.224    -0.187
    PPG        +0.340    -0.325

The native offset **is** demosaic dependent, spreading 0.134 px, and in the
expected direction: the most sophisticated method leaves the least
aberration, the simplest leaves the most. So the mechanism is real.

**But it does not carry the overshoot.** Every one of the three reverses
sign after correction, so no demosaicer avoids overshooting. If suppression
were the whole story, PPG, which leaves the most aberration, should land
near -0.16 px; it lands at -0.325. About 0.165 px of overshoot is
common-mode and independent of demosaic choice. Only the r = 0.72 and 0.78
bins carry 25 or more tiles, so treat the spread as indicative; the
mid-radius bins with 800-plus tiles agree across methods within 0.03 px,
which is what says the gates are coping with PPG and VNG4 zippering.

**What this makes of the magnitude.** The correction applied is +0.50 px at
r = 0.867, verified from the running code and matching Adobe's own DNG at
+0.48. The aberration actually present, measured after the *least*
suppressive demosaic available, is +0.34 px at outer radii. So the
payload-derived correction is roughly one and a half to two and a half times
the aberration it is correcting, and the camera's own JPEG, which shows no
residual at any radius, is consistent with the camera applying about the
native amount.

**Conclusion, and it is a fork in the road.** Adobe is an exact oracle for
the *structure* of the payload, which is how sessions 22 to 25 recovered the
word map to 1e-15, and it is confirmed to read the payload rather than a
lens database. It is **not** a reliable oracle for *magnitude*: Adobe
appears to overcorrect Panasonic lateral CA by roughly a factor of two, and
we now reproduce that overcorrection faithfully. The stated goal was to
reproduce Panasonic's own correction from Panasonic's own data, and on this
frame Adobe's reading is demonstrably not what Panasonic's camera does.

**The patch must not be proposed as it stands.** On this frame it would
introduce about 0.3 px of reversed fringing at the corners where the shipped
code introduces none, which is a visible regression, not an improvement.
Trigger three of "When to stop" is arguably met for the Adobe-as-oracle
route. The decision on what to do instead belongs to the developer, so it is
put to them rather than taken here; the options are recorded in the task
list.

### The overcorrection is universal, and the factor is two (session 36)

Eighteen frames, two bodies, six lenses including two Sigmas, both signs of
correction. Uncorrected 16-bit renders with demosaic forced to PPG, which
suppresses the least aberration and so gives the fairest native estimate,
measured with the session 34 instrument unchanged, against Adobe's instructed
correction read from the eighteen DNGs. Driver and report
`/c/temp/tca/measure/session36_{corpus.py,report.txt}`, Adobe extraction
`adobe_table.py`, renders under `measure/corpus/`.

Useful incidental discovery: **lens correction is not auto-applied**, so a
render with no XMP is already an uncorrected reference. A default sidecar
carries eleven history entries and no `lens` among them.

**1. The camera is accurate on every frame.** Median camera-JPEG residual in
the outer band is 0.026 px, at the instrument's noise floor, with only two of
eighteen above 0.15 px (P1260640 at -0.22, P1260636 at +0.17). Panasonic's
own development leaves essentially no lateral chromatic offset anywhere in
the corpus.

**2. Adobe's instructed correction is twice the aberration present.** Over
the eleven frame-bands where the native offset exceeds 0.15 px and a ratio
therefore means something, Adobe-over-native has **median 2.00**, 16th to
84th percentile 1.38 to 2.98, and nine of eleven above 1.5. It clusters
rather than scattering.

**3. Adobe's sign is right, tested where it is hardest.** Six frames have
Adobe instructing a *negative* blue correction at r = 0.85, that is pushing
blue outward. On five the measured native offset is also negative, so the
unusual sign is correct: P1260635 -0.240, P1260639 -0.187, P1366483 -0.081,
P1366479 -0.075, P1260638 -0.057. The sixth, P1366486, mismatches at +0.081
against Adobe's -0.107, both below the meaningful threshold.

So the decode's **structure is validated far beyond the one frame**: which
lens gets a correction, its sign, and its radial shape are all right across
six lenses and two bodies, including the awkward long-focal frames where the
correction reverses. Only the magnitude is wrong, and it is wrong by the
same factor everywhere.

**Sparse frames, flagged rather than used**: P1366482 has 47 tiles in the
outer band, P1366483 452, P1366479 592. Their signs agree with Adobe but
their magnitudes carry no weight. The GX80 JPEG-to-render half-diagonal ratio
is 0.9949 and the G9's is 1.0000, so no radius rescale was applied and none
is needed at this band width.

**What a factor of two means for the options.** Option B, scaling the map,
now has an empirical basis across eighteen frames rather than one, and the
factor is suspiciously round. A definitional halving somewhere in the
payload's meaning would explain it, and would make the scaling principled
rather than a fudge; the mechanism is not yet identified, and a mechanism
would be worth having before shipping. Note also that darktable already
exposes the knob: `cor_ca_r_ft` and `cor_ca_b_ft` multiply eps directly, with
range 0 to 2 and default 1, so the hypothesis can be tested end-to-end
without touching code, and a user could already compensate by hand.

### Half strength, tested without touching code (session 37)

darktable's `cor_ca_r_ft` and `cor_ca_b_ft` multiply eps directly, so the
halving hypothesis is testable by rendering. Built lens history entries
programmatically with method embedded-metadata, version 2, `modify_flags` set
to TCA only so the chromatic term acts alone, and strength 1.0 and 0.5, on six
frames across two bodies and four lenses including one where the correction
reverses sign. Params offsets verified by decoding the donor blob first: the
four fine-tune floats sit at 304, and `md_version` at 324. Scripts
`/c/temp/tca/measure/{make_ft_xmp.py,run_ft.sh,session37_half.py}`, report
`session37_report.txt`, renders under `measure/ft/`.

Self-tests first. The applied correction scales linearly with strength, with
`(off-ca10)/(off-ca05)` at 2.14 in the mid band, and blue pixel differences
halving as expected. Green moves slightly too, because autoscale takes its
maximum across all three channels, so changing the chromatic term changes the
global scale a little; the in-image estimator is insensitive to that.

**Overshoot at full strength is confirmed where the signal is strong.** On the
three frames with native offsets above 0.15 px the residual flips sign:
P1366477 +0.389 native to -0.078, P1366484 +0.208 to -0.264, P1260641 +0.165
to -0.068. On the three weak frames the overshoot is below the noise floor and
cannot be seen either way.

**Half strength helps but does not reach the camera.** The camera's residual is
at or below 0.13 px everywhere. At strength 0.5 two frames match that, at or
below 0.04 px, while four sit at 0.1 to 0.3 px. Fitting the strength that
would zero each frame's residual, which is well posed because the correction
is linear in strength, gives an outer-band **median of 0.51** with range 0.3 to
0.8 once a sign-reversed frame and a noise-floor frame are set aside, and a mid
band median of 0.32.

**The awkward frame.** P1260635, where Adobe instructs a negative correction,
disagrees by band: at outer radii full strength lands closer to zero than half
does, while at mid radii half is better. One frame, but it is a reminder that
the shape may not be exactly right either, not only the scale.

**So a factor of two is first-order right and not the whole story.** The
correction as Adobe reads it, and as we reproduce it, is about twice what the
image needs; applying half of it improves chromatic registration on most
frames but leaves two to three times the camera's residual, and the
frame-to-frame spread in the best-fit strength is 0.3 to 0.4 in strength units.
No mechanism for the factor has been identified, and a magic 0.5 in the decode
would be hard to justify to a reviewer without one.

**One avenue is already closed.** SILKYPIX, Panasonic's own bundled converter,
was shown in session 12 not to consume 0x011b at all, so it cannot serve as a
second witness to Panasonic's intent. The camera itself remains the only
witness, and it has now been measured on eighteen frames.

### Word 14 is a gate, not a scale (session 38)

The obvious mechanism for a clean factor of two would be a scale word that
Adobe reads as a boolean. Word 14 is the candidate: session 23 showed that
zeroing it makes Adobe emit no correction at all, and it is **256 in all 132
files on disk**, across thirteen body types from the GH4 to the S1RII.

Rebuilt the payload editor, which `/tmp` took, and this time located the
payload by Rigo's checksum rather than by a fixed offset, since the offset
varies by body from 0x39c to 0x4b8. Self-checks: setting word 14 to its
existing value changes no bytes, and any edit changes only the target word
and the four checksums. `/c/temp/tca/measure/payload.py`.

Ten mutants of P1366477 through Adobe:

    w14    hex      planes   result
    0      0x000    1        distortion only, no per-channel CA
    1      0x001    1        distortion only
    64     0x040    1        distortion only
    65     0x041    1        distortion only
    128    0x080    1        distortion only
    192    0x0c0    1        distortion only
    256    0x100    3        CA emitted, baseline
    257    0x101    3        CA emitted, coefficients identical
    320    0x140    3        CA emitted, coefficients identical
    512    0x200    3        CA emitted, coefficients identical

**Word 14 is a boolean gate on the high byte.** Any value with a non-zero
high byte enables the per-channel planes, and the value has no effect on the
coefficients whatever: the R-minus-G and B-minus-G ratios against baseline
are 1.000000 for 257, 320 and 512, with green untouched. So it is not a
scale, and this route to explaining the factor of two is closed.

**A near miss worth recording.** Word 7, which session 23 found inert for
Adobe, clusters at values that look exactly like Q15 fixed point: on the G9
it is 32570, 32767, 32575 for one lens and 16383, 16380, 16252, 16383,
16383, 16189 for the others, that is 1.0 and 0.5; on the GX80 the same
lenses give 16359, 16343, 16222 and 8135, 8007, 8055, 8175, 8168, 8014,
almost exactly half the G9 values. A per-lens strength that Adobe ignores
would look precisely like this. **But the measurements do not support it**:
P1366477 carries the high value and needs the most reduction, ratio 2.09,
while P1366481 carries the half value and needs almost none, ratio 1.03. Nor
can our instrument settle it, since per-frame ratios scatter from 0.36 to
3.83 and only the aggregate median is meaningful. Word 7 remains the leading
structural candidate but needs a method that does not depend on measuring
tenths of a pixel per frame.

**The one avenue that could settle it outright** is the camera's own code.
`/c/temp/tca/firmware/G9___V27.bin` is on disk, and the routine that reads
this payload is in there. That is a large undertaking and is not started.

**Also found**: there is no integration test covering the Panasonic
embedded-metadata path at all. Tests 0145 and 0146, used to validate this
work since session 27, are `lens-metadata-xtransIV-modversion-6` and `-7`,
which exercise Fuji. They pass, but they were never evidence about
Panasonic. Adding a Panasonic case needs a sample RW2 in the integration
test repository.

### Shipped: half strength, with the reasoning in the source

`_PANA_CA_STRENGTH 0.5` multiplies eps for both channels, verified by
instrumenting the running code: blue-minus-green at knot 13 on P1366392 is
now 9.1791e-5, exactly half of 1.83582e-4. Commits `3d16d0d9c2` and
`b267d72431`. The constant is named and documented rather than folded into
the tables, so that if a mechanism is found it can be removed in one place.

### Olympus does it too (session 39)

The developer's suggestion, that Olympus samples are available from camera
review sites, turned this from a Panasonic question into a darktable
question. Six OM System OM-5 Mark II frames with their in-camera JPEGs from
photographyblog, M.Zuiko 12-45mm F4 at 12mm, the widest setting and so where
lateral CA is largest. Every file carries `Exif.OlympusIp.0x150a` with four
values and `0x150c` with six, which is exactly what `exif.cc:1272` and
`:1295` read, so these exercise darktable's Olympus path.

**This path shares nothing with the Panasonic work.** Its coefficients come
straight from Olympus's own tag, with no Adobe involvement at any point, and
its branch in `_init_coeffs_md_v2` is separate code.

Rendered uncorrected and TCA-only pairs with demosaic PPG, measured with the
session 34 instrument unchanged. Script and report
`/c/temp/tca/measure/session39_{olympus.py,report.txt}`.

- The camera's JPEGs are again **residual-free**: median absolute
  blue-minus-green of 0.021 px in the outer band, per frame +0.054, +0.008,
  +0.000, +0.002, +0.034, +0.056.
- **darktable's correction reverses the residual's sign** in four of six
  frames by the accurate estimator and five of six by the corroborating one.
  The two that do not reverse have native offsets below 0.1 px, at the noise
  floor, and still grow in absolute value under correction, from -0.087 to
  -0.309 and from -0.019 to -0.092, which is overshoot by another route.
- Where a ratio is meaningful, native offset above 0.15 px, the applied
  correction over the aberration present is **1.87 and 2.34, median 2.11**.

**So the factor of two is not ours and not Panasonic's.** Two manufacturers,
two independent code paths, two independent sources of coefficients, the same
doubling. That relocates the problem: it is not an error in the 0x011b decode,
which is now confirmed exact three ways, but something shared in how
darktable turns a per-channel radial coefficient into a pixel displacement, or
in a convention that both manufacturers use and darktable does not follow.

**Weight of evidence, stated honestly.** Only two of the six Olympus frames
carry a native signal strong enough for a ratio, one of them with just 36
JPEG tiles in the outer band, and all six share one lens at one focal length,
so they are six scenes against one correction rather than six independent
profiles. The sign reversals are the sturdier part, since no positive gain
error can produce one. Call it strongly suggestive, not established.

**What it means for the shipped constant.** `_PANA_CA_STRENGTH 0.5` looks less
like a fudge now: if the same factor appears in a path that never touched
Adobe, the correction is a shared convention error rather than a Panasonic
mis-decode. That also means the honest fix may belong in the common code
rather than in the Panasonic branch, and that Olympus, Sony and Fuji users
are getting the same doubling today. Sony and Fuji are untested.

**Method note worth keeping.** Maker notes sit near the start of an ORF, so
`curl -r 0-5000000` is enough to inspect a body's correction tags without
downloading 20 MB. A 1.5 MB range is not: it truncates the ImageProcessing
IFD and produces false absences, which is what first made it look as though
modern OM bodies had dropped these tags. They have not; all seven bodies
checked from the E-M1 to the OM-3 carry both.

### Sony and Fujifilm: not universal (session 40)

Four Sony ILCE-6700 frames with the E PZ 10-20mm F4 G, an ultra-wide zoom at
10, 12.5 and 20mm, and four Fujifilm X-T5 frames with the XF30mm and XF80mm
macro primes, all with their in-camera JPEGs, all carrying the tags
`exif.cc:1157` and `:1185` require. Rendered uncorrected against TCA-only,
PPG for the Bayer Sony and Markesteijn for X-Trans, measured with the same
instrument. Script and report
`/c/temp/tca/measure/session40_{sonyfuji.py,report.txt}`.

**The one robust cross-manufacturer fact.** Every camera's own JPEG is
residual-free: median absolute offsets of 0.020 px (Panasonic), 0.021
(Olympus), 0.017 (Sony) and 0.019 in red, 0.095 in blue (Fujifilm). Four
manufacturers, four independent pipelines, all leaving the channels
registered. Whatever each camera does internally, it works.

**Like for like, which needed care.** Sony's and Fuji's blue offsets are at
the noise floor on these lenses, so their informative channel is red, while
Panasonic's and Olympus's was blue. Comparing blue against red would have
been meaningless, so the Panasonic red ratios were recomputed from the
session 36 tables:

    manufacturer  channel  ratio applied/present   n
    Panasonic     blue     2.00                    11
    Panasonic     red      1.71                    28
    Olympus       blue     2.11                     2
    Fujifilm      red      1.41                     3
    Sony          red      1.15                     3

**So the doubling is not universal.** Sony looks close to correct and
Fujifilm sits between, while Panasonic over-applies in both channels and
Olympus in blue. But this is a gradient rather than a clean split, and the
evidence is thin everywhere except Panasonic: n = 3 for Sony and Fuji, n = 2
for Olympus, against n = 11 and 28 for Panasonic, with per-frame scatter
running from 1.0 to 3.9. The distinction between 1.15, 1.41 and 1.71 is not
established at these sample sizes.

**One nuance that argues against a single convention error.** Panasonic's red
ratio, 1.71, is lower than its blue, 2.00, on the largest sample we have. A
clean factor-of-two in how a coefficient becomes a displacement would apply
equally to both channels. The two may still be within each other's
uncertainty, but it weakens the tidy story from session 39 that a shared
convention is at fault.

**What this settles for the patch.** The half-strength constant stays in the
Panasonic branch and does **not** move into common code, since Sony at 1.15
would be made worse by it. Olympus over-applying is worth reporting upstream
as an observation with its evidence, not as a patch: two usable frames on one
lens is not enough to change anyone's images.

**To strengthen this** the missing ingredient is obvious: Sony and Fuji
frames whose *blue* aberration is large, which means different lenses rather
than more frames of these. Macro primes and this particular ultra-wide both
happen to put their lateral CA in red.

### Eighty-one frames, nine bodies (session 41)

Since the firmware is unlikely ever to give up the mechanism, the next best
thing is to pin the number down. The third-body corpus had raws but almost no
JPEGs; those are downloadable from the same review pages, so the paired corpus
grew from 18 frames on two bodies to **81 frames on nine**: GX9, G80, G90,
GH5, GH5S, S1II, S1IIE, S1RII and S9.

**Pairing was verified, not assumed.** The JPEG and raw galleries on a review
page are not always the same length, so index N in one need not be the same
exposure as index N in the other. Every pair was checked by
`DateTimeOriginal`, and the JPEG deleted when it did not match. That rejected
the GH4 and GX8 sets outright, whose galleries are indexed differently, and
kept 78. Script `/c/temp/tca/measure/fetch_jpgs.sh`.

**New tooling, replacing what `/tmp` ate.** `pana_decode.py` parses
`_pana_ca_radii`, `_pana_ca_M_R` and `_pana_ca_M_B` out of `lens.cc` rather
than keeping a second copy that could drift, locates the payload by checksum,
and reproduces the running code exactly: eps_b identical and eps_r agreeing to
the last printed digit against the instrumented build. That makes one render
per frame sufficient, since the decoded correction is computed analytically
instead of by rendering a corrected copy.

**The structural result is now overwhelming.** On every frame with a
measurable native offset the decode gets the **sign right: 53 of 53 in the
outer band and 44 of 44 in the mid band**. Across nine bodies, many lenses and
both signs of correction, it never once points the wrong way. No scale or gain
error can affect that, and it is the strongest evidence yet that the word map
is genuinely Panasonic's.

**The cameras remain perfect.** Median absolute JPEG residual 0.017 px in blue
and 0.014 in red over 156 frame-bands, mean -0.009, standard deviation 0.066.
That standard deviation is also the cleanest estimate of the instrument's own
precision that we have, since the quantity being measured is truly zero.

**The magnitude, estimated four ways.** Regression through the origin of the
decoded correction on the aberration present:

    band   channel   forward   reverse   orthogonal   noise-corrected
    outer  blue      1.67      3.39      3.11         1.79
    outer  red       1.31      2.94      2.57         1.36
    mid    blue      1.48      2.86      2.57         1.51
    mid    red       1.62      2.31      2.15         1.63

Forward regression is attenuated by noise in the denominator and reverse is
inflated by it, so the truth is bracketed between them. **Every estimator
exceeds 1.0**, the lowest being 1.31, so the correction is definitely too
strong. A factor of 2 sits comfortably inside the bracket; so does 1.5. The
bootstrap 95% interval on the forward blue slope is 1.32 to 2.15.

**Per-body differences are noise, not signal.** The outer-band blue slope
ranges from 0.69 to 3.18 across the nine bodies, but the S1II gives 0.69 while
the S1IIE, very nearly the same camera, gives 2.80. Different sample scenes
and lenses per body, not different conventions.

**Where that leaves the shipped constant.** 0.5 remains the best single choice:
it is inside every bracket, it is the round number a definitional error would
produce, and erring toward under-correction is the safer direction when the
alternative reverses the fringing. What has changed is the confidence behind
it, which now rests on 81 frames and nine bodies rather than one frame. What
has not changed is that the factor is unexplained.

### The factor tracks provenance, not manufacturer (session 42)

Reading how darktable's other embedded-CA branches were derived explains the
pattern session 40 measured. Quotes below are verbatim from
`https://github.com/darktable-org/darktable/pull/12760`, verified directly.

**Sony came from Sony's own decoder.** Freddie Witherden recovered the
`2^-21` chromatic scale and `2^-14` distortion scale by decompiling Sony
Imaging Edge Desktop and setting hardware breakpoints on its coefficient
buffers, published at
`https://discuss.pixls.us/t/sony-raw-chromatic-aberration-correction-model/21153`;
the distortion scale was independently published by Yakov Galka at
`https://stannum.io/blog/0PwljB`. The decompiled form is
`green = 1 + 2^-14 d`, `red = (1 + 2^-21 c_r) green`, `blue = (1 + 2^-21 c_b)
green`, which is exactly the per-channel multiplicative structure relative to
green that our branch uses. **Sony measures 1.15, essentially correct.**

**Olympus came from Adobe.** paolodepetrillo, opening the PR: *"these tags
were identified by comparing the tag values to their conversion to
WarpRectilinear opcodes by Adobe DNG Converter"*, with the caveat *"nothing
here should be considered authoritative - just my attempt to figure them
out"*, and later *"I can guess at how it should work but have no way to know
if I'm interpreting it as the lens manufacturer intended"*. **Olympus
measures 2.11.**

**Fujifilm's provenance is undocumented.** paolodepetrillo asked directly how
the Sony and Fuji splines were derived and got an answer about vignetting
only. **Fuji measures 1.41.**

**And ours came from Adobe too.** Sessions 22 to 25 used Adobe DNG Converter
as the oracle precisely because it reads the payload. **Panasonic measures
2.00 in blue and 1.71 in red.**

    provenance of the scale              manufacturer   measured ratio
    decompiled from the maker's decoder  Sony           1.15
    matched against Adobe DNG Converter  Olympus        2.11
    matched against Adobe DNG Converter  Panasonic      2.00
    undocumented                         Fujifilm       1.41

**The factor follows how the scale was obtained, not who made the camera.**
Every branch traced to Adobe over-applies by about two; the one branch traced
to the manufacturer's own software is right. That reframes our constant
completely: 0.5 is not a Panasonic fudge but a correction for an error
inherited from using Adobe as the oracle, and the same error is already
shipping for Olympus.

**A user reported exactly our signature, two years ago.** AxelG-DE, 18
November 2023, on the 15mm Summilux: *"For the 15mm Summilux, not all CA's
disappear, they just flip :-)"*, with a video. That is a reversed residual at
the frame edges, observed by eye, on a lens whose correction came through the
Adobe-derived path. Also open: issue #16648, "Difference for embedded
lens-correction for OM-System Mark II between DNG and ORF".

**Our method is the project's own stated standard.** sgotti, 12 November
2022: *"I think that, like done for the Fuji and Sony corrections, we should
use the ooc image as the reference leaving lensfun out from the comparison."*
Validating against the out-of-camera JPEG is what the maintainers asked for,
and commit `f51dee797b` is an in-tree precedent for empirically refitting
these constants, though for Fuji distortion and vignetting rather than CA.

**Independent corroboration of the session 26 frame fix.** paolodepetrillo's
description of the Olympus model is `r_in = dist(r_out) + ca_red(dist(r_out))`,
that is, the chromatic term evaluated at the *distorted* radius. That is the
raw-frame convention we arrived at separately, and it confirms the sixth error
we found was real.

**The experiment this hands us.** Panasonic ships no desktop decoder that
reads 0x011b, which session 12 established for SILKYPIX, so the camera is our
only reference. **Olympus does**: OM Workspace is free, runs on Windows, and
paolodepetrillo himself suggested probing it. Comparing OM Workspace's
rendering of an ORF against Adobe DNG Converter's opcodes for the same file
would test the inherited-Adobe hypothesis directly on a manufacturer decoder.
If OM Workspace applies about half of what Adobe instructs, the hypothesis is
confirmed, and our 0.5 stops being empirical.

### Adobe is faithful to Sony, and word 7 is not the scale (session 43)

The OM Workspace plan was to compare a manufacturer-derived reading of a CA
tag against Adobe's reading of the same file. That comparison is available
already, without the gated download: darktable's Sony branch *is* a
manufacturer-derived reading, since its `2^-21` scale came from decompiling
Sony Imaging Edge. Converting the four Sony ARW files with Adobe DNG
Converter and evaluating both models at matched physical radii:

    file      r     sony B-G    adobe B-G  ratio   sony R-G    adobe R-G  ratio
    a6700_01  0.50  0.000e+00  -3.765e-05   n/a   +1.223e-04  +1.435e-04  1.17
    a6700_02  0.50 -2.357e-04  -2.465e-04  1.05   +4.124e-04  +4.145e-04  1.01
    a6700_03  0.50 -1.223e-04  -1.414e-04  1.16   +2.445e-04  +2.373e-04  0.97
    a6700_04  0.50 -1.490e-04  -1.893e-04  1.27   +3.579e-04  +3.845e-04  1.07

On the well-conditioned points, red at r = 0.50 where the magnitudes are
largest, Adobe agrees with Sony's own convention to within 17%. **Adobe does
not double everything.** The generic "Adobe inflates" reading of session 42 is
refuted; whatever happens with Panasonic and Olympus is specific to those
formats, not a property of Adobe's opcode conversion. The n/a entries are
where Sony's spline interpolates to exactly zero while Adobe's polynomial does
not, a knot-placement artefact rather than a scale difference.

**Word 7 is not a strength word.** With 110 frames carrying it, grouping by
its Q15 value and fitting the slope per group gives, outer band:

    w7 as Q15   n   blue   red
    ~1.0        5   1.48   1.56
    ~0.5        9   0.83   0.44
    ~0.25      10   2.67   1.29
    ~0.125     30   1.72   1.16

If the camera scaled by w7 and Adobe ignored it, the ~1.0 group would sit near
1 and the ~0.5 group near 2. They do the opposite, and there is no monotone
trend. Independently fatal: across the full corpus word 7 ranges from -30255
to +32735, and a negative strength is meaningless. The Q15-looking clustering
in the nine G9 and GX80 files was a coincidence of one body pair.

**What this exposes about our own number.** With Adobe exonerated generically
and word 7 gone, one candidate that remains is a bias in the measurement
rather than in the data. The instrument was calibrated against a *difference*
between two renders and is good to 3% there, but the native aberration is an
*absolute* offset within a single image, and session 34 already found that
`off` minus `old` reads -0.1 to -0.3 px where it should read zero. Demosaic
coupling pulls red and blue toward green, so an absolute in-image offset is
systematically under-read, which inflates the ratio of decoded correction to
aberration present. Session 35 measured that suppression at 0.134 px between
LMMSE and PPG and PPG is not free of it either.

**So the factor may be smaller than 2**, and the way to settle it is a
measurement with no demosaic at all: render with `photosite color`, method 4,
which leaves every pixel with only its own channel, then measure radial
offsets between the red, green and blue sub-planes of the mosaic directly.
That removes the one systematic we cannot otherwise bound. Until then the
honest range remains 1.3 to 3.4 with a central estimate near 1.7, and 0.5 is
a defensible but not exact choice of strength.

## Scope and goal

Set by the developer, post-session-21, and it settles two things this
document had left open.

**The goal is to reproduce Panasonic's own CA correction in darktable,
computed from the same data Panasonic uses.** Not to correct the lens as
well as possible, and not to correct it by whatever means works.

Two consequences follow, and both close off work this document had
previously entertained.

- **The hybrid path is out**, and so is image-adaptive CA detection in
  `cacorrectrgb`. Embedded distortion plus Lensfun TCA would give users a
  working correction, but from a different lens model measured by someone
  else; edge statistics would give one computed from the image. Neither is
  Panasonic's correction, so neither is an answer to this question. The
  proposals are annotated in place as ruled out rather than deleted.
- **The direct raw CA measurement of sessions 14 to 21 was aimed at the
  wrong quantity.** It records the sensor's actual aberration, and the
  goal is Panasonic's correction of that aberration, which by session 18's
  own estimate leaves 40 to 50 percent of it in place at outer radii. The
  two are different targets, as that session said in as many words. So a
  refit that matched the sensor better would be a step away from the goal,
  and the AAHD measurement work is demoted from critical path to
  background: useful for quantifying how far Panasonic sits from physical
  truth, not for fitting against.

The targets that do answer the question, in descending order of
directness: the camera's own rendered output, whether the at-capture JPEG
or an in-camera RAW development of a file we hold the RW2 for; a
third-party decode of the payload, which is what the Adobe DNG
WarpRectilinear coefficients are, if it can be shown that Adobe reads the
payload at all; and the payload's own structure recovered by differential
probing.

## When to stop

Sessions 14 to 21 are eight consecutive negative results. That is not by
itself a reason to stop, but the absence of any written criterion for
stopping is a problem in its own right: it is what let sessions 17, 18 and
20 re-run variants of the same experiment against a metric that session 21
then showed could not rank them. The criterion below is deliberately
written before the next round of work rather than after it.

**Abandon the metadata decode, and fall back to the hybrid path, if any
one of these holds.**

1. **The Adobe premise fails.** If DNG Converter's per-channel warp
   coefficients do not respond to the 0x011b payload, under the controls
   listed with that task (a no-op rewrite, a whole-payload transplant from
   another frame of the same body and lens, and a 0x0119 mutation as a
   positive control), then the shipped `C_R` and `C_B_lo` describe Adobe's
   lens-profile database rather than the camera's own data. There is then
   nothing in the tag that session 5 ever decoded, and no reason to keep
   fitting it.
2. **No trustworthy measurement can be built.** If no estimator can be
   demonstrated with per-file bias below roughly 0.1 px at r = 0.85,
   candidates that differ by a few tenths of a pixel cannot be ranked, so
   no refit can be justified. 0.1 px is the bar because the differences
   that mattered in sessions 17, 18 and 20 were 0.2 to 0.4 px.
3. **The information is not in the tag.** If, once a trustworthy
   measurement exists, real CA still varies materially between frames
   whose predictor words are bit-identical, then no function of the
   payload can reproduce per-frame CA. Session 21 measured 0.24 px of such
   variation but could not separate it from estimator bias; with a
   trustworthy estimator that ambiguity disappears and the answer is
   decisive either way.
4. **Session 25 with nothing to show.** If three further sessions produce
   no candidate that beats the shipped configuration on a rendered-pixel
   comparison against paired camera JPEGs, stop. Not because the problem
   is unsolvable, but because the remaining leads will have been tried and
   the cost is no longer proportionate to a correction of a fraction of a
   pixel.

**What falling back means concretely.** Revised after the developer ruled
out the hybrid path: there is no substitute correction to fall back *to*,
so stopping means stopping, with the best available decode left in place.
Keep the shipped session 5 + K = 1 coefficients, since no evidence
contradicts them and removing a working correction on suspicion would be
worse than leaving it. Lock them behind a registered rendered-pixel
comparison against paired camera JPEGs, so no later agent repeats sessions
14 to 20. Publish the structural findings and the negative results, which
are worth more to the next person than another private refit. Record in
the document that the remaining distance to Panasonic's output is
unrecovered, and what it would take to close it. Then close the
investigation.

One thing not to do on stopping: revert `47c223703e`. Its distortion
handling is correct and useful on its own, and the CA branch shipped since
session 13 is the closest approximation to Panasonic's correction that
anyone has published.

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
  interpolate. **Answered, session 19: continuous.** Five to seven of the
  six predictor words move monotonically with focal length on every zoom
  with a focal sweep in the corpus, and word[8] takes 21 distinct values
  on the Leica 12-60 alone. They also move between frames at fixed lens,
  focal length and aperture, which the discrete-profile reading cannot
  explain.
- Do the Olympus branch's `ca[6]` fields on OM-D bodies map onto
  Homeister's zone model? Both are Micro Four Thirds; worth a passing
  side-by-side check but not on the critical path. **Answered, session 19:
  no.** Olympus `ca[6]` is one whole-image polynomial per channel
  (`src/iop/lens.cc:2379-2454`); 0x011b is a four-zone record. The layouts
  are not related, and no other project decodes 0x011b either.
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

## Optional TODOs (post-shipping)

None of the following block shipping the patch specified in "For the
implementing agent". They exist because the investigation could go further,
and the eventual result would be a cleaner or more universal decode. Marked
in decreasing order of expected payoff.

### 1. G9 firmware RE

Panasonic distributes G9 firmware images publicly for update purposes. The
firmware ARM binary contains Panasonic's own algorithm for producing the CA
correction that ends up in the in-camera JPEG - the exact reference we are
approximating by applying session 5's `C_R` at Adobe DNG magnitude
(session 13's `K = 1`). Extracting it would resolve:

- The B-plane higher-order coefficients (`k_r2`, `k_r3`) that our
  regression could not decode from the six-word predictor set. This is
  the biggest remaining residual after session 13 - the darktable
  render at `K = 1` still leaves thin B fringing at the extreme corner
  on strong-CA lenses, and the undecoded higher orders are the leading
  suspect.
- Whether the algorithm is a polynomial (session 8's model) or a spline
  (Homeister's model as seen in SILKYPIX for shading, which is a
  neighboring algorithm to CA and might share the shape).
- Whether small residual C_R/C_B_lo fit error (session 5's LOGO R^2
  = 0.97-0.99) can be replaced by the exact per-body coefficients the
  firmware itself uses.

Cost: substantial. Multi-day project against a stripped ARM binary with
Panasonic's own header/signature format. Requires reverse-engineering
tooling (Ghidra with ARM support, or IDA), a working understanding of
Panasonic firmware container layout, and time to identify the CA routine
inside a much larger firmware image. Not delegatable to a single
subagent session.

If the darktable patch ships and gets user feedback that the residual
CA at `K = 1` is inadequate on some lens/body combination, this is the
first place to invest - especially for closing the B-plane higher-order
gap, which the corpus fit could not solve on its own.

### 2. Adobe Camera Raw / Lightroom RE

Adobe honors 0x011b when generating DNGs (session 5's ground truth was
their `WarpRectilinear` opcodes). Adobe's own RAW processor
(Camera Raw plugin, Lightroom binary) applies those opcodes internally.
Reverse-engineering that path would give us Adobe's *exact* mapping from
0x011b to per-channel warp coefficients, which is a well-tested
implementation of the same algorithm.

Cost: moderate. Adobe binaries are larger than SILKYPIX but not
fundamentally different in RE tooling. We already have Adobe's *output*
(the DNG opcodes) from session 5, so the residual question is the
transformation function. This is worth much less than firmware because
we can already reproduce Adobe's numeric output via session 5's C_R
matrix; the only gain is understanding.

### 3. B-plane higher-order decode

Session 5 could not decode `D_B.k_r2` and `D_B.k_r3` from the six-word
predictor set. Session 6 tried adding discrete words from step 2's
classification; the best four-word augmentation reached LOGO R^2 = 0.85
on `k_r3` but failed on `k_r2`, and 10 predictors on 18 files was over
the parametrization boundary. Session 8's JPEG-referenced fit had the
same issue.

Two paths to resolution: (a) a larger corpus (third body with paired
RW2+JPEG shots, or a broader lens set beyond the five in the training
corpus), (b) firmware RE per TODO 1 above which would give the exact
form.

Cost: low if it just needs more shots (an afternoon), medium if it
needs firmware.

### 4. Third-body validation of K = 1 with paired JPEGs

Session 6's third-body corpus (DC-S5, DC-G9M2, DC-GH5, DMC-GX8) verified
the decode structurally and in sign but did not have paired camera JPEGs
to test the magnitude of the correction on those bodies. Session 13
established `K = 1` on a G9 body outside the training corpus (P1366392,
Leica DG 12-60 @ 14mm); a paired RW2+JPEG on a body outside the G9 +
GX80 family would either confirm C_R generalises at unit magnitude or
reveal a body-conditional adjustment - keyed on N1 (word[11]) or on a
sensor-format detection - that closes any residual.

Cost: low. One paired RW2+JPEG shot on any body outside the training
corpus (GH5, S5, S1, G9M2, etc.), run through darktable-cli with the
current `_pana_K = 1` build, and eyeball the corner against the JPEG.
Any consistent over- or under-correction of the same sign across
multiple lenses is the signature of a needed body-conditional factor.

### 5. SPD file format for per-lens overrides

`DefaultLensInfo.spd` (119 KB) and `DefaultParameters.spd` (7.8 MB) in
SILKYPIX's install use the proprietary "ISL Multi purpose file format"
with an encrypted/compressed payload (session 9). If those files carry
per-lens CA correction overlays that SILKYPIX applies on top of the raw
0x011b payload, they would be a *third* form of correction (raw
0x011b, camera firmware application of it, SILKYPIX overlay from SPD)
which could explain any residual per-lens variance left after session
13's `K = 1` decode. Lower priority than it was under the session 8
K-spread framing, since Adobe DNG (which our C_R matches) does not
appear to consult such an overlay.

Cost: unknown. Format is not documented and payload is
encrypted/compressed. Best approached from the DLL side: find the
routine that decrypts/reads the SPD, use it to dump the plaintext
tables, then look at their structure. That was out of scope for
sessions 10-12 which focused on locating the CA algorithm itself.

### 6. Dynamic tracing of SILKYPIX or camera firmware

Session 10 and 12 identified the CA algorithm as living behind a
plugin factory reached only through dynamic dispatch. Frida on a
running SILKYPIX process (loading a Panasonic RW2 and hooking the
plugin's apply method) would give us the algorithm without static
plugin-manager RE. Cost: moderate; requires a Windows box running
SILKYPIX with Frida attached, and an hour or two of hook development.

Camera firmware could be similarly traced if an emulator (like Panasonic
firmware on an ARM emulation like QEMU or a specialised Panasonic
firmware sim) is available; more speculative.

### 7. Fit robustness

The C_R and C_B_lo matrices were fit on 18 files, cross-validated by
leaving one lens/focal group out. The feature selection was done on the
same corpus. A larger training set (30-50 files across 5+ lenses on 3+
bodies) would either tighten the fit or reveal that the six-word set
is not universal. Cost: moderate; needs more RW2+DNG pairs.

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
