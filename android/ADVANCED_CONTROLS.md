<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Additional native editing controls

## Camera white balance

For a supported RAW camera, the white-balance editor offers the original
camera preset names and their available fine-tuning values. Choose a preset,
adjust tuning, then apply the pair in one history step. The Current label
reports the verified applied preset; browsing another entry does not change it.
Undo/Redo restore both the native coefficients and displayed selection.

The catalog follows the desktop camera maker/model matching and preset order.
Only supported tuning values appear. Three-channel entries use the original
interpolator; four-channel sources require a verified four-channel entry.
Inactive channel bytes and calibrated coefficients above the manual slider
maximum are preserved. LDR, monochrome and unmatched-camera images do not get
an unrelated camera catalog. Manual gains and calibrated temperature/tint
remain separate controls.

## AgX and Filmic RGB

AgX exposes the original Blender-like, smooth and unmodified primaries actions,
reverse pre-mapping primaries, and reading the scene range from a usable active
exposure instance. These are complete native parameter updates with one Undo
step. Range and pivot edits also perform the original desktop coupling.

Filmic RGB exposes dynamic range scaling and automatic hardness. Grey-point
and range changes perform the desktop's associated relative-exposure updates
and hardness computation. Manual gamma/hardness controls appear only when the
corresponding automatic mode is off. Invalid coupled settings leave all fields
and history unchanged. Image-area automatic level/pivot pickers remain outside
this batch; displayed RGB is not substituted for module-input statistics.

## Placing masks on the photo

Create a native mask using the existing shape controls, enable **Place masks on
photo**, select a form and its **Mask centre** or **Clone source**, then tap the
photo. The supported centre shapes are circle, ellipse and gradient. Clone
source anchors are available where their native projection is defined; rotated
path/brush source handles remain in the numeric editor. A marker is drawn only
when the native handle projects into the current image. Photo letterboxing and
action bars do not become mask coordinates.

The original dimension-only pixelpipe maps through crop, orientation,
perspective, lens correction, pixel aspect and other active geometry. Taps map
back to original-image coordinates; both axes commit together in one native
mask history item. Successful placement refreshes the preview and projection.
Other edits, Undo/Redo, image or tool changes revoke that projection. The mode
does not draw, resize or rotate an outline or replace the existing node editor.

See [native verification](NATIVE_ENGINE_TESTS.md) for host, JNI and Android
compilation results and their device/RAW limitations.
