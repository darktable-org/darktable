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
- Contrary to the initial assessment in this thread, the RW2 does carry
  chromatic-aberration correction data. It lives in
  `Exif.PanasonicRaw.0x011b` (64 bytes, 32 x int16 LE, structurally two
  DistortionInfo-shaped blocks). darktable has never parsed it. ExifTool
  documents the tag as "chromatic aberration correction" but ships no decoded
  structure; RE reference is ExifTool forum thread 9366.
- Follow-on work is (a) confirm the regression scope, (b) reverse-engineer
  0x011b, (c) extend the Panasonic embedded-metadata path in
  `_init_coeffs_md_v2` to produce non-identical R/B coefficients.

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

`0x011b` on the sample, 64 bytes = 32 x int16 LE:

```
block A (16 int16, mirrors 0x0119 layout):
  c480 b839 1200 6e02 0500 e2ff 0600 01f1
  0f00 0800 b201 0600 a80c fc01 afdd 81d6

block B (16 int16):
  0100 1801 8808 4404 8888 0007 d4f6 2002
  8002 1105 c818 1167 6508 f8fc 0040 63f4
  47a9 abe5
```

The 32-byte block shape and its position adjacent to DistortionInfo are the
main structural hints. DistortionInfo itself is a 16 x int16 record with
checksums at [0..1], the on/off nibble in low byte of [7], polynomial
coefficients at [4], [8], [11], a scale at [5] and a constant `DistortionN`
at [12]. If 0x011b follows the same layout twice (once for R, once for B)
that is what a follow-on agent should assume as the starting hypothesis and
then falsify.

Upstream reference (ExifTool `Image::ExifTool::PanasonicRaw`, `Main` tag
table):

```perl
# 0x11b - chromatic aberration correction (ref 3) (also see forum9366)
```

`ref 3` and `forum9366` are the ExifTool documentation reference and the
u.exiftool.org thread that originated the identification. Neither ships a
decoded field layout; ExifTool leaves the tag as an opaque undefined-type
blob.

## Where the code needs to change

For the follow-on agent, this is the minimum set of touchpoints, all
concentrated:

### 1. `src/common/image.h`

Extend `dt_image_correction_data_t::panasonic` with CA fields. Draft:

```c
struct {
  float a, b, c;
  float scale;
  gboolean has_ca;
  // per-channel polynomial coefficients, same normalization as a/b/c/scale
  float ca_r_a, ca_r_b, ca_r_c;
  float ca_r_scale;
  float ca_b_a, ca_b_b, ca_b_c;
  float ca_b_scale;
} panasonic;
```

Exact field set depends on the RE outcome (see next section).

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

## Reverse-engineering plan for 0x011b

None of what follows has been executed. It is a plan for the next agent.

### Corpus

One RW2 is not enough to disambiguate the field layout. Collect at minimum:

- multiple lenses on the same body (variation in CA coefficients, fixed
  camera-side scaling),
- multiple focal lengths on a zoom (progression of coefficients),
- one file with in-camera correction turned off (control: what does the tag
  look like when there is nothing to correct? Is bit [7] low nibble the same
  on/off flag as DistortionInfo?),
- one round-tripped TIFF/DNG that darktable exported after reading and
  writing the tag through `dt_exif_read_blob` (regression cover once the
  reader lands),
- ideally one Panasonic body without a distortion tag at all (RW2 files
  where 0x0119 is absent; verify 0x011b is also absent, or, if present,
  what layout it has).

Ask upstream (issue tracker) for a corpus if the follow-on agent does not
have Panasonic hardware.

### Cross-check tools

- SILKYPIX or Panasonic's own converter: knows what these fields mean by
  construction. Compare the corrected output against a null-CA baseline.
- RawTherapee: as of writing does not decode 0x011b either (verify against
  current `rtengine/` source), but its GUI has manual CA sliders that let
  the follow-on agent match against SILKYPIX visually.
- ExifTool `forum9366` and `PanasonicRaw.pm` history in the ExifTool git
  tree; someone in that thread may already have named the fields even if
  the code does not.

### Working hypothesis (falsifiable)

Two 16-int16 blocks, each mirroring DistortionInfo:

| position | DistortionInfo (0x0119)     | 0x011b block A hypothesis (R) | 0x011b block B hypothesis (B) |
|---------:|:----------------------------|:------------------------------|:------------------------------|
| [0,1]    | checksums                   | checksums?                    | checksums?                    |
| [4]      | b coeff (r^5)               | ca_r b?                       | ca_b b?                       |
| [5]      | scale (raw, /32768)         | ca_r scale?                   | ca_b scale?                   |
| [7] low  | on/off nibble               | on/off nibble?                | on/off nibble?                |
| [8]      | a coeff (r^3)               | ca_r a?                       | ca_b a?                       |
| [11]     | c coeff (r^7)               | ca_r c?                       | ca_b c?                       |
| [12]     | DistortionN = 2500 (const)  | ?                             | ?                             |

Falsify by checking whether:

- [12] is 2500 in 0x011b or a different sentinel (on the sample it is 8776
  and 25668, so *not* 2500; the analogy may only be partial).
- [7] low nibble tracks with in-camera CA-correction setting when toggled.
- Blocks A and B differ in ways consistent with red-vs-blue chromatic shifts
  (typically opposite signs, similar magnitudes).

The sample values decoded as int16 LE are printed in the "What the RW2
actually contains" section above; use them as the first data point.

### Rendering test

Once a candidate decoding exists, prototype it in a scratch Python script
that computes `Ru / f_r(Ru)` and `Ru / f_b(Ru)` and demosaics with
per-channel radial resampling. Compare against SILKYPIX output. Only once
that agrees within a small dE margin should the C code be written.

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

- Panasonic G9, LUMIX G 42.5/F1.7, 5184x3888, DistortionCorrection = On,
- DistortionInfo (0x0119) present and parseable,
- 0x011b present, 64 bytes, matches the raw dump above.

Do not check the file into the repo. Use it locally for the RE work and cite
it in reports without committing binary blobs.

## Open questions

- Is 0x011b present *only* when in-camera CA correction is enabled, or
  always? The DistortionInfo tag uses an explicit on/off flag at byte [7]
  low nibble; we do not know yet whether 0x011b uses the same convention or
  is unconditionally present.
- Does 0x011b vary with focal length on a zoom lens? If yes, is the
  variation continuous (implying the camera has a per-focal-length LUT
  baked into the firmware) or does it snap between a small number of
  discrete profiles?
- Do the Olympus branch's `ca[6]` fields on OM-D bodies map onto 0x011b's
  layout at all? Olympus and Panasonic share Micro Four Thirds heritage;
  worth diffing the two parsers side-by-side.
- Should the fine-tune sliders (`tca_r`, `tca_b`, `tca_override`) work on
  the metadata path the same way they work on the Lensfun path?
- Would a first-cut *interim* fix (reverting the Panasonic auto-select of
  embedded-metadata mode) be worth landing in this checkout ahead of the
  RE work? It stops the bleeding for users, buys time for the RE. If the
  follow-on agent has a maintainer's sign-off, ship the interim first as a
  separate commit, then the feature.

## What is *not* claimed here

- That the working hypothesis for 0x011b's layout is correct. It is a first
  guess anchored on the shape of the neighboring 0x0119.
- That the sample file's values are representative of other Panasonic
  bodies. G9 firmware version is `0.2.7.0` per makernote; other bodies may
  write differently.
- That the workaround (switch to Lensfun) restores *exactly* what the user
  had before `47c223703e`. It restores Lensfun-based TCA correction, which
  is what was actually happening before. If the user reports the fringing
  is subtly different, that is expected; they were always seeing Lensfun,
  not Panasonic, correction.
- That `47c223703e` should be reverted. The distortion metadata handling
  it adds is correct and useful on its own; the regression is in the
  auto-selection policy, not in the distortion math.

## References

- Regression-introducing commit: `47c223703e`, "Add Panasonic RW2 embedded
  lens distortion correction", 2026-07-27.
- Panasonic DistortionInfo layout notes:
  https://github.com/trou/panasonic-rw2/blob/master/notes.txt (cited in
  `47c223703e`).
- ExifTool `Image::ExifTool::PanasonicRaw.pm`, `Main` tag table, comment at
  tag 0x11b: `# 0x11b - chromatic aberration correction (ref 3) (also see
  forum9366)`.
- ExifTool forum thread 9366 (u.exiftool.org): originator of the 0x011b
  identification. Read for any partial layout notes before starting fresh
  RE.
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
