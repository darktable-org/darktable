# Math, Matrices, and Color Science

Darktable provides an extensive library of fast, SIMD-optimized math and color functions. Do not write your own matrix multiplications or color space conversions.

## Core Math Helpers

Include `common/math.h` and `common/darktable.h` for general math limits, power functions, and clamping:
- `CLAMP(val, min, max)` - Fast bounding macro.
- `dt_fast_exp2f(x)`, `dt_fast_log2f(x)` - Accelerated power/log functions for tight loops.
- `fmaxf()`, `fminf()`, `copysignf()` - Standard C99 math is preferred over custom macros where SIMD compiler auto-vectorization is needed.

## Color Matrices and Transposition

Darktable's image processing loops are heavily optimized for SIMD (Single Instruction, Multiple Data). To facilitate vectorization across color channels, **3x3 color matrices in darktable are often transposed**.

If you define a standard matrix where rows correspond to output channels (e.g. Row 0 = R, Row 1 = G, Row 2 = B), applying it standardly requires dot products across the input vector `[R, G, B]`. By transposing the matrix, the compiler can use broadcast multipliers efficiently.

**Standard Application (Slow):**
```c
// Requires dot products
out[0] = M[0][0]*in[0] + M[0][1]*in[1] + M[0][2]*in[2];
```

**Transposed Application (Fast, used throughout DT):**
```c
#include "common/colorspaces.h"
// M_transposed is mathematically M^T

// Applies M_transposed to `in` and stores to `out`
dt_apply_transposed_color_matrix(in, M_transposed, out);
```
Internally, `dt_apply_transposed_color_matrix` computes:
`out[c] = M[0][c]*in[0] + M[1][c]*in[1] + M[2][c]*in[2]` (which evaluates to $M \cdot in$ if M is stored column-major / transposed).

When constructing combined matrices (e.g., $A \cdot B$), remember that the transpose of a product reverses the order: $(A \cdot B)^T = B^T \cdot A^T$.
Use `dt_colormatrix_mul(dst, M1, M2)` to multiply two formally 3x3 matrices. Keep track of the transposition state to avoid unexpected color swapping.

## Color Space Conversions

Include `common/colorspaces_inline_conversions.h` for highly optimized, per-pixel transformation functions.
- `dt_XYZ_to_RGB_clipped()`, `dt_RGB_to_XYZ()` - Pipe working space (usually linear Rec2020) to XYZ and back.
- `dt_Lab_to_XYZ()`, `dt_XYZ_to_Lab()` - Lab conversions.
- `dt_xyY_to_XYZ()`, `dt_XYZ_to_xyY()` - Chromaticity.

**Important Note on XYZ:** Darktable's internal profile connection space (PCS) is **XYZ D50**. If you are reading external research papers or matching formulas from other software, their XYZ might be **D65**. Use `dt_XYZ_D50_2_XYZ_D65(in, out)` and `dt_XYZ_D65_2_XYZ_D50(in, out)` to bridge them.

## Chromatic Adaptation (CAT)

Include `common/chromatic_adaptation.h` when you need to adapt colors between different white points (e.g., changing the illuminant tempertaure creatively, or mapping to a specific display white).

Darktable provides pre-calculated, transposed matrices for popular Chromatic Adaptation Transforms, such as **CAT16** (recommended) and Bradford.

```c
#include "common/chromatic_adaptation.h"

// Example: Adapting XYZ D50 to a target LMS color space using CAT16
dt_apply_transposed_color_matrix(xyz_pixel, XYZ_to_CAT16_LMS_trans, lms_pixel);

// Note that adaptation happens in LMS space (von Kries model). 
// You scale the LMS components based on the source/destination white points, 
// then convert back to XYZ:
dt_apply_transposed_color_matrix(lms_pixel, CAT16_LMS_to_XYZ_trans, xyz_pixel);
```
Do not hardcode your own CAT matrices in your module if standard ones exist in the headers, to reduce code duplication and maintain consistency across the pipeline.
