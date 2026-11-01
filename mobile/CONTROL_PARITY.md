<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Mobile editing control coverage

The Android single-image editor now presents native sliders, precise inputs,
choice menus, switches, text fields, document imports and action buttons for the
ported tools. A successful edit updates the original module/history and renders
through the original darktable pixelpipe. This is broad tool coverage, not a
claim that every desktop widget, picker or canvas gesture has been ported.

The reviewed source policies contain **511 exact scalar fields across 77
modules** and **37 fixed-array families plus four sensor black levels**, allowing
up to **440 array components across 18 modules**. Together these policies cover
951 possible numeric field addresses across 81 modules. These are source-policy
counts: active color-checker patches, sensor type and current state determine
which controls are advertised for a particular image. They exclude dedicated
curve nodes, zone partitions, blending, masks, per-form retouch/liquify settings,
text/file fields and action buttons. An action button is not counted as a numeric
adjustment. Installed modules and runtime metadata remain authoritative.

Exact identities, types, hard bounds, desktop soft slider ranges and coupled
checks are recorded in the [scalar policy](core/dt_mobile_parameter_policy.h),
[additional scalar policy](core/dt_mobile_extended_parameter_policy.h) and
[fixed-array policy](core/dt_mobile_array_policy.h). Structural counts, padding,
opaque LUTs and unreviewed fields cannot be written through the ordinary numeric
API. Reviewed fixed-capacity selectors such as retouch wavelet scales use explicit
bounds and coupled checks.

## Tool controls

| Original tool family | Native mobile interaction |
| --- | --- |
| Exposure and tone mapping | Exposure/black/compensation; basic adjustments; tone equalizer EV bands and masking settings; filmic/filmic RGB, AgX, sigmoid, tone/global-tone mapping, local contrast, shadows/highlights, relight and input gamma controls |
| Color grading and calibration | RGB/legacy color balance, channel mixers, color equalizer hue sectors, color harmonizer nodes, active color-checker target Lab patches, monochrome filter coordinates, color correction/contrast, colorize, split toning, vibrance, velvia, low-light and negative inversion controls |
| Detail and denoising | Profiled denoise settings and fixed bands, diffuse/sharpen, wavelet equalizers, RAW denoise, non-local/bilateral denoise, sharpen radius/amount/threshold, high/low pass, blur, soften, bloom, grain, chromatic-aberration correction, defringe, haze removal and censorize controls |
| Sensor and optics | RAW black/white levels, source-compatible demosaicing/highlight methods, hot-pixel controls, mode-aware lens correction with calibrated camera/lens selection, shooting settings, source capability/status and manual vignette, pixel aspect ratio and input/output intent/gamut settings |
| Geometry and framing | Crop bounds batch, native-input-aware aspect presets and orientation drafts, perspective rotation/shift/shear/lens-model parameters, legacy crop rotation, image orientation, canvas expansion, borders and frame colors |
| Finishing and external assets | Vignette/graduated-filter settings, watermark text/font/template and placement, 3D LUT import/settings, external raster mask import/channel selection; overlay image import and placement/settings |

Continuous controls provide sliders and precise input. Native enums and reviewed
integer menu values use choice menus; booleans use switches. Dragging submits one
native operation on release. Reset uses original image-specific defaults, and a
tool-level settings reset preserves its enabled state, masks and blending.
Dependent crop and white-balance values use an atomic Apply operation. Tone
equalizer's photo-selected adjustment also updates its nine bands in one Apply.

The engine intersects introspection bounds with reviewed domains and validates
storage types, exact enum membership and cross-field constraints before history
changes. Examples include exposure-derived white/black separation, ordered
levels/curve abscissae, positive color-balance denominators, filmic target
luminance order, AgX target/gamut constraints, retouch scale relationships and
RAW white above all four black levels. Readable imported values outside these
reviewed write domains are not silently clipped or made writable.

RAW preparation and demosaicing controls are omitted when the input cannot use
those original operations. Demosaicing choices follow Bayer, X-Trans, four-Bayer
and true-monochrome source capabilities. Highlight modes follow the original
image-specific menu. Enum sentinels and GUI-only automatic-detection entries are
not offered as processing choices. Other settings can still depend on a module's
selected mode; exposing a stored setting does not make every mode use it.

## Dedicated native operations

| Feature | Available interaction | Further desktop interaction |
| --- | --- | --- |
| Tone equalizer | Photo-point sampling of the original filtered mask; exposure draft slider and fine buttons; original smooth nine-band update in one Apply/Undo step | Full desktop graph/picker presentation and mask overlay |
| Base/RGB/Lab tone/color-zone curves | Native samples and interpolation, drag/numeric nodes, add/remove, native channel defaults; RGB linked/independent options | Further module-specific automatic sampling and graph gestures |
| Levels and fixed bands | Ordered black/middle/white components, channel vectors and frequency-band sliders with native bounds | Full desktop graph/picker presentation |
| Zone system | Native zone-count reset, ordered output boundaries and automatic-position switches | Desktop strip dragging/pickers |
| White balance | Image settings, active gain channels and calibrated Kelvin/tint pairs | Full camera preset/tuning browser and spot picker |
| Lens correction | Original image-default initialization, matched camera/lens catalog selection, focal/aperture/distance/crop settings, native autoscale and source-aware correction/manual-vignette controls | Calibration absent from the installed database, unsupported embedded opcodes and unverified camera-specific geometry |
| Crop/perspective | Atomic crop bounds/reset; aspect presets fitted to the original crop input ROI, portrait/free drafts; perspective scalar transforms | Uncropped-photo touch overlay and automatic line fitting |
| Blending | Original opacity, supported modes/reverse/fulcrum, preceding raster-source selection/inversion; input/output parametric channels with four ordered thresholds, polarity, boost/reset and mask refinement | Live channel histograms, automatic sampling and further desktop graph gestures |
| Drawn masks | Add/delete native circle, ellipse, gradient, closed Bézier path and brush forms; node/handle/feather controls, brush hardness/density, opacity/inversion and five native group operations | Freehand stroke capture, direct canvas manipulation and full mask-manager workflows |
| Spots/retouch | Native clone/repair shapes and paths, retouch brushes, source coordinates, per-form algorithm/blur/fill controls, wavelet selectors and settings reset | Complete desktop repair canvas, freehand gestures and scale-copy workflow |
| Liquify | Native standalone warp nodes, position/radius/strength/hardness/type and add/delete | Linked strokes, line/curve paths and the full interactive mesh canvas |
| External files | System document import into retained session assets; native raster PFM/PNG, LUT Cube/3DL/Hald-PNG and overlay PNG/JPEG/TIFF inputs | Optional compressed LUT formats, wider overlay-format verification and complete installed asset browsers |
| Instances/history/export | Default instances, independent parameters, session undo/redo and installed native export options | Copy/delete/reorder instances, styles/presets and complete optional codec parity |

Parametric controls follow the selected native Lab, display RGB or scene RGB
blend space. The editor groups input/output channels, shows the applied threshold
shape and constrains each of its four sliders against its neighbors. Channel
enable/inversion, gain and reset operate on the original blend parameters.
Mask refinement includes native blur, feathering guide/radius, brightness,
contrast and RAW details where the input supports them. Controls are advertised
only for applicable active mask modes; raster and parametric modes retain the
original exclusive source semantics.

Path and brush editing uses native form identities and up to 64 nodes per form.
The form/node selectors show one editable set at a time, and discard old drafts
after insertion, deletion or history restoration. Closed paths keep at least
three nodes and brushes at least two. Subsequent drawn group members offer the
original union, intersection, difference, exclusion and sum operations; the first
member initializes the group. Spots supports clone paths, while retouch supports
both paths and brushes. The diagram shows applied input-space coordinates and
handles separately from the processed photo; it is not a photo-aligned touch
canvas or a rasterized feather preview.

Crop aspect presets fit a draft rectangle using the original pipeline's input
dimensions for that crop instance, including preceding geometry changes.
Orientation and preset selection remain drafts until the complete bounds are
applied atomically. Manual edge edits return to free mode. Ratios are not
estimated from the already-cropped preview.

Lens correction resolves image-specific native defaults before marking parameters
as edited and retains a valid crop factor when a body is unmatched. Its profile
selector and numeric controls follow the selected method and actual source or
Lensfun modifier capabilities. Embedded CA-only DNG warps are not reported as
fisheye geometry, and RAW GainMaps are distinct from lens-vignette controls.
The bundled database has no HERO12 Black profile, and no older GoPro profile is
substituted automatically. Manual vignette remains independent of a profile;
camera-specific geometry still needs appropriate calibration and source-file
verification. See [lens correction](../android/LENS_CORRECTION.md).

File imports validate the original buffer layouts, UTF-8 byte limits, readable
regular paths and supported extensions before changing the complete native tuple.
Raster imports update path and basename together. Overlay imports use the
original image importer and image-ID/filename tuple, reject self/cyclic references
and synchronize native overlay dependencies with edit/history state. LUT imports clear stale
compressed-LUT state and use the original decoder, accepting a retained absolute
cache path without changing the existing desktop relative-folder route. Imported
assets remain available for the open session, including history restoration.

Internal pipeline/diagnostic modules, including the `mask_manager` history
sentinel, are hidden from the editing-tool list while their native history
identities remain intact. Its private `dummy` field is not a mask editor.
The legacy `colortransfer` module has no editing controls in the current desktop
GUI either: upstream retains only an explanation and the ability to switch old
edits off. Android follows that behavior and points to the current color-mapping
tool instead of exposing private acquisition flags or cluster counts.

## Remaining scope and verification

Automatic picker operations and GUI-maintained parameter transformations are not
all covered by independent scalar writes. Examples include exposure deflicker
metering, filmic automatic hardness/range helpers, AgX range/pivot assistance,
full noise/white-balance preset selection and source-patch acquisition. The native
manual parameter controls remain available; unported automatic controls are not
represented by buttons which merely store an otherwise unused value.

Freehand mask stroke capture, direct photo-canvas editing, live channel
histograms, automatic pickers, linked liquify strokes and the advanced workflows
listed above remain. Overlay selection uses its original import/dependency
operations; exact tested formats and history/pixel results belong to the
verification record rather than a general desktop-parity claim.

The dependency-only Android build still has the processing engine disabled.
These controls need an engine-enabled runtime with matching plugins/data; adding
widgets does not enable processing in that build. See [Android build/setup](../android/README.md),
[port status](dependencies/PORTING_STATUS.md) and
[native verification](../android/NATIVE_ENGINE_TESTS.md) for the exact tested
fixtures and platforms. Source checks, compilation, actual pixel/history tests,
Compose interaction and physical-device performance establish different scopes.
