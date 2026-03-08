# DONE
---
* Idea: Replace `sqrtf` and explicit division with `dt_fast_inv_sqrtf` / native reciprocals in `heat_PDE_diffusion` for gradient mapping.
* Outcome: **FAILED**. Disrupted compiler auto-vectorization across the anisotropic SIMD blocks. Measurement increased to ~51.0s vs 49.7s baseline. Reverted.
---
* Idea: Implement conditional Dead Code Elimination (DCE) to skip isotropic laplacian calculation bypass evaluation in `diffuse.c` geometry loop.
* Outcome: **FAILED**. Dynamic branching completely broke OpenMP loop vectorization. Runtime degraded heavily. Reverted.
---
* Idea: Implement OpenCL float4 `native_rsqrt` replacement for `dtcl_sqrt` magnitude derivations. 
* Outcome: **FAILED**. Actual execution time of `diffuse.cl` geometry shaders regressed as native math extensions tripped up divergent parameter caching inside the GPU warp. Reverted.
---
* Idea: Rewrite the 9-element `kernel` matrix instantiation with inline algebraically reduced scalar permutations (removing `compute_kernel` and `build_matrix` overheads).
* Outcome: **SUCCESS**. Reduced baseline from 49.775s to 45.631s. (Committed)

---
* Idea: Strip internal array allocations for `find_gradients` normalizations, inlining arithmetic directly into native scalars inside `heat_PDE_diffusion`.
* Outcome: **SUCCESS**. Reduced baseline from 45.631s to 40.958s (another ~10.2% drop). (Committed)

---
* Idea: Merge the 4 `for_each_channel()` parallel loops in `heat_PDE_diffusion` into a single loop, replacing aligned arrays for intermediate values with pure scalar unrolling.
* Outcome: **FAILED**. Broke GCC loop auto-vectorization across the channel dimension, causing 25% execution regression (Failed fast at 50.8s vs 40.9s baseline). Reverted.

---
* Idea: Replace nested coordinate permutation loops initializing `neighbour_pixel` matrix and calculating scalar variance with explicitly flattened sequential memory gathers.
* Outcome: **SUCCESS**. Reduced baseline from 40.958s to 40.024s (~2.2% drop). Removed compiler bounds checks and inner iteration bloat. (Committed)

---
* Idea: Eliminate `neighbour_pixel_HF[9]` and `neighbour_pixel_LF[9]` intermediate stack arrays entirely, computing gradients, sums, and variance directly from HF/LF source pointers using pre-computed n0-n8 offsets. Avoids 72 float writes + reads from stack, leveraging L1 cache spatial locality.
* Outcome: **SUCCESS**. Reduced baseline from 40.024s to 39.215s (~2.0% drop). (Committed)

---
* Idea: Merge post-convolution loops (variance regularization + derivatives accumulation + output) into a single `for_each_channel`, eliminating the `acc` intermediate array.
* Outcome: **FAILED**. Measured 39.577s vs 39.215s baseline — ~0.9% regression. Compiler optimizes separate loops better (likely vectorizing accumulation separately). Reverted.

---
* Idea: Merge gradient computation and sums/variance loops into a single `for_each_channel` loop, loading all 18 pixel values once and computing everything in one pass to eliminate 8 redundant reads of shared pixel positions.
* Outcome: **FAILED**. Measured 40.958s vs 39.215s baseline — ~4.4% regression. GCC `split-loops` optimization in extra_optimizations.h generates better code with separate loops. Large loop body prevents effective vectorization. Reverted.

---
* Idea: Compute cos²θ, sin²θ, and cosθ·sinθ directly from squared gradient magnitude (gx²/m², gy²/m², gx·gy/m²) instead of normalizing the gradient first then squaring. Reuses gx² and gy² already computed for magnitude, eliminating redundant squarings and one division per gradient direction per channel.
* Outcome: **SUCCESS**. Reduced baseline from 39.215s to 38.847s (~0.94% drop). (Committed)

---
* Idea: Precompute reciprocal of regularized variance (1/var) in a separate vectorizable loop, then use multiplication (acc * inv_var) instead of division (acc / var) in the output computation.
* Outcome: **FAILED**. Measured 39.211s vs 38.847s baseline — ~0.9% regression. GCC `finite-math-only` already optimizes the division; extra loop overhead outweighed any benefit. Reverted.

---
* Idea: Pair the 4 `compute_convolution` calls into 2 `for_each_channel` loops: derivatives 0+2 (sharing gradient angle data, applied to LF and HF sums) and derivatives 1+3 (sharing laplacian angle data, applied to LF and HF sums). Each loop computes 2 derivatives in one pass, halving loop overhead and improving register reuse for shared angle values.
* Outcome: **SUCCESS**. Reduced baseline from 38.847s to 36.877s (~5.1% drop). Initial version had bug using isotropy_type[0] for both derivatives 0 and 2 (and [1] for both 1 and 3). Fixed to use correct per-derivative isotropy_type, with slight performance cost from additional branches. (Committed)

---
* Idea: Merge derivative accumulation (`acc += derivatives[k] * ABCD[k]`) directly into the paired convolution loops, eliminating the `derivatives[4]` intermediate array and the separate accumulation loop. Each derivative scalar is multiplied by its ABCD weight immediately after computation.
* Outcome: **FAILED**. Measured 37.966s vs 36.877s baseline — ~3.0% regression. Larger loop body with the accumulation hurt GCC auto-vectorization. Reverted.

---
* Idea: Conditionally skip gradient/laplacian angle computation and dt_vector_exp calls when both derivatives in a pair use DT_ISOTROPY_ISOTROPE (since the isotropic path never reads c2 or angle data).
* Outcome: **ABANDONED**. Approach was motivated by inspecting benchmark test data parameters, violating the rule against tailoring optimisations to specific inputs. Reverted before benchmarking.

---
* Idea: Unroll the derivative accumulation loop (`for k=0..3 { acc += derivatives[k]*ABCD[k] }`) into a single `for_each_channel` expression computing all 4 products in one pass. Eliminates outer loop overhead, removes zero-initialization, and lets GCC emit a single vectorized multiply-add chain.
* Outcome: **SUCCESS**. Reduced baseline from 36.877s to 36.231s (~1.75% drop). (Committed)

---
* Idea: Merge variance regularization (`variance[c] = threshold + variance[c] * factor`) into the output loop, computing `var` inline and eliminating a separate `for_each_channel` pass. The output loop body stays small (1 FMA + division + add + fmax).
* Outcome: **SUCCESS**. Reduced baseline from 36.231s to 35.972s (~0.71% drop). (Committed)

---
* Idea: Merge accumulation loop into output loop (eliminating `acc` intermediate array), computing all derivatives*ABCD products, variance, and output in a single `for_each_channel`.
* Outcome: **FAILED**. Measured 36.237s vs 35.972s baseline — ~0.74% regression. Same pattern as before: GCC generates better code with separate loops even though the body is small. Reverted.

---
* Idea: Remove 0.5f scaling from gradient/laplacian computation. The factor cancels in angle ratios (cos²θ, sin²θ, cosθsinθ) and is absorbed into `half_anisotropy` (0.5 * anisotropy) for magnitude. Eliminates 4 float multiplies per pixel per channel.
* Outcome: **SUCCESS**. Reduced baseline from 35.972s to 35.557s (~1.15% drop). (Committed)

---
* Idea: Establish "Current Best" baseline for general-case and preset benchmarks using module iteration count = 20. Code includes verified 0.5f scaling and `c2_is_identity` (skipping math for zero anisotropy).
* General Case Baseline: **110.692s** (G1: 38.297, G2: 39.325, G3: 33.070)
* Preset Baseline: **185.659s** (deblur: 30.766, denoise: 25.711, contrast: 59.175, sharpness: 43.746, bloom: 26.261)
* Outcome: **ESTABLISHED**. These are the targets to improve.

---
* Idea: Add `c2_is_identity` and `c2_reuse` arrays with conditional branching inside the `for_each_channel` gradient/laplacian loop to skip angle computation and dt_vector_exp calls when anisotropy is zero, plus reuse c2 values when anisotropy pairs match. Also added identity shortcut in derivative computation.
* Outcome: **FAILED**. Massive regression — 56.7s vs 35.972s baseline (~58% worse). The if/else branching inside the `for_each_channel` loop completely destroyed GCC auto-vectorization. Confirmed once again that branching inside channel loops is catastrophic. Reverted.

---
* Idea: Remove 0.5f central-difference scaling from gradient/laplacian computation. The factor cancels in angle ratios (cos²θ = gx²/m²) and is absorbed into `half_anisotropy[k] = anisotropy[k] * 0.5f` precomputed outside the pixel loop. Saves 4 float multiplies per pixel per channel.
* Outcome: **SUCCESS**. Reduced baseline from 35.972s to 35.599s (~1.04% drop). (Committed)

---
* Idea: Skip gradient/laplacian angle computation and exp() when all derivatives using those angles are isotropic. Split combined gradient+laplacian `for_each_channel` into two separate loops, each wrapped with a loop-invariant `if(need_*_angles)` check. Conditionally skip `dt_vector_exp` calls for isotropic derivatives.
* Outcome: **FAILED**. Standard benchmark improved 12.3% (31.210s vs 35.599s) but general case regressed 2.4% (50.221s vs 49.042s), exceeding the 0.5% threshold. Loop splitting hurt GCC auto-vectorization for all-anisotropic case. Reverted.

---
* Idea: Same angle-skipping optimization but keeping the combined gradient+laplacian loop (no splitting). Wrapped entire loop with `if(need_grad_angles || need_lapl_angles)`, only split the exp() calls.
* Outcome: **FAILED**. Standard benchmark improved 6.9% (33.143s) but general case still regressed 2.0% (50.022s vs 49.042s). Even wrapping the combined loop with an `if` affected GCC code generation for the taken branch. Reverted.

---
* Idea: Eliminate `sin_theta_grad_sq` and `sin_theta_lapl_sq` arrays entirely using identity `sin²θ = 1 - cos²θ`. Reformulate a11/a22 via `p = cos²θ*(1-c2)` with `a11 = c2+p, a22 = 1-p` (ISOPHOTE) or swapped (GRADIENT). Also use `a11+a22 = 1+c2` for center weight. Saves 2 array declarations, 8 stores per pixel, 2 multiplies per pixel.
* Outcome: **FAILED**. Measured 36.230s vs 35.599s baseline — 1.77% regression. The `p = cos²θ*(1-c2)` formulation creates serial dependencies, whereas the original `cos²θ + c2*sin²θ` allows two independent MAD operations in parallel. Reverted.

---
* Idea: Shared intermediates for diag_sum/diag_diff — compute `a=n0+n8, b=n2+n6`, then `diag_sum=a+b, diag_diff=a-b` instead of independent computation.
* Outcome: **FAILED** (benchmark). Measured 36.201s vs 35.599s baseline — 1.69% regression. GCC was already performing CSE; explicit intermediates hurt register allocation. Reverted.

---
* Idea: Pre-halve `cos_theta_sin_theta` (multiply by 0.5f) in the gradient/laplacian angle loop so the derivative computation can use it directly as `b11` without the `* 0.5f`. Saves one multiply per non-isotropic derivative per channel.
* Outcome: **FAILED** (benchmark). Measured 36.323s vs 35.599s baseline — 2.03% regression. The extra `*0.5f` in the gradient loop (runs for ALL pixels) costs more than the savings in derivative b11 (only non-isotropic paths). Reverted.

---

* Idea: Remove dead zero-initialization of `derivatives[4]` and intermediate sum arrays (`diag_sum_LF`, etc.). These 64-byte structures are unconditionally written in `for_each_channel` loops before being read. Removing the `{ { 0.f } }` initializers saves redundant memsets in the hot pixel loop.
* Outcome: **FAILED** (benchmark). Measured 36.123s vs 36.053s baseline. No significant change (likely noise or compiler already optimizing). Reverted.

---

* Idea: Use `a11+a22 = 1+c2` identity for center weight. Currently `(-2.0f * (a11 + a22))` is computed for each derivative. Replacing this with `(-2.0f * (1.0f + c2[k][c]))` should save one addition and one multiplication (negation) inside the derivative loops, as `c2` is already available.
* Outcome: **FAILED** (benchmark). Measured 36.473s vs 36.053s baseline. Regression of ~1.1%. Reverted.

---

* Idea: Precompute `neg_magnitude` once in gradient loop. Compute `neg_mag_grad = -sqrtf(m2_grad)` once, then `c2[0][c] = neg_mag_grad * half_anisotropy[0]; c2[2][c] = neg_mag_grad * half_anisotropy[2]`. Currently the compiler must negate the magnitude implicitly for each c2 assignment. Explicit precomputation ensures one negation instead of two per angle set.
* Outcome: **FAILED** (benchmark). Measured 36.501s vs 36.053s baseline. Regression of ~1.2%. Reverted.

---

* Idea: Use `center_HF[c]` and `center_LF[c]` in output loop. Replace `HF[index + c]` with `center_HF[c]` and `LF[index + c]` with `center_LF[c]` in the final output computation. These values are already loaded into `dt_aligned_pixel_t` arrays during the sums loop. Reusing them avoids redundant memory loads from the source `LF` and `HF` pointers.
* Outcome: **FAILED** (benchmark). Measured 36.397s vs 36.053s baseline. Regression of ~0.9%. Reverted.

---

* Idea: Precompute isotropic convolution results. Compute `iso_LF[c]` and `iso_HF[c]` in a separate `for_each_channel` loop after sums, then use directly in derivative loops for isotropic branches (`derivatives[k][c] = iso_LF[c]`). Reduces derivative loop body complexity and avoids redundant isotropic formula computation when multiple derivatives sharing the same source are isotropic.
* Outcome: **FAILED** (benchmark). Measured 36.424s vs 36.053s baseline. Regression of ~1.0%. Reverted.

---

* Idea: Add `#pragma GCC optimize("fast-math")` scoped to `heat_PDE_diffusion` to skip Newton-Raphson refinement after `vrsqrtps`/`vrcpps`, saving ~20 instructions (~3.5% of loop body).
* Outcome: **FAILED** (no effect). Assembly analysis showed 578 vs 579 instructions — essentially identical. `-ffast-math` does NOT control NR refinement; that's governed by `-mrecip` (a machine flag, not an optimization flag). Benchmark: 35.704s vs 35.599s baseline (~0.3%, noise). Reverted.

---

* Idea: Swap sums and angle loop order. Move the sums+variance `for_each_channel` loop (which loads all 18 LF+HF neighbor pixels) before the gradient/laplacian angle loop. After sums runs, all pixel values are in L1 cache, so the angle loop's re-loads of n1,n3,n5,n7 are guaranteed cache hits.
* Outcome: **SUCCESS**. Reduced baseline from 35.599s to 35.224s (~1.05% drop). Zero code logic change — purely a loop reorder that improves cache locality. (Committed)

---

* Idea: Fuse `dt_vector_exp` into gradient/laplacian angle loop. Inline the integer bit-trick directly in the `for_each_channel` where c2 values are computed, eliminating the separate `for(k=0..3) dt_vector_exp()` loop.
* Outcome: **FAILED** (benchmark). Measured 35.655s vs 35.224s baseline — 1.22% regression. Mixing integer bit-trick operations (float-to-int conversion) with the float-heavy angle computation disrupted GCC auto-vectorization scheduling. Reverted.

---

# IN PROGRESS


# UPCOMING

1. ~~**Fuse `dt_vector_exp` into gradient/laplacian angle loop** [MOVED TO IN PROGRESS]~~: Inline the integer bit-trick (`0x3f800000 + (int)(x * 0x00B2F854)`) directly in the gradient/laplacian `for_each_channel` loop where c2 values are first computed, rather than calling `dt_vector_exp` in a separate `for(k=0..3)` loop. Eliminates separate exp loop overhead and reduces c2's live range on stack.

2. **Fuse angle+exp+derivatives (eliminate angle storage arrays)**: Fuse gradient angle computation + inline exp + derivative 0+2 computation into a single `for_each_channel` loop. Do the same for laplacian angles + derivatives 1+3 in a second loop. Eliminates 6 angle storage arrays (96 bytes stack) by consuming angles immediately. Risk: larger loop body may hurt auto-vectorization.

3. **Specialize `heat_PDE_diffusion` for `has_mask=false`**: Split inner pixel loop into two code paths: one without mask handling (eliminates mask byte load + `if(opacity)` branch per pixel) and one with. Select via loop-invariant `if(has_mask)` at function level. Common no-mask path gets tighter code without dead else-block.

4. **Use `__builtin_expect` on the `if(opacity)` branch**: Mark the opacity check with `__builtin_expect(opacity, 1)` to hint the compiler/CPU that this branch is almost always taken. May improve branch prediction and code layout, putting the hot path in the fall-through position.

5. **Eliminate `diag_diff` arrays by recomputing inline in derivatives**: Currently `diag_diff_LF` and `diag_diff_HF` are stored in the sums loop and read once each in derivative computation (multiplied by `b11`). Since `diag_diff = n0 - n2 - n6 + n8` involves values already loaded in the sums loop, and it's only used once with a scalar multiply, we could pass the 4 raw corner values through instead, computing `b11 * (n0 - n2 - n6 + n8)` inline in the derivative loop. Saves 2 array writes (8 floats) at the cost of 4 extra loads (likely L1 hits from the sums loop). Reduces register pressure from fewer live arrays.
