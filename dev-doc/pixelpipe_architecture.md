# Pixelpipe Architecture

The **pixelpipe** is the core image processing engine of darktable. It is responsible for taking an input image (RAW or raster), passing it through a series of modules (IOPs), and producing an output for display (darkroom, thumbnail) or export.

## Core Structures

### `dt_dev_pixelpipe_t`
Defined in `src/develop/pixelpipe_hb.h`.
This structure represents a single instance of a processing pipeline. A `dt_develop_t` (the main development state) holds several pipes:
- `preview_pipe`: For the center view in darkroom.
- `preview2_pipe`: For the secondary preview (e.g., navigation window).
- `full`: For high-quality export or 1:1 view.
- Export pipes are created on the fly.

Key members:
- `nodes`: A `GList` of `dt_dev_pixelpipe_iop_t`. The processing chain.
- `image`: Copy of the `dt_image_t` being processed.
- `input`: The source image data (float buffer).
- `cache`: The `dt_dev_pixelpipe_cache_t` storing intermediate results.

### `dt_dev_pixelpipe_iop_t`
Defined in `src/develop/pixelpipe_hb.h`.
Represents a specific instance of a module *within a pipe*. While `dt_iop_module_t` represents the module's global state and settings, `dt_dev_pixelpipe_iop_t` ("piece") holds the state specific to one execution context.

Key members:
- `module`: Pointer to the logic definition (`dt_iop_module_t`).
- `data`: Module-specific processing data (committed parameters).
- `enabled`: Whether this node is active in this run.
- `roi_in`, `roi_out`: Regions of Interest (see below).
- `histogram`: Buffer for histogram data if requested.

## Processing Flow

The lifecycle of a pipe run typically follows this sequence:

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

## Introspection Connection

The parameters in `dt_dev_pixelpipe_iop_t->data` are binary blobs defined by the module's `params_t` struct. The **introspection** system allows the core to copy, hash, and store these blobs without knowing their internal structure.
