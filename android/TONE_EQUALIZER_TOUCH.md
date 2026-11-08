<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Tone equalizer: adjusting from the photo

The desktop's on-image wheel control selects an input tone and changes nearby
exposure bands smoothly. On a phone, use the photo to select the tone and keep
the adjustment controls below it, where the finger does not obscure the image.

1. Select and enable **tone equalizer** in **Light / Color**.
2. Turn on **Adjust on photo**, then tap the desired area. A crosshair marks the
   point. **Select photo centre** is also available for keyboard/accessibility
   input and when precise tapping is difficult.
3. Drag the horizontal exposure slider or press **−0.1 EV / +0.1 EV**. Each
   action immediately requests the selected output exposure and refreshes the
   preview. **Selected tone** follows the current target while processing and
   then synchronizes to the actual native result. There is no Apply button or
   separate adjustment value.

Similar input tones across the image change together. This is a tonal
adjustment, rather than a spatial brush selection. Each drag is one Undo step;
individual fine-button presses use distinct gestures. While a render is in
progress, the slider and fine buttons remain available and newer absolute
targets replace pending work, preserving the final requested value.

The point remains selected after a tone adjustment so that it can be adjusted
again. Disabling the tool or
switching images, instances or categories leaves photo-selection mode. Other
edits and Undo/Redo clear the sample. Touches in the fitted image's letterbox
area or action bar, and drags across the image, do not select a tone.

The original processor supplies the filtered input luminance mask, including
the current mask/filter settings. A dedicated original preview pass samples
that mask with the desktop's 3×3 weighted kernel. The final preview coordinate
is transformed back through subsequent geometry into the selected module's
input ROI, using the last successful preview's resolution. Display RGB values
are never used to estimate the tone.

The native grouped edit calls the desktop's own Gaussian interpolation and
solver on a private copy of the parameters and validates the nine gains. The
Gaussian centre uses the processor's actual `[-8,0] EV` domain, including its
boundary bands for brighter or darker mask samples. Previously an unclamped
centre could give every band an almost-zero weight and make the edit invisible.

The mask's input exposure stays fixed. **Selected tone** instead evaluates the
actual quantized correction LUT and adds its correction to that input. A target
is fitted using private solver passes, committing once per update; successive
updates of one gesture replace the latest session history snapshot. Sampling
and output queries change neither parameters nor history. The ViewModel binds
the selected point to its image session, module instance and stable selection
ID; stale callbacks cannot apply it to another image, and its own preview
updates do not discard a continuing gesture. A
failed render after a native edit marks the retained preview as out of date,
disables photo selection, and offers a read-only preview retry.

See [native verification](NATIVE_ENGINE_TESTS.md) for the exact validation
scope. Compiling Compose component tests does not establish Android execution.
