# IOP Module API Reference

This guide documents the functions that darktable Image Operation (IOP) modules can implement. The API is defined in `src/iop/iop_api.h` and used via `src/develop/imageop.h`. See `src/iop/useless.c` for a fully documented example module.

See also:
- [Pixelpipe Architecture](pixelpipe_architecture.md) for pipeline data flow and caching.
- [Introspection System](introspection.md) for parameter management.
- [GUI Architecture](GUI.md) for GUI events, callbacks, thread safety, and widget reparenting.

---

## Module Structure Overview

Every IOP module needs:
1. **Parameter struct** (`dt_iop_modulename_params_t`) - the user-facing parameters, serialized to the database, controlled via UI widgets in `self->params`
2. **Processing data struct** (`dt_iop_modulename_data_t`) - optional but common; a processing-optimized version of the parameters, stored in `piece->data` and used by `process()`. When not provided, `piece->data` is a plain copy of `params_t` (see [params_t vs data_t](#params_t-vs-data_t--the-two-parameter-structs) below).
3. **GUI data struct** (`dt_iop_modulename_gui_data_t`) - widget references, only present in darkroom mode
4. **Required functions** - `name()`, `default_colorspace()`, `process()`
5. **Optional functions** - GUI, lifecycle, geometry, etc.

---

## Required Structures

### Parameter Struct (`params_t`)

This struct defines the user-facing parameters. It is the contract between the module and the outside world:
- **Database:** Serialized as a binary blob and stored in the history stack. This is how edits persist across sessions.
- **UI widgets:** `dt_bauhaus_*_from_params()` functions read and write fields in `self->params` via introspection.
- **Presets/styles:** Params are what gets exported and imported.

```c
// Version number - increment when struct changes
DT_MODULE_INTROSPECTION(1, dt_iop_mymodule_params_t)

typedef struct dt_iop_mymodule_params_t
{
  // Introspection tags configure widgets automatically:
  // $MIN, $MAX, $DEFAULT set slider range/default
  // $DESCRIPTION sets the widget label
  float exposure;    // $MIN: -10.0 $MAX: 10.0 $DEFAULT: 0.0 $DESCRIPTION: "exposure"
  float gamma;       // $MIN: 0.1 $MAX: 4.0 $DEFAULT: 1.0
  gboolean enabled;  // $DEFAULT: TRUE $DESCRIPTION: "enable correction"

  // Enum fields auto-populate comboboxes
  dt_mymodule_method_t method; // $DEFAULT: METHOD_AUTO
} dt_iop_mymodule_params_t;
```

**Rules:**
- Use `gboolean` not `bool` (4-byte alignment)
- Changes require version bump and `legacy_params()` migration
- Values are serialized to database - no pointers

**Introspection tags** (parsed from comments at compile time, used by `dt_bauhaus_*_from_params()`):

| Tag | Applies to | Purpose |
|-----|-----------|---------|
| `$MIN: value` | `float`, `int` | Hard minimum for the widget |
| `$MAX: value` | `float`, `int` | Hard maximum for the widget |
| `$DEFAULT: value` | all types | Default value (also used by `dt_iop_default_init()`) |
| `$DESCRIPTION: "text"` | all types | Widget label in the GUI (translatable) |

For enums, `$DESCRIPTION` on each enum member becomes the combobox entry text. For `gboolean`, `$DEFAULT` accepts `TRUE` / `FALSE`.

The `DT_MODULE_INTROSPECTION(version, struct_type)` macro at the top of the file activates this system and sets the parameter version number.

### Enum for Comboboxes

```c
typedef enum dt_mymodule_method_t
{
  METHOD_AUTO = 0,    // $DESCRIPTION: "automatic"
  METHOD_MANUAL = 1,  // $DESCRIPTION: "manual"
  METHOD_CUSTOM = 2,  // $DESCRIPTION: "custom"
} dt_mymodule_method_t;
```

### Module Structure Details (`dt_iop_module_t`)

The `dt_iop_module_t` structure (defined in `src/iop/iop_api.h`) is the main handle for your module instance. Key members:

-   `params`: Pointer to the current parameter struct.
-   `default_params`: Pointer to the default parameters.
-   `gui_data`: Pointer to your GUI data struct.
-   `dev`: Pointer to the `dt_develop_t` session.
-   `widget`: The main widget container for the module.
-   `multi_priority` (or `instance`): The integer instance number (0 for the first instance).
-   `iop_order`: The module's execution order position in the pixelpipe.
-   `enabled`: boolean flag for the module's enabled state.

### `params_t` vs `data_t` — The Two Parameter Structs

A common source of confusion is the relationship between the parameter struct (`params_t`) and the processing data struct (`data_t`). They serve different purposes:

| | `dt_iop_modulename_params_t` | `dt_iop_modulename_data_t` |
|---|---|---|
| **Where it lives** | `self->params` | `piece->data` |
| **Source** | Database / UI widgets | Built by `commit_params()` |
| **Purpose** | Record user intent in a stable, serializable format | Provide processing-ready values to `process()` |
| **May contain** | Raw user values (e.g. EV, percentages, enum choices) | Precomputed LUTs, splines, normalized/transformed values, expensive one-time calculations |
| **Constraints** | No pointers; must be serializable; changing it requires a version bump | No constraints — can contain pointers, LUTs, runtime-only data |

**When you don't need a `data_t`:** If `process()` can work directly from the raw user parameters without any transformation, you don't need a separate `data_t`. The default `init_pipe()` allocates `piece->data` as a `params_t`-sized buffer, and the default `commit_params()` does `memcpy(piece->data, params, self->params_size)`.

**When you do need a `data_t`:** Most non-trivial modules define a separate `data_t` struct because:
- Some user parameters need transformation before they are useful for processing (e.g. converting percentages to linear factors, degrees to radians, EV values to exposure multipliers).
- Expensive calculations should happen once in `commit_params()`, not per-pixel in `process()` (e.g. building interpolation splines, computing lookup tables, solving matrices).
- Additional runtime state is needed that doesn't belong in the database (e.g. pointers to the current working color profile, gamut boundary LUTs).

**Concrete examples from the codebase:**

```c
// exposure.c: data_t embeds params_t and adds precomputed fields
typedef struct dt_iop_exposure_data_t
{
  dt_iop_exposure_params_t params;  // raw user params
  int deflicker;                    // computed: is deflicker mode active?
  float black;                      // computed: adjusted black point
  float scale;                      // computed: exposure multiplier
} dt_iop_exposure_data_t;

// filmicrgb.c: data_t is entirely different from params_t
typedef struct dt_iop_filmicrgb_data_t
{
  float dynamic_range;        // derived from white_point - black_point
  float grey_source;          // computed from user grey_point / 100
  float contrast;             // clamped/adjusted contrast
  float saturation;           // normalized from user percentage
  float sigma_toe, sigma_shoulder;  // computed from spline
  struct dt_iop_filmic_rgb_spline_t spline;  // fully solved spline LUT
  // ... no direct copy of params fields
} dt_iop_filmicrgb_data_t;
```

**The data flow:**
```
Database ──load──→ self->params ──UI widgets──→ self->params
                                                    │
                                             commit_params()
                                                    │
                                                    ▼
                                              piece->data  ──→ process()
                                          (params_t or data_t)
```

When a `data_t` is used, you must also implement `init_pipe()` and `cleanup_pipe()` to allocate/free it. See [Pipe Lifecycle Functions](#pipe-lifecycle-functions) below.

---

## Required Functions

### `name()` / `default_colorspace()`

```c
const char *name() { return _("my module"); }

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
    dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;  // or IOP_CS_LAB, IOP_CS_RAW
}
```

### `process()` - Image Processing

```c
void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  // Get processing data — this is piece->data as prepared by commit_params().
  const dt_iop_mymodule_params_t *d = piece->data;
  const size_t ch = piece->colors;  // Usually 4 (RGBA)

  // Validate input format
  if(!dt_iop_have_required_input_format(4, self, piece->colors,
                                        ivoid, ovoid, roi_in, roi_out))
    return;

  // Process pixels
  DT_OMP_FOR()
  for(int j = 0; j < roi_out->height; j++)
  {
    const float *in = ((float *)ivoid) + (size_t)ch * roi_in->width * j;
    float *out = ((float *)ovoid) + (size_t)ch * roi_out->width * j;

    for(int i = 0; i < roi_out->width; i++)
    {
      for_each_channel(c, aligned(in, out))
        out[c] = in[c] * d->exposure;

      in += ch;
      out += ch;
    }
  }
}
```

**Important:**
- **Never use GTK+ API directly in `process()`** — see [GUI.md](GUI.md#3-thread-safety--updating-gui-from-process) for the correct approach
- Use `piece->data` for parameters, not `self->params`
- Use `DT_OMP_FOR()` for parallelization
- Use `for_each_channel()` for vectorization

---

## Processing Fundamentals

### Region of Interest (`dt_iop_roi_t`)

```c
typedef struct dt_iop_roi_t
{
  int x, y, width, height;  // position and dimensions in pixels (at current scale)
  float scale;               // zoom factor relative to full image (0 < scale <= 1.0)
} dt_iop_roi_t;
```

For most modules (those that don't change geometry), `roi_in` and `roi_out` are identical.

### Pipe Types (`dt_dev_pixelpipe_type_t`)

Multiple pipelines may process an image simultaneously. Check `piece->pipe->type` when behavior should differ:

```c
DT_DEV_PIXELPIPE_FULL       // Full-resolution center view
DT_DEV_PIXELPIPE_PREVIEW    // Navigation preview
DT_DEV_PIXELPIPE_PREVIEW2   // Secondary preview (dual view)
DT_DEV_PIXELPIPE_EXPORT     // Full export
DT_DEV_PIXELPIPE_THUMBNAIL  // Thumbnail generation
DT_DEV_PIXELPIPE_SCREEN     // PREVIEW | FULL | PREVIEW2
DT_DEV_PIXELPIPE_ANY        // All types
```

### Input Scaling (`piece->iscale`)

The ratio between the input buffer and the full image. Use to scale spatial parameters:

```c
const float sigma = user_radius * roi_out->scale / piece->iscale;
```

### Pixel Processing Macros

```c
DT_OMP_FOR()                           // OpenMP parallel for with safe defaults
for_each_channel(c, aligned(in, out : 16))  // SIMD-vectorized 4-channel loop
copy_pixel(out, in);                    // 4-channel copy
copy_pixel_nontemporal(out, in);        // bypass cache (sequential writes only)
dt_omploop_sfence();                    // memory fence after nontemporal writes
```

### Buffer Allocation

```c
float *temp = NULL;
if(!dt_iop_alloc_image_buffers(module, roi_in, roi_out,
                                4 | DT_IMGSZ_OUTPUT | DT_IMGSZ_CLEARBUF,
                                &temp, NULL))
  return;  // allocation failed — trouble message already set
// ... use temp ...
dt_free_align(temp);
```

### Trouble Messages

```c
dt_iop_set_module_trouble_message(module,
    _("unsupported input"), _("expected 4-channel input"), NULL);
dt_iop_set_module_trouble_message(module, NULL, NULL, NULL);  // clear
```

### Pipeline Refresh Functions

Trigger reprocessing from GUI callbacks:

```c
dt_iop_refresh_center(module);   // invalidate full pipe, redraw center view
dt_iop_refresh_preview(module);  // invalidate preview pipe
dt_iop_refresh_all(module);      // invalidate all pipes
```

---

## Optional Functions

### Metadata: `description()`, `flags()`, `default_group()`, `aliases()`

These functions provide metadata for the module. See `src/iop/iop_api.h` for signatures.

Common flags: `IOP_FLAGS_INCLUDE_IN_STYLES`, `IOP_FLAGS_SUPPORTS_BLENDING`, `IOP_FLAGS_ALLOW_TILING`, `IOP_FLAGS_HIDDEN`, `IOP_FLAGS_DEPRECATED`, `IOP_FLAGS_ONE_INSTANCE`.

### GUI Functions

For `gui_init()`, `gui_update()`, `gui_changed()`, `gui_cleanup()`, `gui_focus()`, `color_picker_apply()`, and mouse/drawing events, see [GUI.md](GUI.md).

### `init_presets()` - Built-in Presets

Called once per module type to register built-in presets:

```c
void init_presets(dt_iop_module_so_t *self)
{
  dt_iop_mymodule_params_t p = {
    .exposure = 1.0f,
    .gamma = 2.2f,
    .method = METHOD_MANUAL,
    .enabled = TRUE
  };
  dt_gui_presets_add_generic(_("my awesome preset"), self->op, self->version(), &p, sizeof(p));
}
```

---

## Lifecycle Functions

### `init()` / `cleanup()`

Called once per module instance. Usually not needed if using `$DEFAULT` tags.

```c
void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);  // Use introspection defaults
  self->hide_enable_button = TRUE;  // Override specific settings
}
```

### `init_global()` / `cleanup_global()` - Module-Level Resources

Called once per module *type* (not per instance). Used primarily to load OpenCL kernels:

```c
typedef struct dt_iop_mymodule_global_data_t
{
  int kernel_process;
} dt_iop_mymodule_global_data_t;

void init_global(dt_iop_module_so_t *self)
{
  dt_iop_mymodule_global_data_t *gd = calloc(1, sizeof(*gd));
  self->data = gd;
  gd->kernel_process = dt_opencl_create_kernel(42, "mymodule_process");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_mymodule_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_process);
  free(gd);
}
```

Access in `process_cl()` via `self->global_data`.

### `reload_defaults()` - Per-Image Defaults

Called when switching images. Update defaults based on image properties:

```c
void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_mymodule_params_t *d = self->default_params;
  const dt_image_t *img = &self->dev->image_storage;

  if(!dt_image_is_raw(img))
    self->default_enabled = FALSE;
}
```

Common checks: `dt_image_is_raw()`, `dt_image_is_hdr()`, `dt_image_is_ldr()`, `dt_image_is_monochrome()`, `dt_image_is_bayerRGB()`.

### Pipe Lifecycle Functions

Each pixelpipe has its own copy of every module's data via `dt_dev_pixelpipe_iop_t` ("piece").

#### `init_pipe()` / `cleanup_pipe()`

**Default behavior** (if not implemented): allocates/frees `self->params_size` bytes.

**Custom implementation** (required when using a separate `data_t`):

```c
void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_mymodule_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_mymodule_data_t *d = piece->data;
  dt_free_align(d->gamut_LUT);  // free any sub-allocations
  dt_free_align(piece->data);
  piece->data = NULL;
}
```

### `commit_params()` - Transform UI Parameters into Processing Data

Called by the framework whenever parameters are synced to the pixelpipe. Its job is to translate `self->params` into processing-ready `piece->data`.

**Caching note:** The framework hashes `piece->data` after this function returns. If the hash changes, the cache for this module and all subsequent ones is invalidated. Ensure deterministic output for identical inputs.

**Simple case** — no transformation needed, default `memcpy` suffices. Don't implement this function.

**Common case** — precompute expensive values:

```c
void commit_params(dt_iop_module_t *self, dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_mymodule_params_t *p = (dt_iop_mymodule_params_t *)p1;
  dt_iop_mymodule_data_t *d = piece->data;

  d->exposure_scale = exp2f(p->exposure);        // EV → linear multiplier
  d->blending = p->blending / 100.0f;            // percentage → 0-1 range
  dt_iop_compute_spline(p, &d->spline);          // solve spline
}
```

---

## Version Migration

### `legacy_params()` - Upgrade Old Parameters

```c
int legacy_params(dt_iop_module_t *self,
                  const void *const old_params, const int old_version,
                  void **new_params, int32_t *new_params_size, int *new_version)
{
  if(old_version == 1)
  {
    typedef struct { float exposure; } v1_params_t;
    const v1_params_t *o = old_params;
    dt_iop_mymodule_params_t *n = malloc(sizeof(dt_iop_mymodule_params_t));

    n->exposure = o->exposure;
    n->gamma = 1.0f;          // default for new field
    n->method = METHOD_AUTO;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_mymodule_params_t);
    *new_version = 2;
    return 0;
  }
  return 1;  // Unknown version
}
```

---

## OpenCL Processing (Optional)

If the module can use the GPU, implement `process_cl()` wrapped in `#ifdef HAVE_OPENCL`. OpenCL kernels are loaded in `init_global()` and accessed via `self->global_data`. The pipeline falls back to CPU `process()` on failure. See existing modules (e.g., `exposure.c`, `sharpen.c`) for complete examples.

---

## Tiling Support

If `IOP_FLAGS_ALLOW_TILING` is set, implement `tiling_callback()` to report memory requirements:

| Field | Purpose |
|-------|---------|
| `factor` / `factor_cl` | Total CPU/GPU memory as a multiple of input buffer size |
| `maxbuf` / `maxbuf_cl` | Largest single temporary buffer as a multiple of input size |
| `overhead` | Fixed memory overhead in bytes |
| `overlap` | Pixels of overlap between adjacent tiles (for spatial filters) |
| `xalign` / `yalign` | Tile origin alignment (1 = none, 2 = Bayer pattern) |

```c
void tiling_callback(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  tiling->factor = 2.5f;     // input + 1.5× temp buffers
  tiling->factor_cl = 2.5f;
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 4;       // 4-pixel overlap for a 3×3 kernel
  tiling->xalign = 1;
  tiling->yalign = 1;
}
```

---

## Geometry Functions (Optional)

For modules that change image geometry (crop, rotate, lens correction), implement `modify_roi_in()`, `modify_roi_out()`, `distort_transform()`, `distort_backtransform()`, and optionally `distort_mask()`. See `src/iop/iop_api.h` for signatures and modules like `ashift.c` for examples.

---

## Quick Reference: Function Call Order

```
Module Load:
  init_global() → [once per module type]

Image Open:
  init() → reload_defaults() → gui_init()

Pixelpipe Creation (per pipe):
  init_pipe()  [allocates piece->data]

Params Change:
  gui_update() → gui_changed()

User Edits Widget:
  [auto-callback] → gui_changed()
       → commit_params()  [transforms self->params → piece->data]
       → process()        [reads piece->data]

Image Switch:
  reload_defaults() → gui_update() → gui_changed()

Darkroom Exit:
  gui_cleanup() → cleanup_pipe() [per pipe] → cleanup()

Module Unload:
  cleanup_global()
```
