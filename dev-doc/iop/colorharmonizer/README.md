# color harmonizer

**Group:** color
**Pipeline position:** linear RGB, scene-referred
**Internal color space:** darktable UCS / JCH (perceptual)

---

## What it does

The color harmonizer nudges hues — and optionally chroma — toward a selected color palette: a set
of geometrically related hue angles called *harmony nodes*. The goal is to reduce chromatic discord
in an image. Colors that are "off-palette" are gently pulled toward the nearest node; colors
already on a node are left unchanged. A per-node saturation multiplier can additionally boost or
reduce the colorfulness of colors near each node.

---

## Color science

### Color space: darktable UCS JCH

All processing happens in **JCH**, the polar form of the **darktable Uniform Color Space 2022**
(Aurélien Pierre, 2022). darktable UCS was designed specifically for scene-referred color grading:
it uses a perceptually uniform hue-linear UV* plane derived from CIE xyY chromaticity, without the
CAM (Color Appearance Model) complexity or absolute-luminance dependence of HDR-targeted spaces. It
is the same space used by the color balance RGB and color equalizer modules.

| Channel | Symbol | Meaning |
|---------|--------|---------|
| Lightness | J | Normalized perceptual lightness (J = L* / L_white) |
| Chroma | C | Colorfulness, perceptually weighted |
| Hue | H | Hue angle in [-π, π] radians; stored as normalized [0, 1) |

The pipeline for each pixel is:

```
linear RGB (D50)
  → XYZ D50
  → XYZ D65          (CAT16 chromatic adaptation)
  → xyY              (CIE chromaticity + luminance)
  → darktable UCS JCH (hue-linear UV* plane → polar JCH)
  [H and C modified]
  → xyY              (inverse)
  → XYZ D65
  → XYZ D50          (inverse chromatic adaptation)
  → linear RGB (D50)
```

`J` (lightness) is never modified. `H` (hue) is always modified proportionally to angular
distance from the nearest node. `C` (chroma) is modified only when per-node saturation values
differ from 1.0.

### Hue normalization

Hue angles are stored as fractions of a full rotation ([0, 1] = [0°, 360°]). All angular
arithmetic wraps correctly at the 0/1 boundary.

### Gaussian hue attraction

Each pixel is pulled toward its **nearest** harmony node. The strength of that pull is weighted
by a Gaussian whose width is controlled by the pull width.

```
σ = pull_width × 0.5 / N                 (N = number of nodes)

w_i       = exp(−d_i² / 2σ²)             (d_i = circular distance from h to node i)
diff_i    = nodes[i] − h  (wrapped to [−0.5, 0.5])

nearest   = argmax_i(w_i)
hue_shift = w_nearest × diff_nearest
```

The shift is proportional to the **angular distance from the nearest node** scaled by the
Gaussian weight. This means:

- A pixel **exactly at a node** always produces zero shift (`diff_nearest = 0`), regardless of
  pull width. This is the key correctness invariant: node-colored pixels are never displaced.
- A pixel **far from all nodes** receives near-zero shift because `w_nearest ≈ 0`.
- The Gaussian `w_nearest` acts as a **proximity gate**: at wide pull widths it stays near 1
  across the entire hue circle so all pixels are attracted; at narrow widths it falls off quickly
  so only hues close to a node are affected.

Using the nearest-node's own difference (rather than a weighted average of all nodes' differences)
avoids an artefact where opposing nodes — e.g. in a complementary pair — partially cancel each
other and displace pixels that are already correctly positioned on one node toward the other.

### Per-node saturation

Each harmony node carries an independent saturation multiplier `s_i ∈ [0, 2]`. For each pixel,
the saturation delta is derived from the winning (nearest) node:

```
sat_delta = (s_nearest − 1) × w_nearest
```

Applied to chroma:

```
C_new = C × (1 + sat_delta × chroma_weight)
      ≈ C × s_nearest   (at the node, where w_nearest = 1 and chroma_weight ≈ 1)
```

At the node the result is exactly the configured saturation multiplier. Away from the node the
effect tapers to zero via the same Gaussian `w_nearest`. At `s_i = 1.0` (default) the saturation
channel is unaffected.

### The applied shift

```
cutoff        = t³ · 0.03                         t = neutral_protection ∈ [0, 1]
chroma_weight = C / (C + cutoff + ε)

pull    = pull_strength × chroma_weight
new_hue = h + hue_shift × pull
        = h + (w_nearest × diff_nearest) × pull

new_C   = C × (1 + sat_delta × chroma_weight)
```

The cutoff at `t = 1` is 0.03. Vivid colors in photographic images typically have C ≥ 0.3,
giving them a chroma weight ≥ 0.91 — almost the full effect — even at maximum neutral protection.
Only near-neutral colors (C < 0.03) are substantially shielded. The cubic exponent concentrates
slider sensitivity in the upper half of the range, where pastel and muted tones are progressively
included in the protected zone.

### Two-pass processing

To allow optional spatial smoothing of the correction field, processing is split into two passes:

**Pass 1 — map:** For each pixel, compute `hue_delta` and `sat_delta` from the current pixel's
hue and the harmony nodes. Store these in two per-pixel correction maps.

**Blur (optional):** If spatial smoothing is enabled, both maps are Gaussian-blurred. This softens
zone-boundary transitions visible in smooth spatial gradients (e.g. skies, skin). The blur sigma
scales with both the smoothing slider and the pull width.

**Pass 2 — apply:** For each pixel, re-read its hue and chroma from the input, look up the
(possibly blurred) `hue_delta` and `sat_delta`, and apply the final correction.

On GPU (OpenCL), the map and apply passes are separate kernels; the blur runs on the correction
buffer between them.

---

## Harmony rules

All node positions are offsets from the anchor hue. Angles are in degrees.

| Rule | Nodes | Node positions | Character |
|------|-------|----------------|-----------|
| **Monochromatic** | 1 | 0° | Single hue family |
| **Analogous** | 3 | 0°, −30°, +30° | Adjacent neighbors; naturalistic, cohesive |
| **Analogous complementary** | 4 | 0°, −30°, +30°, +180° | Analogous triad plus its opposite |
| **Complementary** | 2 | 0°, +180° | Direct opposites; maximum contrast |
| **Split complementary** | 3 | 0°, +150°, +210° | Anchor plus both neighbors of its complement |
| **Dyad** | 2 | −30°, +30° | Anchor is symmetry axis, not a node |
| **Triad** | 3 | 0°, +120°, +240° | Evenly spaced; balanced, colorful |
| **Tetrad** | 4 | −30°, +30°, +150°, +210° | Two dyad pairs; anchor is symmetry axis, not a node |
| **Square** | 4 | 0°, +90°, +180°, +270° | Four equally spaced hues |
| **Custom** | 2–4 | Freely placed | User-defined node positions |

> **Note on dyad and tetrad:** the anchor hue sets the symmetry axis of the pattern, not the
> position of a node. The palette is symmetric around the anchor. This matches the vectorscope
> harmony guide overlay.

---

## Controls

### Harmony rule

#### Vectorscope two-way sync
When enabled (default), any change to the harmony rule, anchor hue, or custom node positions is
immediately reflected in the vectorscope harmony overlay, and changes made in the vectorscope are
reflected back in the module. Disable to make adjustments without disturbing the vectorscope
display.

#### Import from vectorscope *(refresh icon, standard modes only)*
Imports the harmony rule and anchor hue currently configured in the vectorscope panel, then
switches the histogram panel to the vectorscope view.

#### Rule selector
Selects the geometric pattern of the target palette. When switching to *custom*, the current
rule's node positions are copied as a starting point.

#### Infer from image *(camera icon, standard modes only)*
Analyses the preview image's chroma-weighted hue histogram and automatically selects the harmony
rule and anchor hue that best fit the image's existing color distribution — i.e. the combination
that already covers the most chromatic energy and therefore requires the least correction.

The algorithm:
1. Builds a 360-bin hue histogram weighted by chroma (achromatic pixels are ignored).
2. Smooths it with three passes of a circular box filter to suppress noise.
3. Scores all 9 × 360 = 3,240 (rule, anchor) combinations at 1° resolution by computing what
   fraction of chromatic energy falls within the Gaussian attraction zones of each rule's nodes.
4. Sets the rule and anchor hue to the combination with the highest coverage score.

The result replaces the current rule and anchor. Use **pull strength** to control how strongly
the remaining off-palette hues are then pulled toward the detected palette.

#### Anchor hue
The primary hue from which all node positions are derived (not shown in *custom* mode). Expressed
as a normalized value displayed in degrees. An eyedropper is available to sample a color directly
from the image.

The color swatch strip below the slider shows the actual node colors for quick visual feedback.

#### Active nodes *(custom mode only)*
Number of active harmony nodes in custom mode (2–4). Only the first N node rows are shown and
active; the others are hidden.

#### Node hue sliders *(custom mode only)*
One row per active node. Each row shows a color swatch and a hue slider with an eyedropper. The
hue slider sets the node position on the hue wheel (0°–360°). Use the eyedropper to sample a
desired hue directly from the image.

---

### Effect controls

#### Pull strength
Global scale on the hue pull. At 0 nothing changes. At 1 the Gaussian proximity weight is the
only limit on how far each pixel moves. A pixel exactly on a node shifts by 0; a pixel at the
midpoint between two nodes is shifted by the Gaussian weight at that distance multiplied by its
angular gap to the nearest node.

Note: pull strength scales the **hue** correction only. The saturation correction (per-node
saturation multipliers) is applied independently, at full strength regardless of this slider.

#### Pull width
Scales the standard deviation σ of each node's Gaussian attraction zone. Range: 0.25–4.0.

- **< 1 (narrow):** the Gaussian decays quickly with distance; only hues very close to a node
  are attracted. The rest of the hue wheel is barely touched. Useful for images already close to
  harmonic, or when precise, surgical correction of specific hues is needed.
- **= 1 (default):** the Gaussian tapers to roughly 14 % at the midpoint between adjacent nodes —
  clean zone separation with smooth transitions.
- **> 1 (wide):** the Gaussian stays high across most of the hue circle; all pixels are
  attracted noticeably regardless of how far they are from a node. Useful for strongly discordant
  images or a painterly look.

Increasing pull width never displaces pixels that are already exactly on a harmony node — their
angular distance to the nearest node is zero, so their hue shift is always zero.

#### Neutral color protection
Shields low-chroma pixels from correction. The weight for each pixel is:

```
chroma_weight = C / (C + t³ · 0.03)
```

At C = 0 the weight is always zero regardless of the slider: fully achromatic pixels (pure
grays) are never touched. As C grows, the weight approaches 1. The slider sets how aggressively
low-chroma pixels are exempted: low values protect only near-absolute grays; high values extend
protection to muted and pastel tones. Even at the maximum, vivid colors remain largely unaffected.

Default: 0.50.

#### Smoothing
Applies a Gaussian blur to the correction maps (hue delta and saturation delta) before they are
applied to the image. This smooths the spatial transitions at zone boundaries — visible as subtle
colour steps in smooth gradients like skies or skin when adjacent regions fall in different
attraction zones.

The blur sigma scales with both this slider and the current pull width: wider attraction zones
span larger hue ranges and generate larger corrections, which can produce sharper spatial
boundaries when pixels near each other fall on opposite sides of a zone midpoint.

- **0 (default, off):** transitions are handled purely by the Gaussian interpolation in hue space.
  Maximum detail is preserved. Recommended for most uses.
- **0.1–0.5:** subtle spatial averaging to reduce colour noise in smooth areas.
- **> 1.0:** broad spatial blending; can create a painterly effect but may bleed corrections
  across image edges.

---

### Saturation section *(collapsible)*

One row per active harmony node. Each row shows a color swatch (the node's hue) and a saturation
multiplier slider.

#### Node saturation sliders
Saturation multiplier for colors near the corresponding harmony node.

- **100 % (default):** chroma is unchanged.
- **< 100 %:** desaturates colors near this node, proportionally to their Gaussian proximity.
- **> 100 %:** boosts chroma near this node.

The effect is weighted by the pixel's Gaussian proximity to the node (`w_nearest`) and further
modulated by **neutral color protection** so near-achromatic pixels are spared. At the node itself
(maximum weight) the chroma is multiplied by exactly the slider value.

---

## Usage guide

### Basic workflow

1. **Set the anchor hue.** Use the eyedropper to pick the dominant or most important color in
   the scene — a sky, a garment, a skin tone — or drag the slider manually while watching the
   swatch strip.

2. **Choose a harmony rule.** For portraiture, *complementary* (subject vs. background) or
   *analogous* (warm tones) are natural starting points. For landscapes, *triad* or *split
   complementary* often work well. Enable *vectorscope two-way sync* (under the harmony rule
   heading) to see the palette on the vectorscope while you choose.

3. **Raise pull strength slowly.** Start around 0.2–0.3. The effect is graduated; values
   above 0.6 are rarely needed for a convincing result.

4. **Adjust pull width if needed.** If many off-palette hues are not moving enough, widen the
   zones. If you want to correct only specific hue ranges without touching the rest, narrow them.

5. **Use neutral color protection to taste.** Raise it for images with many desaturated tones
   (architecture, faded film looks) to exempt pastels and muted tones from correction.

6. **Use the saturation section for palette polishing.** Once the hue pull is set, open the
   Saturation section to boost or calm chroma per node — e.g. boost the warm nodes and desaturate
   the cool ones for a cinematic split-tone look.

### Custom harmony mode

Use *custom* when none of the geometric rules match the image's intended palette.

1. Set the harmony rule to *custom*. The previous rule's node positions are copied as a starting
   point.
2. Use *active nodes* to select how many palette targets you need (2–4).
3. Drag each node's hue slider or use its eyedropper to position nodes at the desired palette
   hues.
4. Raise pull strength to pull off-palette colors toward the custom nodes.

Custom nodes are synced to the vectorscope as free-floating angle markers (the vectorscope shows
no standard rule name for custom palettes).

### Working with the vectorscope

With *vectorscope two-way sync* enabled, the vectorscope overlays the harmony guide on the
image's color distribution. Colors inside the guide arcs are on-palette; colors outside are
being corrected. Use this to verify the pull is moving in the intended direction.

To start from a palette already set in the vectorscope panel, click the *import from vectorscope*
button (refresh icon, next to the sync checkbox).

### Interaction with blending

The module supports parametric and drawn masking. Common uses:

- Apply harmonization only to the background by masking out the subject.
- Use a luminance mask to restrict corrections to midtones.
- Reduce opacity as a global intensity control on top of pull strength.

### Typical parameter ranges

| Goal | Pull strength | Pull width | Neutral color protection |
|------|--------------|------------|-------------------------|
| Subtle finishing touch | 0.1–0.25 | 1.0–1.5 | 0.2–0.4 |
| Moderate creative grade | 0.3–0.5 | 1.5–2.5 | 0.2–0.5 |
| Aggressive stylization | 0.5–0.9 | 2.5–4.0 | 0.0–0.3 |
| Fix specific off-hues only | 0.4–0.8 | 0.25–0.7 | 0.2 |

---

## Technical notes

- Lightness (`J`) is **never modified**. All radiometric accuracy is preserved.
- Hue correction is zero for any pixel whose hue already matches a harmony node, regardless of
  pull width or pull strength. This is a mathematical guarantee, not a threshold: the angular
  distance to the nearest node is zero, so the shift is zero.
- Chroma is modified only when at least one node has its saturation slider set away from 100 %.
  With all nodes at 100 %, the saturation channel passes through unchanged.
- OpenCL acceleration is supported; the GPU path is numerically equivalent to the CPU path.
- Hue is undefined for fully achromatic pixels (C = 0). These pixels are unaffected by design:
  the chroma weight drives to zero regardless of other settings.
- darktable UCS is designed for scene-referred normalized data and does not depend on absolute
  luminance, making it naturally suited to darktable's pipeline. Its hue axis was explicitly
  constructed to be perceptually linear (equal angular steps correspond to equal perceived hue
  differences), which is the key property exploited here.
