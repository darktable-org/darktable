# Pixelpipe Architecture

The **pixelpipe** is the core image processing engine of darktable. It is responsible for taking an input image (RAW or raster), passing it through a series of modules (IOPs), and producing an output for display (darkroom, thumbnail) or export.

## Core Structures

### `dt_dev_pixelpipe_t`
Defined in `src/develop/pixelpipe_hb.h`. This structure represents a single instance of a processing pipeline. A `dt_develop_t` (the main development state) holds several pipes:
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
Defined in `src/develop/pixelpipe_hb.h`. Represents a specific instance of a module *within a pipe*. While `dt_iop_module_t` represents the module's global state and settings, `dt_dev_pixelpipe_iop_t` ("piece") holds the state specific to one execution context.

Key members:
- `module`: Pointer to the logic definition (`dt_iop_module_t`).
- `data`: Pointer to this piece's processing data — either a plain copy of the module's `params_t`, or a module-defined `data_t` produced by `commit_params()` (see [IOP_Module_API.md](IOP_Module_API.md#params_t-vs-data_t--the-two-parameter-structs)).
- `enabled`: Whether this node is active in this run.
- `roi_in`, `roi_out`: Regions of Interest (see below).
- `blendop_data`: Pointer to the blending parameters for this instance.
- `histogram`: Histogram data for this module.
- `process_cl_ready`: (boolean) Flag indicating if OpenCL processing is ready/possible.
- `process_tiling_ready`: (boolean) Flag indicating if tiled processing is ready/possible.

## Processing Flow

1.  **Change Detection**: `dt_dev_pixelpipe_change()` checks flags to see what changed (history, params, zoom).
2.  **Node construction** — only when the topology changed (`DT_DEV_PIPE_REMOVE`), not on every run: `dt_dev_pixelpipe_cleanup_nodes()` tears the node list down and `dt_dev_pixelpipe_create_nodes()` rebuilds it.
    -   Creation walks *every* module in `dev->iop`, enabled or not, and gives each one a `dt_dev_pixelpipe_iop_t`. Enablement is recorded in `piece->enabled`, not by leaving the node out.
    -   Each node's `piece->data` is allocated here, by `dt_iop_init_pipe()` calling the module's `init_pipe()` (or the default, which `calloc()`s `params_size` bytes). Teardown calls `piece->module->cleanup_pipe()` directly.
    -   This is the `piece->data` *lifetime*: it spans however many processing runs happen before the next topology change, and `commit_params()` overwrites its contents in place.
3.  **Synchronization**:
    -   `dt_dev_pixelpipe_synch_all()`: first commits every module's `default_params` over all nodes, then replays the history stack on top.
    -   Calls `commit_params()` on modules to transform or copy parameters from the global module state into the pipe-specific `piece->data`.
    -   This ensures thread safety: the pipe runs on a copy of parameters.
4.  **Processing**: `dt_dev_pixelpipe_process()` is the main driver.
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
Each module instance in the pipeline (`dt_dev_pixelpipe_iop_t`) maintains a `hash` that represents its state. The hash is computed cumulatively. For a given module at position $N$, the key of a cache lookup is built from two groups of inputs.

**Always:**
1.  The image ID.
2.  The color profiles: input, working, output and export ICC profile info (`pipe->input_profile_info` and friends), by pointer identity — see [What the Cache Key Does Not Cover](#what-the-cache-key-does-not-cover).
3.  The hashes of all preceding enabled/non-skipped modules (0 to $N-1$).
4.  The parameters of the current module (via `piece->hash`, which covers operation name, instance, params, and blending).

**Only when the lookup supplies a ROI** — which is the normal processing case:

5.  The pipe type (preview, full, export, etc.).
6.  The detail mask state.
7.  The ROI itself — including its `scale` (`dt_iop_roi_t`, `src/develop/pixelpipe.h`) — plus `pipe->scharr.hash` and the color picker's sample when an included module is picking.

Together these are the key as `dt_dev_pixelpipe_cache_hash()` builds it, not a hash of the whole pipe. The split matters because the reduced form is useful on its own: without a ROI, what remains — image, profiles, preceding pieces, this piece — is a hash of the parameter state of the pipe up to that position, independent of any particular region. That reduced form is what `dt_dev_pixelpipe_piece_hash()` produces when it is passed a NULL ROI, as `hlreconstruct`'s `opposed` code does for its own cache; it is not a cache-line lookup.

Because a normal lookup already carries the pipe type, the scale and the color profiles, those do not need to be in a module's `params` — deliberately so. See [IOP_Module_API.md](IOP_Module_API.md#commit_params---transform-parameters-into-processing-data) for the rule this puts on `commit_params()`.

When `dt_iop_commit_params` is called (usually after a param change), the `piece->hash` is updated. This hash change propagates down the pipeline.

### What the Cache Key Does Not Cover

The key does not hash the pipe or the image record wholesale. It hashes selected fields, so "I can reach it through `piece->pipe` or `self->dev`" is not a reason to assume an input is covered. Three cases worth knowing:

- **Image metadata other than the id.** The id identifies the image; it does not version it. Most of `dt_image_t` is capture data fixed at import, and the darkroom works from a copy of the record taken when the image was loaded (`dev->image_storage`, `src/develop/develop.c`), so that part cannot move under a running pipe. The database-backed part can, and none of it is read from that copy: on every call, `dt_variables_expand()` takes the rating from the live image-cache record and the color labels, tags and user metadata from their own tables (`src/common/variables.c`). Where each can be edited differs — the rating and the color labels from within the darkroom, the tagging panel there too when `plugins/darkroom/tagging/visible` is set, the user-metadata editor in lighttable and tethering only (`src/libs/metadata.c`).
- **Profile contents.** The four profiles are in the key by identity — the pipe's profile-info *pointers* are hashed, not what they point at. They are in the base hash of every lookup, rather than in a `piece->hash`, because profile changes are committed globally rather than per-module and so cannot be tracked per piece.
- **Preferences and other process-global state.** `colorout` reads `plugins/lighttable/export/force_lcms2` from the config (`src/iop/colorout.c`).

A module cannot add to the key: `dt_iop_commit_params()` builds `piece->hash` *after* the module's `commit_params()` has returned and assigns it unconditionally (`src/develop/imageop.c`), and `src/iop/iop_api.h` declares nothing else for the purpose. Three routes remain:

- **Put the value in `module->params`.** That is the buffer the wrapper hashes — not the `params` argument the callback was handed, which on the defaults sync is a different object. Getting it there means the normal UI-and-history path, so the value persists to history and XMP as well: this suits small, stable, serializable values only. `overlay` keeps the overlay image's path in its params for exactly this reason, and zero-fills the buffer before writing so that trailing bytes of a previous path cannot bleed into the hash (`src/iop/overlay.c`).
- **Use a pipe field the framework already hashes.** `colorout` publishes its export profile with `dt_ioppr_set_pipe_export_profile_info()` precisely so that part *is* hashed. This is not a general escape hatch — the four profile-info pointers, the pipe type and the detail-mask flag are fixed slots, not somewhere to hang arbitrary state.
- **Invalidate explicitly.** For anything that cannot reach the key at all — an external file's contents, the image's tags — drop the stale cache lines yourself. `dt_iop_refresh_center()` and its siblings (see [IOP_Module_API.md — Pipeline Refresh Functions](IOP_Module_API.md#pipeline-refresh-functions)) invalidate every cache line from a given `iop_order` onwards immediately; `dt_dev_reprocess_center()` records the same boundary and the next pipe run applies it. The call is the easy half — the hard half is knowing when to make it, and neither example in the tree has a precise trigger. `rasterfile` hashes its own params and image id to decide when to re-read its file (`src/iop/rasterfile.c`), which catches a change of *which* file but not an edit to the file at that path, so it also reprocesses unconditionally from `gui_focus()`. Database metadata is the same shape: `DT_SIGNAL_METADATA_CHANGED` and `DT_SIGNAL_TAG_CHANGED` announce that something changed, but neither carries an image id, and `DT_SIGNAL_TAG_CHANGED` also fires for tag-dictionary edits that touch no image at all (`src/common/tags.c`). Either invalidate on every event, or re-read the value you depend on and compare it with the one you last used.

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

How the two relate depends on what the module does:

-   **Point operations** (exposure, curves): `roi_in == roi_out`.
-   **Geometric operations** (lens correction, rotate): `roi_in` is a transformed version of `roi_out`.
-   **Neighborhood operations** (blur, sharpen): `roi_in` is slightly larger than `roi_out` (padding).

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

The parameters a module serializes — its `params_t` struct, which is also what `dt_dev_pixelpipe_iop_t->data` holds when the module defines no separate `data_t` — are binary blobs. The **[introspection](introspection.md)** system allows the core to copy, hash, and store these blobs without knowing their internal structure.
