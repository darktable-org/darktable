# IOP Module API Reference

This guide documents the functions that darktable Image Operation (IOP) modules can implement. The API is defined in `src/iop/iop_api.h` and used via `src/develop/imageop.h`. See `src/iop/useless.c` for a fully documented example module.

See also:
- [Pixelpipe Architecture](pixelpipe_architecture.md) for pipeline data flow and caching.
- [Introspection System](introspection.md) for parameter management.
- [GUI Architecture](GUI.md) for GUI events, callbacks, and widget reparenting.
- [GUI Threading](GUI_Threading.md) for sharing `gui_data` between the GTK and pipe worker threads.

---

## What a Module Is Made Of

**Required — every IOP module has these:**
1. **Parameter struct** (`dt_iop_modulename_params_t`) - the user-facing parameters, serialized to the database, controlled via UI widgets in `self->params`
2. **Core functions** - `name()`, `default_colorspace()`, `process()`

**Conditional — present when the module's shape calls for them:**

3. **Processing data struct** (`dt_iop_modulename_data_t`) - a processing-optimized version of the parameters, stored in `piece->data` and used by `process()`; when not provided, `piece->data` is a plain copy of `params_t` (see [params_t vs data_t](#params_t-vs-data_t--the-two-parameter-structs) below). A module that defines one usually implements [`init_pipe()` and `cleanup_pipe()`](#init_pipe--cleanup_pipe) too.
4. **GUI data struct** (`dt_iop_modulename_gui_data_t`) - widget references, present only where the module has a GUI: the darkroom, plus a throw-away instance built at startup. A hidden module never gets one.

**Optional:**

5. **Other callbacks** - GUI, lifecycle, geometry, metadata, etc.

---

## Data Structures

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
                                                    │   the pipe's defaults sync
                                                    │   passes default_params
                                                    ▼   here instead
                                            commit_params(p)
                                                    │
                                                    ▼
                                              piece->data  ──→ process()
                                          (params_t or data_t)
```

When a `data_t` is used, allocating and freeing it is usually yours to do in `init_pipe()` and `cleanup_pipe()`. See [Pipe Lifecycle Functions](#pipe-lifecycle-functions) below for when the defaults are enough.

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
- **Never use GTK API directly in `process()`** — see [GUI_Threading.md](GUI_Threading.md) for the correct approach
- **`commit_params()` has no thread affinity either** — `gui_data` it shares with widget callbacks needs the GUI critical section, and only when a GUI exists; see [`commit_params()`](#commit_params---transform-parameters-into-processing-data)
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

Multiple pipelines may process an image simultaneously. Check `dt_pipe_is_full()` and friends when behavior should differ:

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

For `gui_init()`, `gui_update()`, `gui_changed()`, `gui_cleanup()`, `color_picker_apply()`, and mouse/drawing events, see [GUI.md](GUI.md).

`gui_focus(self, in)` is not covered there: the framework calls it when the module gains or loses focus in the darkroom (`src/iop/iop_api.h`). `rasterfile` uses it to re-read a file that may have changed while the module was not in focus (`src/iop/rasterfile.c`).

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

### `change_image()` - Reset GUI State for the New Image

Called on an image switch, after `reload_defaults()` and before the new image's params reach `gui_update()`. Switching image does not tear down every module: each module's base instance — the one with the lowest `multi_priority` — is kept, GUI and all, and reused for the new image (`src/views/darkroom.c`). Anything in `gui_data` that describes the *old* image — a cached curve, a selected region, a pending readout — therefore survives unless you clear it here:

```c
void change_image(dt_iop_module_t *self)
{
  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  if(!g) return;

  g->cache_valid = FALSE;
  g->selected_node = -1;
}
```

`gui_cleanup()` does **not** run for that retained instance — only the module's extra instances are torn down, and what replaces them is whatever the new image's history needs — so `change_image()` is also where a module that schedules GUI updates from the pipe has to cancel them; see [GUI_Threading.md](GUI_Threading.md#the-callback-must-not-outlive-the-module-or-the-image). `basicadj`, `retouch`, `rgblevels` and `rgbcurve` implement it, and each also calls it from `gui_init()` to set the same initial state.

### Pipe Lifecycle Functions

Each pixelpipe has its own copy of every module's data via `dt_dev_pixelpipe_iop_t` ("piece").

#### `init_pipe()` / `cleanup_pipe()`

**Default behavior** (if not implemented): `calloc()`s `self->params_size` bytes and `free()`s them (`src/develop/imageop.c`). That is enough for a `data_t` no larger than `params_t` that owns no sub-allocations — `rasterfile` defines its own `data_t` and relies on the defaults.

**Custom implementation**, needed when the `data_t` is larger than the `params_t`, or owns memory that has to be released with it:

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

### `commit_params()` - Transform Parameters into Processing Data

Called by the framework whenever parameters are synced to the pixelpipe. Its job is to translate the incoming parameters into processing-ready `piece->data`. Work from the `params` argument, not from `self->params`: they are the same in the normal case, but the pipe's defaults sync passes `default_params` instead (`src/develop/pixelpipe_hb.c`).

**Threading note:** `commit_params()` has no thread affinity. For your darkroom instance it usually runs on a pixelpipe worker thread, but it also runs on the GTK main thread when an image switch rebuilds the screen pipes' nodes. Any `gui_data` it shares with widget callbacks therefore needs the GUI critical section, i.e. the module's `gui_lock`.

Enter that section only when `gui_data` is non-NULL: `gui_data` is `NULL` on export and `gui_lock` is only initialized when a GUI exists, so the non-GUI branch must compute what processing needs on its own. The in-tree idiom writes `self->dev->gui_attached && g`. The `dev` dereference is safe here, since a module always has a `dev` by the time a pipe commits it, but `g` is the half that does the work — and even `g` is not quite proof that `gui_lock` was initialized; see [GUI_Threading.md](GUI_Threading.md#using-gui_data-from-commit_params).

**Caching note:** After this function returns, `dt_iop_commit_params()` hashes the operation name, the instance, `module->params`, and the blend parameters and mask group if blending is on (`src/develop/imageop.c`). Read that list literally in both directions: it is `module->params`, *not* the `params` argument your callback was just handed — on the defaults sync those are different objects — and it never includes `piece->data`, the output you just produced. The hash is also the wrapper's work, not the callback's, so a `commit_params()` reached another way does not update it; `basecurve` calls its own directly from `init_pipe()`. If that `piece->hash` changes, the cache for this module and all subsequent ones is invalidated.

`piece->hash` is only part of the cache key: a lookup also folds in the image id, the color profiles, the hashes of the preceding pieces and — when it supplies a ROI, which is the normal case — the pipe type, the detail-mask flag, the ROI, the Scharr state and the color picker's sample (`dt_dev_pixelpipe_cache_hash()`, `src/develop/pixelpipe_cache.c`; the full list is in [pixelpipe_architecture.md](pixelpipe_architecture.md#hash-based-caching)). So pipe type, scale and color profiles do not need to be in `params` — a normal lookup has them already.

There is no hook for adding to that key. The wrapper builds `piece->hash` after your callback returns and assigns it unconditionally, and `src/iop/iop_api.h` declares nothing else for the purpose. So keep the output a deterministic function of what the key represents, and give any other input either a place in the key or an explicit invalidation path. The three routes are to put a serializable value in `module->params`, to use a pipe field the framework already hashes, or to invalidate explicitly; which inputs the key leaves out, and which route suits which, are set out in [pixelpipe_architecture.md — What the Cache Key Does Not Cover](pixelpipe_architecture.md#what-the-cache-key-does-not-cover).

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

If `IOP_FLAGS_ALLOW_TILING` is set, the pixelpipe is allowed to process a piece in tiling mode. If some parameter combinations do not allow tiling, clear that permission for the piece in `commit_params()`.

Memory requirements and tile alignment are reported by `tiling_callback()`. A module that does not provide one gets the defaults computed by `default_tiling_callback()`.

Provide a specific `tiling_callback()` whenever the module may exceed the requirements assumed by `default_tiling_callback()`, or needs special alignment, so that:

- the tiling process will not allocate more memory than it was granted;
- the OpenCL code path will not be tried when the requirements are too high, which avoids costly late fallbacks to the CPU path;
- tile stitching will be correct for the alignment the module needs.

| Field | Purpose |
|-------|---------|
| `factor` / `factor_cl` | Total CPU/GPU memory as a multiple of input buffer size |
| `maxbuf` / `maxbuf_cl` | Largest single temporary buffer as a multiple of input size |
| `overhead` | Fixed memory overhead in bytes |
| `overlap` | Pixels of overlap between adjacent tiles (for spatial filters) |
| `align` | Tile origin alignment (1 = none, other values only for special algorithms) |


An example
```c
void tiling_callback(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  tiling->factor = 2.5f;     // input + output + 2 single channel temp buffers
  tiling->factor_cl = 3.75f; // as above but we need an additional rgb buffer plus a single channel buffer for a mask
  tiling->maxbuf = 1.0f;
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 4;       // 4-pixel overlap for a 3×3 kernel
  tiling->align = 1;         // no special care for sensor patterns
}
```

---

## Geometry Functions (Optional)

For modules that change image geometry (crop, rotate, lens correction), implement `modify_roi_in()`, `modify_roi_out()`, `distort_transform()`, `distort_backtransform()`, and optionally `distort_mask()`. See `src/iop/iop_api.h` for signatures and modules like `ashift.c` for examples.

---

## Quick Reference: Function Call Order

```
Module Load:
  init_global()  [once per module type]

Image Open:  [simplified]
  init() → reload_defaults() → gui_init() → reload_defaults()
                                            [defaults are recomputed
                                             once the widgets exist]

Pixelpipe Creation (per pipe):
  init_pipe()  [allocates piece->data]

Params Change:
  gui_update() → gui_changed() [if implemented]

User Edits Widget:
  [auto-callback] → gui_changed() [if implemented]
       → commit_params(p)  [transforms its p argument → piece->data]
       → process()         [reads piece->data]

Image Switch:  [the base instance is kept; extra instances are destroyed,
                then rebuilt from the new image's history]
  reload_defaults() → change_image() [if implemented] → [history params loaded]
       → gui_update() → gui_changed() [if implemented]

Darkroom Exit:
  cleanup_pipe() [per pipe] → gui_cleanup() → cleanup()

Module Unload:
  cleanup_global()
```

`[if implemented]` marks the optional callbacks: the framework skips them for a module that does not have them. The GUI steps are skipped entirely for a hidden module. See [GUI.md](GUI.md) for the full event flow.
