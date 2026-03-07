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

# IN PROGRESS




    
# UPCOMING

1. **Pre-halve cos_theta_sin_theta at storage time**: Store `cos_theta_sin_theta_grad[c] = diff_gx * diff_gy * inv_m2 * 0.5f` in the gradient loop. Then b11 in derivative computation becomes `(c2-1) * half_cs` instead of `(c2-1) * cs * 0.5f`, saving 1 multiply per non-isotropic derivative × 4 derivatives × 4 channels = 16 ops, at cost of 8 extra ops in gradient loop. Net ~8 ops saved for all-anisotropic case.

3. **Remove dead zero-initialization of `derivatives[4]`**: Currently `dt_aligned_pixel_t derivatives[4] = { { 0.f } }` zero-fills 64 bytes. All 4 derivatives are unconditionally written (either isotropic or anisotropic path) before being read. The zero-init is dead code that the compiler may not eliminate due to alignment attributes. Removing it saves a memset.

4. **Use `a11+a22 = 1+c2` identity for center weight only**: Replace `-2.0f * (a11 + a22)` with `-2.0f * (1.0f + c2[k][c])` in derivative output. Keeps a11/a22 computation unchanged (unlike the failed sin²θ attempt). Enables earlier computation of center weight since it only depends on c2 (available before a11/a22), potentially improving ILP.

5. **Precompute `neg_magnitude` once in gradient loop**: Compute `neg_mag_grad = -sqrtf(m2_grad)` once, then `c2[0][c] = neg_mag_grad * half_anisotropy[0]; c2[2][c] = neg_mag_grad * half_anisotropy[2]`. Currently the compiler must negate the magnitude implicitly for each c2 assignment. Explicit precomputation ensures one negation instead of two per angle set.
