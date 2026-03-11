# Pixelpipe Architecture

The **pixelpipe** is the core image processing engine of darktable. It is responsible for taking an input image (RAW or raster), passing it through a series of modules (IOPs), and producing an output for display (darkroom, thumbnail) or export.

## Core Structures

### `dt_dev_pixelpipe_t`
Defined in `src/develop/pixelpipe_hb.h`.
This structure represents a single instance of a processing pipeline. A `dt_develop_t` (the main development state) holds several pipes:
- `dev->full.pipe`: Main darkroom center view (via `dt_dev_viewport_t full`).
- `dev->preview_pipe`: Navigation/overview preview (direct member of `dt_develop_t`).
- `dev->preview2.pipe`: Second darkroom window (via `dt_dev_viewport_t preview2`).
- Export pipes are created on the fly.

Key members include:
- `nodes`: A `GList` of `dt_dev_pixelpipe_iop_t` representing the processing chain.
- `image`: The `dt_image_t` being processed.
- `input`: The source image data (float buffer).
- `cache`: The hash-based pixel cache (`dt_dev_pixelpipe_cache_t`).
- `input_timestamp`: Timestamp of the input data, used for invalidation.
- `bypass_blendif`: (boolean) If true, blending is bypassed (e.g., for mask display).
- `mask_display`: Controls how/if masks are displayed in the UI.

### `dt_dev_pixelpipe_iop_t`
Defined in `src/develop/pixelpipe_hb.h`.
Represents a specific instance of a module *within a pipe*. While `dt_iop_module_t` represents the module's global state and settings, `dt_dev_pixelpipe_iop_t` ("piece") holds the state specific to one execution context.

Key members:
- `module`: Pointer to the logic definition (`dt_iop_module_t`).
- `data`: Pointer to the module's parameters (the C struct).
- `enabled`: Whether this node is active in this run.
- `roi_in`, `roi_out`: Regions of Interest (see below).
- `blendop_data`: Pointer to the blending parameters for this instance.
- `histogram`: Histogram data for this module.
- `process_cl_ready`: (boolean) Flag indicating if OpenCL processing is ready/possible.
- `process_tiling_ready`: (boolean) Flag indicating if tiled processing is ready/possible.

## Processing Flow

1.  **Change Detection**: `dt_dev_pixelpipe_change()` checks flags to see what changed (history, params, zoom).
2.  **Synchronization**:
    -   `dt_dev_pixelpipe_synch_all()`: Iterates over the history stack.
    -   Calls `commit_params()` on modules to copy parameters from the global module state to the pipe-specific `piece->data`.
    -   This ensures thread safety: the pipe runs on a copy of parameters.
3.  **Processing**: `dt_dev_pixelpipe_process()` is the main driver.
    -   It determines the "Region of Interest" (ROI) starting from the requested output (screen area or export size).
    -   **Back-propagation**: It iterates *backwards* from the last module to the first. It calls `modify_roi_in()` on each module to ask: "If I need this output area, what input area do you need?".
        -   Calculates distorts, lens corrections, crops, etc.
    -   **Forward-processing**: It iterates *forwards* from the first module.
        -   Checks the cache (`pixelpipe_cache`). If a hash matches, it reuses the buffer.
        -   If not cached, it calls `process()` (or `process_cl` for OpenCL).
        -   Stores the result in the cache if appropriate.

## Pipeline Caching

Darktable employs a sophisticated caching mechanism to avoid redundant processing. This is implemented in `src/develop/pixelpipe_cache.c`.

### Hash-based Caching
Each module instance in the pipeline (`dt_dev_pixelpipe_iop_t`) maintains a `hash` that represents its state.
The hash is computed cumulatively. For a given module at position $N$, the hash depends on:
1.  The image ID.
2.  The pipe type (preview, full, export, etc.).
3.  The detail mask state.
4.  The colour profiles: input, working, and output ICC profile info (`pipe->input_profile_info`, `pipe->work_profile_info`, `pipe->output_profile_info`). Because colour profile changes are committed globally rather than per-module, they cannot be tracked in individual `piece->hash` values and are instead included in the base hash for every cache lookup.
5.  The hashes of all preceding enabled/non-skipped modules (0 to $N-1$).
6.  The parameters of the current module (via `piece->hash`, which covers operation name, instance, params, and blending).

When `dt_iop_commit_params` is called (usually after a param change), the `piece->hash` is updated. This hash change propagates down the pipeline.

### Cache Storage (`dt_dev_pixelpipe_cache_t`)
The cache stores processed buffers keyed by these hashes. When the pipeline runs, it checks if a valid buffer exists for the current hash at each stage.
-   **Hit**: The cached buffer is reused. Processing for this module (and potentially previous ones) is skipped.
-   **Miss**: The module's `process()` method is executed.

### Invalidation
Cache invalidation is handled via `dt_dev_pixelpipe_cache_invalidate_later` and `dt_dev_pixelpipe_cache_flush`.
-   `invalidate_later(pipe, order)`: Invalidates all cache lines for modules with `iop_order` >= `order`.
-   `flush(pipe)`: Invalidates everything.

Debugging flags:
-   Running darktable with `-d pipe` shows cache hits/misses.
-   `-d memory` shows cache memory usage.

## Regions of Interest (ROI)

Darktable processes images in chunks (tiles) or just the visible area to save memory and improve performance.

-   **`roi_out`**: The region the module *must* produce.
-   **`roi_in`**: The region the module *needs* from the previous module.

For simple point operations (exposure, curves), `roi_in == roi_out`.
For geometric operations (lens correction, rotate), `roi_in` is a transformed version of `roi_out`.
For neighborhood operations (blur, sharpen), `roi_in` is slightly larger than `roi_out` (padding).

## Threading and OpenCL

The pixelpipe is designed to be threaded.
-   **CPU**: Modules use OpenMP (`DT_OMP_FOR`) within their `process()` function to parallelize loops.
-   **GPU**: If OpenCL is available and enabled, the pipe looks for `process_cl()` callbacks. The pipe handles data transfer to/from the GPU.

## Pipeline Ordering Asymmetry

Two key pipeline operations iterate modules in **different** orders:

- **`commit_params()`** runs in **forward** pipe order (e.g., temperature before channelmixerrgb). This is the normal processing direction.
- **`_dt_dev_load_pipeline_defaults()`** runs in **reverse** pipe order (e.g., channelmixerrgb before temperature). This happens during history reset and default loading.

This asymmetry matters for modules that communicate via shared state. For example, `temperature.c` writes white balance coefficients into `dev->chroma.wb_coeffs`, and `channelmixerrgb.c` reads them during `commit_params()`. During forward processing, temperature commits first and the data is available. But during reverse-order default loading, channelmixerrgb runs first — before temperature has written its values. This caused a bug where stale values from a previous image or history state influenced the defaults.

**Consequence:** Shared state (like `dev->chroma`) must be properly reset before reverse-order iteration to prevent this class of ordering-dependent bug.

## Introspection Connection

The parameters in `dt_dev_pixelpipe_iop_t->data` are binary blobs defined by the module's `params_t` struct. The **[introspection](introspection.md)** system allows the core to copy, hash, and store these blobs without knowing their internal structure.
