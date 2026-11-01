<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Tone equalizer: adjusting from the photo

The desktop's on-image wheel control selects an input tone and changes nearby
exposure bands smoothly. On a phone, use the photo to select the tone and keep
the adjustment controls below it, where the finger does not obscure the image.

1. Select and enable **tone equalizer** in **Light / Color**.
2. Turn on **Adjust on photo**, then tap the desired area. A crosshair marks the
   point. **Select photo centre** is also available for keyboard/accessibility
   input and when precise tapping is difficult.
3. Adjust the selected tone using the horizontal exposure slider or the
   **−0.1 EV / +0.1 EV** buttons. The displayed adjustment is a draft.
4. Tap **Apply** to update the original nine exposure bands in one history
   step. Similar input tones across the image change together. This is a tonal
   adjustment, rather than a spatial brush selection.

The point remains selected after a tone adjustment so that it can be adjusted
again. **Cancel** discards only the pending adjustment. Disabling the tool or
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
solver on a private copy of the parameters, validates the nine gains, and
commits one history item. Sampling itself changes neither parameters nor
history. The ViewModel binds the selected point to its image session, module
instance and preview; stale callbacks cannot apply it to another image. A
failed render after a native edit marks the retained preview as out of date,
disables photo selection, and offers a read-only preview retry.

See [native verification](NATIVE_ENGINE_TESTS.md) for the exact validation
scope. Compiling Compose component tests does not establish Android execution.
