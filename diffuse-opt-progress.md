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
