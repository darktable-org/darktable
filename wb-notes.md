# Current state
darktable is an application to process raw photos. It executes processing in a pipeline.
An early step in the pipeline is to apply white balance. In most applications, that means using multipliers to shift camera RGB values representing neutral areas (such as a grey card placed in the picture) to R = G = B. This is possible in darktable too, via the 'white balance' (temperature.c) module. However, darktable also makes it possible to delay this until a later module, 'color calibration' (channelmixerrgb.c), which uses chromatic adaptation tranform (CAT, e.g. Bradford or CAT 16) to white balance the scene.

## Modes of operation

### legacy

In this mode, temperature.c is responsible for white balancing; channelmixerrgb.c, if used, is normally used without chromatic adaptation transform, as a simple RGB channel mixer.
In this mode, temperature.c provides 3 tabs, allowing the user to use the in-camera EXIF multipliers img->wb_coeffs / dev->chroma->as_shot (DT_IOP_TEMP_AS_SHOT), picking neutral WB from a spot (DT_IOP_TEMP_SPOT), or directly setting multipliers either via RGB sliders, or via temperature/tint sliders (DT_IOP_TEMP_USER). It is also possible to use camera-specific presets, but the mechanism is the same: camera RGB is multiplied component-wise by the user's resulting RGB multipliers, and the aim is to achieve R=G=B for grey/neutral areas. The chosen RGB coefficients are placed in dev->chroma->wb_coeffs. dev->chroma->late_correction is not set in these cases.

This mode helps demosaicing and highlight recovery, because neutral areas have R=G=B, and colours are mostly correct. However, simply neutralising camera RGB to R=G=B via multipliers is not proper chromatic adapation.

The 'input color profile' (colorin.c) takes white-balanced (neutralised) input and converts it to the pipe's working space (normally, D50-based Rec 2020).

### modern

In the modern workflow, channelmixerrgb.c is responsible for setting the final white balance. temperature.c is either used in 'camera reference' mode, employing multipliers that would be correct for D65, or the in-camera EXIF WB multipliers. Only in the latter case, the dev->chroma->late_correction flag is set.

In colorin.c, either the D65-balanced RGB is converted to the pipeline's working space (dev->chroma->late_correction == false), or the in-camera multipliers are undone, and a conversion that gives the same result as using the D65 multipliers is performed.

Finally, colours are fixed in channelmixerrgb.c: in 'as shot in camera' mode, the EXIF multipliers are used to find the actual scene illuminant, or the user may select a neutral area and sample that. Chromatic adaptation is performed to remap colours to the pipeline's D50 white point.
It is important to note that the in-camera mode may shift to daylight or Plankian illuminant or custom mode, depending on the decision of the algorithm.

It is normally an error (warnings are displayed) to use CAT with user-selected multipliers in temperature.c. However, there is one special case: if the camera matrix is wrong, and the resulting D65 multipliers are wrong, we must process the image using the multiplers from temperature.c as the correct D65 coefficients.

The benefit is that the input profile matrix works on data it was designed for (D65 WP); the downside is that in D65 ('camera reference') mode, demosaic doesn't see neutralised data, which causes artefacts, and if highlight reconstruction uses R=G=B for clipped raw pixels, the white/neutral will be shifted later, resulting in tinted highlights. 'as shot to reference' aims to fix that, by using our best guess of correct multipliers to neutralise the scene (from EXIF), and reverting to D65 in colorin.c.

# Aim

The purpose of this change is to make sure it is possible to use temperature.c's user-set multipliers the same way as in-camera multipliers are used, with dev->chroma->late_correction set, ensuring the user can fix WB early on (instead of using the camera's guess at coefficients).

# Solution
- A new checkbox in temperature.c, allowing setting dev->chroma->late_correction in 'user' modes. For convenience, this is to be set automatically, when the user uses CAT.
- Switch uses of the as-shot EXIF multipliers to user's multipliers, where needed.
- Add a new illuminant constant DT_ILLUMINANT_FROM_WB
- In channelmixerrgb.c, use this similarly to DT_ILLUMINANT_CAMERA, but reading coefficients from wb_coeffs, not from as_shot. Also, when in this mode, unlike DT_ILLUMINANT_CAMERA, we must not switch automatically to daylight/Plankian/custom illuminant, as illuminant calculations must follow the **current latest** multipliers set in temperature.c

# summary

# Darktable White Balance Workflow: Technical Summary & Refactoring

## 1. Mathematical Foundations & Data Structures

The pipeline relies on transforming Raw Sensor Data into the Pipeline Working Space (Lab/XYZ, D50). This process involves two distinct matrices and several sets of coefficients.

### A. The Matrices
1.  **Characterization Matrix ($M_{char}$)**
    *   **Definition:** Maps **XYZ (D65)** $\to$ **Raw Camera RGB**.
    *   **Source:** Embedded in DNG (`img->d65_color_matrix`) or from the library database (`img->adobe_XYZ_to_CAM`).
    *   **Concept:** Describes how the sensor "sees" D65 white.
2.  **Input Profile Matrix ($M_{profile}$)**
    *   **Definition:** Maps **D65-balanced Camera RGB** $\to$ **Pipeline XYZ**.
    *   **Location:** Used in `iop/colorin.c`.
    *   **Derivation:** $M_{profile} = M_{char}^{-1} \times \text{diag}(RGB_{native})$, where $RGB_{native}$ is the sensor response to D65 ($M_{char} \times (1,1,1)^T$).
    *   **Constraint:** This matrix **strictly expects** input data to be white-balanced to D65 (where $(1,1,1)$ represents D65).

### B. The Coefficients (Multipliers)
1.  **As-Shot ($WB_{Exif}$)**: Raw multipliers stored in EXIF.
2.  **D65 Reference ($WB_{D65}$)**: Calculated multipliers required to turn the sensor's D65 response ($RGB_{native}$) into neutral $(1,1,1)$.
    *   Math: $WB_{D65} = 1.0 / RGB_{native}$.
3.  **Applied ($WB_{Applied}$)**: The multipliers currently applied by the `temperature` module (`dev->chroma.wb_coeffs`).

---

## 2. Workflows (State Before Refactoring)

### A. Legacy Workflow (Display-Referred)
*   **Method:** The user adjusts WB in `temperature`. `channelmixerrgb` (Color Calibration) is off/identity.
*   **Flow:**
    1.  `temperature`: Applies $WB_{Applied}$. Input becomes neutral (e.g., Tungsten looks white).
    2.  `colorin`: Applies $M_{profile}$.
*   **Issue:** $M_{profile}$ expects D65-balanced data. Since the data is Tungsten-balanced (to look white), the resulting XYZ white point is shifted away from the pipeline standard (D50). This "bakes in" the WB early.

### B. Modern Workflow: Camera Reference (Scene-Referred)
*   **Method:** `temperature` is locked to "Camera Reference". `channelmixerrgb` handles adaptation.
*   **Flow:**
    1.  `temperature`: Applies $WB_{D65}$.
    2.  `colorin`: Receives D65-balanced data. Applies $M_{profile}$. Result is correct D65 XYZ.
    3.  `channelmixerrgb`: Uses a CAT (Chromatic Adaptation Transform) to adapt from the Scene Illuminant to D50.

### C. Modern Workflow: "As Shot" to Reference
*   **Goal:** Allow `temperature` sliders to show "As Shot" values (UX) while keeping the pipe D65-invariant (Math).
*   **Mechanism:** Uses a flag called `late_correction`.
*   **Flow:**
    1.  `temperature`: Applies $WB_{Exif}$. Sets `late_correction = TRUE`.
    2.  `colorin`: Checks `late_correction`. Calculates a normalization factor: $Norm = WB_{D65} / WB_{Exif}$.
    3.  **The Math:** $RGB_{out} = RGB_{in} \times WB_{Exif} \times Norm = RGB_{in} \times WB_{D65}$.
    4.  Result: The data entering the matrix is D65-balanced, satisfying $M_{profile}$ requirements.

---

## 3. The Problem

The "As Shot to Reference" logic (Workflow C) was hardcoded to assume that if `late_correction` is on, the applied coefficients are exactly $WB_{Exif}$.

**Scenario:** A user wants to use the Modern Workflow (CAT via `channelmixerrgb`) but the "As Shot" WB is wrong. They use the **Spot WB** tool in the `temperature` module on a grey card.
1.  `temperature` applies $WB_{User}$.
2.  `colorin` (old logic) divides by $WB_{Exif}$.
3.  **Failure:** $WB_{User} \times (WB_{D65} / WB_{Exif}) \neq WB_{D65}$.
4.  **Result:** `colorin` receives data that is **not** D65-balanced. The input matrix produces incorrect colors.
5.  **Secondary Issue:** `channelmixerrgb` set to "As Shot" reads $WB_{Exif}$ to calculate the CCT, ignoring the user's spot-picked coefficients.

---

## 4. The Solution: User-Provided Late Correction

We enabled a workflow where the user can set custom coefficients in `temperature`, and the pipeline treats them as the "Reference" for `color calibration`.

### A. Logic Changes
1.  **Generalize Late Correction (`iop/colorin.c`, `iop/highlights.c`)**
    *   Instead of dividing by $WB_{Exif}$, these modules now divide by `dev->chroma.wb_coeffs` ($WB_{Applied}$).
    *   **New Math:** $RGB_{out} = RGB_{in} \times WB_{Applied} \times (WB_{D65} / WB_{Applied}) = RGB_{in} \times WB_{D65}$.
    *   This ensures `colorin` always gets D65-balanced data, regardless of what the user set in `temperature`.

2.  **New Illuminant Mode (`iop/channelmixerrgb.c`)**
    *   Added `DT_ILLUMINANT_WB` ("As set in white balance module").
    *   **Behavior:** Instead of reading EXIF, it takes the current multipliers ($WB_{Applied}$), converts them to $(x,y)$ chromaticity using the camera characterization matrix, and uses *that* as the source illuminant for CAT16/Bradford adaptation.

3.  **UI Updates (`iop/temperature.c`)**
    *   Added a "Late Correction" checkbox to the `temperature` module.
    *   It allows users to explicitly toggle this behavior for "User/Spot" modes.
    *   Auto-enables when switching from "Camera Reference" to a manual mode if Color Calibration is active.

### B. Mathematical Robustness
*   **Highlight Recovery (`iop/highlights.c`, `opposed.c`):** Updated to scale clipping thresholds using $WB_{Applied}$. If `temperature` applies a huge gain to the Blue channel, the highlight reconstruction must know this to identify clipped pixels correctly relative to the sensor's saturation point.
*   **Bad Matrix Workaround:** The solution preserves the "Caveat Workaround" (where users manually fix a broken D65 matrix). By un-checking "Late Correction", users can force the `colorin` module to process their custom coefficients as if they were the native D65 response, cancelling out matrix errors.

### C. Refactoring
*   Refactored `illuminants.h` to allow `find_chromaticity_from_coeffs` to accept arbitrary coefficients, removing the need for complex ratio calculations when determining CCT from user sliders.

---

## 5. Summary of Modified Files

*   **`src/iop/temperature.c`**: Added `late_correction` param and checkbox.
*   **`src/iop/channelmixerrgb.c`**: Added `DT_ILLUMINANT_WB` mode; renamed ratio helpers for clarity; fixed GUI infinite loop bug.
*   **`src/iop/colorin.c`**: Updated to use `wb_coeffs` for normalization.
*   **`src/iop/highlights.c`**: Updated clipping logic to use `wb_coeffs`.
*   **`src/iop/hlreconstruct/opposed.c` & `segbased.c`**: Updated coefficient ratios.
*   **`src/common/illuminants.h`**: Added enum; refactored CCT lookup helpers.
