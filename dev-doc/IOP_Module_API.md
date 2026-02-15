# IOP Module API Reference

This guide documents the functions that darktable Image Operation (IOP) modules can implement. The API is defined in `src/iop/iop_api.h` and used via `src/develop/imageop.h`. See `src/iop/useless.c` for a fully documented example module.

See also:
- [Pixelpipe Architecture](pixelpipe_architecture.md) for details on the processing pipeline.
- [Introspection System](introspection.md) for details on parameter management.

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

### GUI Data Struct

```c
typedef struct dt_iop_mymodule_gui_data_t
{
  GtkWidget *exposure;  // Slider reference
  GtkWidget *gamma;
  GtkWidget *method;    // Combobox reference
  GtkWidget *enabled;   // Toggle reference

  // Any other GUI state needed between callbacks
} dt_iop_mymodule_gui_data_t;
```

### `params_t` vs `data_t` — The Two Parameter Structs

A common source of confusion is the relationship between the parameter struct (`params_t`) and the processing data struct (`data_t`). They serve different purposes:

| | `dt_iop_modulename_params_t` | `dt_iop_modulename_data_t` |
|---|---|---|
| **Where it lives** | `self->params` | `piece->data` |
| **Source** | Database / UI widgets | Built by `commit_params()` |
| **Purpose** | Record user intent in a stable, serializable format | Provide processing-ready values to `process()` |
| **May contain** | Raw user values (e.g. EV, percentages, enum choices) | Precomputed LUTs, splines, normalized/transformed values, expensive one-time calculations |
| **Constraints** | No pointers; must be serializable; changing it requires a version bump | No constraints — can contain pointers, LUTs, runtime-only data |

**When you don't need a `data_t`:** If `process()` can work directly from the raw user parameters without any transformation, you don't need a separate `data_t`. The default `init_pipe()` allocates `piece->data` as a `params_t`-sized buffer, and the default `commit_params()` does `memcpy(piece->data, params, self->params_size)`. Simple modules like can use this approach — `process()` casts `piece->data` directly to `params_t *`.

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

// toneequal.c: data_t contains precomputed correction LUT
typedef struct dt_iop_toneequalizer_data_t
{
  float factors[PIXEL_CHAN];                              // solved interpolation
  float correction_lut[PIXEL_CHAN * LUT_RESOLUTION + 1]; // precomputed LUT
  float blending, feathering;                             // normalized from UI %
  // ...
} dt_iop_toneequalizer_data_t;
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

### `name()` - Module Display Name

```c
const char *name()
{
  return _("my module");  // Translatable
}
```

### `default_colorspace()` - Working Colorspace

```c
dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
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
  // If using a custom data_t, cast to that instead of params_t.
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
      // Process pixel
      for_each_channel(c, aligned(in, out))
        out[c] = in[c] * d->exposure;

      in += ch;
      out += ch;
    }
  }
}
```

**Important:**
- **Never use GTK+ API directly in `process()`** - it runs in worker threads (see below for the correct approach)
- Use `piece->data` for parameters, not `self->params`
- Use `DT_OMP_FOR()` for parallelization
- Use `for_each_channel()` for vectorization

### Updating GUI from `process()` - Thread Safety

GTK+ is not thread-safe. If `process()` needs to update the GUI (e.g., display a computed value), you must schedule the update to run in the main GTK thread using `g_idle_add()`.

**Pattern:**
1. Compute the value in `process()`
2. Store it in `gui_data` (protected by critical section)
3. Schedule a callback with `g_idle_add()`
4. The callback runs in GTK thread and updates widgets

**Example from exposure.c - displaying computed deflicker value:**

```c
// Forward declaration of GTK-thread callback
static gboolean _show_computed(gpointer user_data);

// In process() or helper called from process():
static void _process_common_setup(dt_iop_module_t *self,
                                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_exposure_gui_data_t *g = self->gui_data;

  // ... compute exposure value ...

  if(g)  // GUI might not exist (export mode)
  {
    // Store result in gui_data with mutex protection
    dt_iop_gui_enter_critical_section(self);
    g->deflicker_computed_exposure = exposure;
    dt_iop_gui_leave_critical_section(self);

    // Schedule GUI update in main thread
    g_idle_add(_show_computed, self);
  }
}

// Callback runs in GTK main thread - safe to use GTK+ API
static gboolean _show_computed(gpointer user_data)
{
  dt_iop_module_t *self = user_data;
  dt_iop_exposure_gui_data_t *g = self->gui_data;

  // Protect access to gui_data
  dt_iop_gui_enter_critical_section(self);
  if(g->deflicker_computed_exposure != UNDEFINED)
  {
    // Now safe to call GTK+ functions
    gchar *str = g_strdup_printf(_("%.2f EV"), g->deflicker_computed_exposure);
    gtk_label_set_text(g->deflicker_used_EC, str);
    g_free(str);
  }
  dt_iop_gui_leave_critical_section(self);

  return G_SOURCE_REMOVE;  // Don't repeat, remove from idle queue
}
```

**Key functions:**

| Function | Purpose |
|----------|---------|
| `g_idle_add(callback, data)` | Schedule callback in GTK main thread |
| `dt_iop_gui_enter_critical_section(self)` | Lock `gui_data` mutex |
| `dt_iop_gui_leave_critical_section(self)` | Unlock `gui_data` mutex |
| `dt_control_queue_redraw_widget(widget)` | Thread-safe widget redraw (uses `g_idle_add` internally) |
| `dt_control_queue_redraw_center()` | Thread-safe center view redraw |

**The callback must return:**
- `G_SOURCE_REMOVE` (or `FALSE`) - Run once, then remove from queue
- `G_SOURCE_CONTINUE` (or `TRUE`) - Keep in queue, run again

**Alternative pattern: Message passing (from gamutcompress.c)**

Instead of storing in gui_data with critical sections, allocate a message struct that the callback owns and frees:

```c
// Message struct to pass data to GTK thread
typedef struct
{
  dt_iop_module_t *self;
  float computed_values[3];
  float ratio;
} mymodule_gui_update_t;

// Callback runs in GTK thread
static gboolean _update_gui_from_worker(gpointer data)
{
  mymodule_gui_update_t *msg = data;
  dt_iop_module_t *self = msg->self;
  dt_iop_mymodule_gui_data_t *g = self->gui_data;

  if(g)  // GUI might have been destroyed
  {
    // Store values and trigger redraw
    memcpy(g->display_values, msg->computed_values, sizeof(g->display_values));
    g->display_ratio = msg->ratio;
    gtk_widget_queue_draw(self->widget);
  }

  g_free(msg);  // Callback owns the message, must free it
  return G_SOURCE_REMOVE;
}

// At end of process():
void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, ...)
{
  dt_iop_mymodule_gui_data_t *g = self->gui_data;

  // ... image processing ...

  // Only send GUI update for full pipe with GUI attached
  if(g != NULL
     && self->dev->gui_attached
     && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
  {
    // Allocate message - callback will free it
    mymodule_gui_update_t *msg = g_malloc(sizeof(mymodule_gui_update_t));
    msg->self = self;
    memcpy(msg->computed_values, local_values, sizeof(msg->computed_values));
    msg->ratio = computed_ratio;

    g_idle_add(_update_gui_from_worker, msg);
  }
}
```

**Advantages of message passing:**
- No critical sections needed (message is owned by callback)
- Clear ownership semantics
- Works correctly if multiple `process()` calls happen before callback runs

**Important checks before sending:**
- `g != NULL` - GUI exists (not in export mode)
- `self->dev->gui_attached` - Darkroom is active
- `piece->pipe->type & DT_DEV_PIXELPIPE_FULL` - Only for full preview, not thumbnails

**Common mistakes:**
```c
// WRONG - GTK+ call directly in process()
void process(...) {
  gtk_label_set_text(g->label, "value");  // CRASH or undefined behavior!
}

// WRONG - No mutex protection when storing in gui_data
void process(...) {
  g->computed_value = result;  // Race condition!
  g_idle_add(update_gui, self);
}

// WRONG - Forgetting to free the message
static gboolean callback(gpointer data) {
  // ... use data ...
  return G_SOURCE_REMOVE;  // Memory leak! Must g_free(data)
}

// WRONG - Sending updates for preview/thumbnail pipes
if(g != NULL) {  // Missing pipe type check
  g_idle_add(...);  // Will flood with unnecessary updates
}
```

---

## Processing Fundamentals

### Region of Interest (`dt_iop_roi_t`)

Defined in `develop/pixelpipe.h`:

```c
typedef struct dt_iop_roi_t
{
  int x, y, width, height;  // position and dimensions in pixels (at current scale)
  float scale;               // zoom factor relative to full image (0 < scale <= 1.0)
} dt_iop_roi_t;
```

In `process()`, `roi_in` and `roi_out` describe the input and output buffer regions. For most modules (those that don't change geometry), `roi_in` and `roi_out` are identical. Modules implementing `modify_roi_in()` / `modify_roi_out()` may have different input and output regions.

### Pipe Types (`dt_dev_pixelpipe_type_t`)

Multiple pipelines may process an image simultaneously. Check `piece->pipe->type` when behavior should differ per pipeline:

```c
// Defined in develop/pixelpipe.h as bit flags:
DT_DEV_PIXELPIPE_FULL       // Full-resolution center view
DT_DEV_PIXELPIPE_PREVIEW    // Navigation preview
DT_DEV_PIXELPIPE_PREVIEW2   // Secondary preview (dual view)
DT_DEV_PIXELPIPE_EXPORT     // Full export
DT_DEV_PIXELPIPE_THUMBNAIL  // Thumbnail generation

// Convenience combinations:
DT_DEV_PIXELPIPE_SCREEN     // PREVIEW | FULL | PREVIEW2
DT_DEV_PIXELPIPE_ANY        // All types
```

Common usage:
```c
// Only do expensive work for the full view
if(piece->pipe->type & DT_DEV_PIXELPIPE_FULL)
{
  // ... expensive computation or GUI update ...
}
```

### Pixel Channel Count (`piece->colors`)

Set automatically by the pipeline: `1` for RAW colorspace, `4` for RGB/Lab (RGBA with alpha). Always use this rather than hardcoding channel counts. Validate in `process()` with:

```c
if(!dt_iop_have_required_input_format(4 /*channels*/, self, piece->colors,
                                      ivoid, ovoid, roi_in, roi_out))
  return;  // wrong format — input copied to output, trouble message set
```

### Input Scaling (`piece->iscale`)

The ratio between the input buffer and the full image: `piece->iscale * buffer_width = full_image_width`. Use this to scale spatial parameters (blur radii, feathering) so they behave consistently at all zoom levels:

```c
const float sigma = user_radius * roi_out->scale / piece->iscale;
```

### Pixel Processing Macros

These macros from `common/darktable.h` and `common/dttypes.h` are used throughout `process()` functions:

```c
// OpenMP parallel for with safe defaults (firstprivate, static scheduling)
DT_OMP_FOR()
for(int j = 0; j < roi_out->height; j++) { ... }

// SIMD-vectorized channel loop (4 channels, auto-vectorized with OpenMP SIMD)
for_each_channel(c, aligned(in, out : 16))
  out[c] = in[c] * factor;

// Variants:
for_four_channels(c, ...)   // explicit 4-channel loop
for_three_channels(c, ...)  // explicit 3-channel loop
```

### Pixel Copy Functions

```c
// Standard per-pixel copy (4 channels, SIMD-friendly)
copy_pixel(out, in);                // defined in common/darktable.h

// Nontemporal copy — bypasses CPU cache, faster for sequential full-image writes
// Do NOT use if you'll read the destination pixel again soon
copy_pixel_nontemporal(out, in);    // defined in develop/imageop.h

// After a loop using nontemporal writes, ensure visibility:
dt_omploop_sfence();  // emits memory fence only if needed (no-op inside OpenMP loops)
```

### Buffer Allocation

```c
#include "common/imagebuf.h"

float *temp = NULL;
if(!dt_iop_alloc_image_buffers(module, roi_in, roi_out,
                                4 | DT_IMGSZ_OUTPUT | DT_IMGSZ_CLEARBUF,
                                &temp, NULL))
  return;  // allocation failed — trouble message already set

// ... use temp ...

dt_free_align(temp);
```

Flags (OR together): `DT_IMGSZ_OUTPUT` / `DT_IMGSZ_INPUT` (dimensions from which roi), `DT_IMGSZ_CLEARBUF` (zero-fill), `DT_IMGSZ_PERTHREAD` (per-thread buffer), `DT_IMGSZ_FULL` / `DT_IMGSZ_WIDTH` / `DT_IMGSZ_HEIGHT` / `DT_IMGSZ_LONGEST` (dimension selection). The channel count (1–4) is OR'd into the low bits.

ROI-aware copy (handles size mismatch with zero-padding or cropping):
```c
dt_iop_copy_image_roi(out, in, ch, roi_in, roi_out);
```

### Trouble Messages

Report errors or warnings that appear as an icon on the module header:

```c
// Set trouble indicator
dt_iop_set_module_trouble_message(module,
    _("unsupported input"),           // short message (shown in UI)
    _("expected 4-channel input"),    // tooltip
    "module got wrong channel count"); // stderr (can be NULL)

// Clear trouble indicator
dt_iop_set_module_trouble_message(module, NULL, NULL, NULL);
```

### Pipeline Refresh Functions

Trigger reprocessing from GUI callbacks (outside the normal `from_params` → `gui_changed` → history path):

```c
dt_iop_refresh_center(module);   // invalidate full/center pipe, redraw center view
dt_iop_refresh_preview(module);  // invalidate preview pipe
dt_iop_refresh_all(module);      // invalidate all pipes
```

---

## Metadata Functions

### `description()` - Tooltip Description

```c
const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self,
    _("adjusts exposure and gamma"),  // Main description
    _("corrective and creative"),     // Purpose
    _("linear, RGB, scene-referred"), // Input
    _("linear, RGB"),                 // Processing
    _("linear, RGB, scene-referred")); // Output
}
```

### `flags()` - Module Capabilities

```c
int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES
       | IOP_FLAGS_SUPPORTS_BLENDING
       | IOP_FLAGS_ALLOW_TILING;
}
```

Common flags:
- `IOP_FLAGS_INCLUDE_IN_STYLES` - Include in style export by default
- `IOP_FLAGS_SUPPORTS_BLENDING` - Enable blend modes and masks
- `IOP_FLAGS_ALLOW_TILING` - Allow tiled processing (requires `tiling_callback`)
- `IOP_FLAGS_HIDDEN` - Don't show in module list
- `IOP_FLAGS_DEPRECATED` - Mark as deprecated
- `IOP_FLAGS_ONE_INSTANCE` - Disallow multiple instances

### `default_group()` - Module Group

```c
int default_group()
{
  return IOP_GROUP_TECHNICAL;  // or IOP_GROUP_GRADING, IOP_GROUP_EFFECTS
}
```

### `aliases()` - Search Keywords

```c
const char *aliases()
{
  return _("brightness|gamma|levels");  // Pipe-separated
}
```

---

## GUI Functions

### `gui_init()` - Create Widgets

Called once when entering darkroom. Create and configure widgets here.

```c
void gui_init(dt_iop_module_t *self)
{
  dt_iop_mymodule_gui_data_t *g = IOP_GUI_ALLOC(mymodule);

  // Create main container (auto-created if first widget uses _from_params)
  self->widget = dt_gui_vbox();

  // Create widgets linked to params
  g->exposure = dt_bauhaus_slider_from_params(self, "exposure");
  dt_bauhaus_slider_set_format(g->exposure, " EV");
  dt_bauhaus_slider_set_soft_range(g->exposure, -4.0, 4.0);

  g->gamma = dt_bauhaus_slider_from_params(self, "gamma");
  dt_bauhaus_slider_set_digits(g->gamma, 2);

  g->method = dt_bauhaus_combobox_from_params(self, "method");

  g->enabled = dt_bauhaus_toggle_from_params(self, "enabled");
}
```

**Do NOT:**
- Set widget values (that's for `gui_update`)
- Call `gui_update` from here

### `gui_update()` - Sync Widgets from Params

Called when params change (image switch, history navigation, etc.).

```c
void gui_update(dt_iop_module_t *self)
{
  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  dt_iop_mymodule_params_t *p = self->params;

  // Sliders and comboboxes auto-sync, but toggles need manual update
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->enabled), p->enabled);

  // Always call gui_changed at the end
  gui_changed(self, NULL, NULL);
}
```

### `gui_changed()` - React to Widget Changes

Called automatically when introspection widgets change. Also call from `gui_update`.

```c
void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  dt_iop_mymodule_params_t *p = self->params;

  // Adjust UI state based on current params
  if(!w || w == g->method)
  {
    // Show/hide widgets based on method selection
    gtk_widget_set_visible(g->gamma, p->method == METHOD_MANUAL);
  }
}
```

### `gui_cleanup()` - Free Resources

Only needed if `gui_init` allocated extra resources (rare).

```c
void gui_cleanup(dt_iop_module_t *self)
{
  // Free any manually allocated GUI resources
  // (widget destruction is automatic)
}
```

### `gui_reset()` - Reset UI to Defaults

Called when user clicks reset. Usually not needed.

### `gui_focus()` - Focus Notification

```c
void gui_focus(dt_iop_module_t *self, gboolean in)
{
  if(in)
  {
    // Module gained focus - maybe show guides
  }
  else
  {
    // Module lost focus
  }
}
```

---

## Lifecycle Functions

### `init()` - Initialize Module Instance

Called once per module instance. Usually not needed if using `$DEFAULT` tags.

```c
void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);  // Use introspection defaults

  // Override specific settings
  self->hide_enable_button = TRUE;  // Hide on/off switch
}
```

### `cleanup()` - Cleanup Module Instance

```c
void cleanup(dt_iop_module_t *self)
{
  dt_iop_default_cleanup(self);
  // Free any extra allocations from init()
}
```

### `init_global()` / `cleanup_global()` - Module-Level Resources

Called once per module *type* (not per instance) at startup and shutdown. Used primarily to load OpenCL kernels and precompute shared data that doesn't change per image.

```c
// Global data struct — typically holds OpenCL kernel handles
typedef struct dt_iop_mymodule_global_data_t
{
  int kernel_process;
  int kernel_blend;
  float *precomputed_lut;  // shared across all instances
} dt_iop_mymodule_global_data_t;

void init_global(dt_iop_module_so_t *self)
{
  const int program = 42;  // OpenCL program index from programs.conf
  dt_iop_mymodule_global_data_t *gd = calloc(1, sizeof(*gd));
  self->data = gd;

  gd->kernel_process = dt_opencl_create_kernel(program, "mymodule_process");
  gd->kernel_blend = dt_opencl_create_kernel(program, "mymodule_blend");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_mymodule_global_data_t *gd = self->data;
  dt_opencl_free_kernel(gd->kernel_process);
  dt_opencl_free_kernel(gd->kernel_blend);
  free(gd);
}
```

Access in `process_cl()` via `self->global_data`. Note: the `self` parameter is `dt_iop_module_so_t *` (the shared module object), not `dt_iop_module_t *` (the per-instance object).

### `reload_defaults()` - Per-Image Defaults

Called when switching images. Update defaults based on image properties. Common image property checks (from `common/image.h`):

```c
const dt_image_t *img = &self->dev->image_storage;

dt_image_is_raw(img)          // RAW file
dt_image_is_hdr(img)          // HDR (float) data
dt_image_is_ldr(img)          // LDR (8/16-bit) data
dt_image_is_monochrome(img)   // Monochrome sensor
dt_image_is_bayerRGB(img)     // Bayer sensor with RGB CFA
```

```c
void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_mymodule_params_t *d = self->default_params;
  const dt_image_t *img = &self->dev->image_storage;

  if(!dt_image_is_raw(img))
  {
    self->default_enabled = FALSE;
  }
  else
  {
    // Set defaults based on image type, EXIF, etc.
    d->exposure = 0.0f;
  }

  // Update widget defaults if GUI is active
  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  if(g)
  {
    dt_bauhaus_slider_set_default(g->exposure, d->exposure);
  }
}
```

### Pipe Lifecycle Functions

Each pixelpipe (full preview, thumbnail, export) has its own copy of every module's data via `dt_dev_pixelpipe_iop_t` ("pipe piece"). The `init_pipe()` and `cleanup_pipe()` functions manage the `piece->data` allocation for each pipe piece.

#### `init_pipe()` - Allocate Per-Pipe Processing Data

Called when the pixelpipe creates a piece for this module. Allocates `piece->data`.

**Default behavior** (if you don't implement this): allocates `self->params_size` bytes via `calloc`. This is appropriate when `piece->data` is just a copy of `params_t`.

**Custom implementation** (required when using a separate `data_t`):

```c
void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_mymodule_data_t));
}
```

For structs requiring alignment (e.g. containing `DT_ALIGNED_ARRAY` members), use:
```c
piece->data = dt_calloc1_align_type(dt_iop_mymodule_data_t);
```

#### `cleanup_pipe()` - Free Per-Pipe Processing Data

Called when the pixelpipe destroys a piece for this module.

**Default behavior**: calls `free(piece->data)`.

**Custom implementation** (required if `init_pipe` allocated additional resources):

```c
void cleanup_pipe(dt_iop_module_t *self,
                  dt_dev_pixelpipe_t *pipe,
                  dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_mymodule_data_t *d = piece->data;
  dt_free_align(d->gamut_LUT);  // free any sub-allocations
  dt_free_align(piece->data);
  piece->data = NULL;
}
```

### `commit_params()` - Transform UI Parameters into Processing Data

Called by the framework (under a pipe mutex) whenever parameters are synced to the pixelpipe — after a widget change, history navigation, image switch, etc. Its job is to translate the user-facing parameters (`self->params`, originating from the database / UI) into processing-ready data stored in `piece->data`, which `process()` will read.

The `p1` argument is a snapshot of `self->params` at commit time. `piece->data` was allocated by `init_pipe()`.

**Simple case — `piece->data` is a direct copy of `params_t`:**

If no transformation is needed, the default implementation does `memcpy(piece->data, p1, self->params_size)` and you don't need to implement this function at all. You can also do the memcpy yourself and add minor adjustments:

```c
void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, p1, self->params_size);
}
```

**Common case — `piece->data` is a separate `data_t` with precomputed fields:**

Most non-trivial modules define a `data_t` that is larger or differently structured than `params_t`. In `commit_params()`, you copy the relevant user values and precompute anything expensive, so `process()` runs as fast as possible.

```c
void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_mymodule_params_t *p = (dt_iop_mymodule_params_t *)p1;
  dt_iop_mymodule_data_t *d = piece->data;

  // Copy relevant user values
  d->exposure = p->exposure;
  d->method = p->method;

  // Precompute processing-ready values
  d->exposure_scale = exp2f(p->exposure);             // EV → linear multiplier
  d->blending = p->blending / 100.0f;                 // percentage → 0-1 range
  dt_iop_compute_spline(p, &d->spline);               // solve spline from control points
  build_correction_lut(d->correction_lut, d->factors); // precompute LUT
}
```

**Real example from exposure.c** — compensates for exposure bias and highlight preservation, precomputes whether deflicker mode is active:

```c
void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_exposure_params_t *p = (dt_iop_exposure_params_t *)p1;
  dt_iop_exposure_data_t *d = piece->data;

  d->params.black = p->black;
  d->params.exposure = p->exposure;

  // Apply exposure bias compensation on top of user value
  if(p->compensate_exposure_bias)
    d->params.exposure -= _get_exposure_bias(self);
  if(p->compensate_hilite_pres)
    d->params.exposure += _get_highlight_bias(self);

  d->deflicker = (p->mode == EXPOSURE_MODE_DEFLICKER
                  && dt_image_is_raw(&self->dev->image_storage)) ? 1 : 0;
}
```

---

## Version Migration

### `legacy_params()` - Upgrade Old Parameters

```c
int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  if(old_version == 1)
  {
    // Define old struct
    typedef struct { float exposure; } v1_params_t;

    const v1_params_t *o = old_params;
    dt_iop_mymodule_params_t *n = malloc(sizeof(dt_iop_mymodule_params_t));

    // Copy existing fields
    n->exposure = o->exposure;
    // Set defaults for new fields
    n->gamma = 1.0f;
    n->method = METHOD_AUTO;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_mymodule_params_t);
    *new_version = 2;
    return 0;  // Success
  }
  return 1;  // Unknown version
}
```

---

## Color Picker

### `color_picker_apply()` - Handle Picked Color

```c
void color_picker_apply(dt_iop_module_t *self,
                        GtkWidget *picker,
                        dt_dev_pixelpipe_t *pipe)
{
  dt_iop_mymodule_params_t *p = self->params;
  dt_iop_mymodule_gui_data_t *g = self->gui_data;

  if(picker == g->exposure_picker)
  {
    // self->picked_color[0..2] = RGB
    // self->picked_color[3] = luminance (if available)
    p->exposure = log2f(self->picked_color[3]);
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
```

---

## Mouse/Drawing Events (Optional)

For modules that draw on the image (crop, masks, etc.):

```c
void gui_post_expose(dt_iop_module_t *self, cairo_t *cr,
                     float width, float height,
                     float pointerx, float pointery, float zoom_scale)
{
  // Draw overlays on the center image
}

int mouse_moved(dt_iop_module_t *self, float x, float y,
                double pressure, int which, float zoom_scale)
{
  return 0;  // Return 1 if event was handled
}

int button_pressed(dt_iop_module_t *self, float x, float y,
                   double pressure, int which, int type,
                   uint32_t state, float zoom_scale)
{
  return 0;
}

int button_released(dt_iop_module_t *self, float x, float y,
                    int which, uint32_t state, float zoom_scale)
{
  return 0;
}

int scrolled(dt_iop_module_t *self, float x, float y,
             int up, uint32_t state)
{
  return 0;
}
```

---

## Geometry Functions (Optional)

For modules that change image geometry (crop, rotate, lens correction):

```c
// How large is output given this input?
void modify_roi_out(dt_iop_module_t *self,
                    dt_dev_pixelpipe_iop_t *piece,
                    dt_iop_roi_t *roi_out,
                    const dt_iop_roi_t *roi_in)

// What input is needed to fill this output?
void modify_roi_in(dt_iop_module_t *self,
                   dt_dev_pixelpipe_iop_t *piece,
                   const dt_iop_roi_t *roi_out,
                   dt_iop_roi_t *roi_in)

// Transform point coordinates forward
gboolean distort_transform(dt_iop_module_t *self,
                           dt_dev_pixelpipe_iop_t *piece,
                           float *points, size_t points_count)

// Transform point coordinates backward
gboolean distort_backtransform(dt_iop_module_t *self,
                               dt_dev_pixelpipe_iop_t *piece,
                               float *points, size_t points_count)
```

---

## OpenCL Processing (Optional)

If the module can use the GPU, implement `process_cl()`. Wrap in `#ifdef HAVE_OPENCL`:

```c
#ifdef HAVE_OPENCL
int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,         // GPU input buffer
               cl_mem dev_out,        // GPU output buffer
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  dt_iop_mymodule_data_t *d = piece->data;
  dt_iop_mymodule_global_data_t *gd = self->global_data;

  cl_int err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->kernel_process,
                                         width, height,
                                         CLARG(dev_in), CLARG(dev_out),
                                         CLARG(width), CLARG(height),
                                         CLARG(d->scale));
  return err;  // CL_SUCCESS or error code
}
#endif
```

**Return value:** `CL_SUCCESS` on success, any `cl_int` error code on failure. The pipeline falls back to CPU `process()` on failure.

OpenCL kernels are loaded in `init_global()` (see [init_global / cleanup_global](#init_global--cleanup_global---module-level-resources) above) and accessed via `self->global_data`.

---

## Tiling Support

If `IOP_FLAGS_ALLOW_TILING` is set, implement `tiling_callback()` to report memory requirements. The pipeline uses this to split processing into tiles when memory is limited.

The `dt_develop_tiling_t` struct (from `develop/tiling.h`):

| Field | Purpose |
|-------|---------|
| `factor` | Total CPU memory needed as a multiple of input buffer size (e.g. 2.0 = input + one temp buffer) |
| `factor_cl` | Same for GPU memory |
| `maxbuf` | Largest single temporary buffer as a multiple of input size |
| `maxbuf_cl` | Same for GPU |
| `overhead` | Fixed memory overhead in bytes (independent of image size) |
| `overlap` | Pixels of overlap needed between adjacent tiles (for spatial filters like blur) |
| `xalign` | Horizontal alignment of tile origin (1 = none, 2 = Bayer pattern) |
| `yalign` | Vertical alignment of tile origin |

```c
void tiling_callback(dt_iop_module_t *self,
                     dt_dev_pixelpipe_iop_t *piece,
                     const dt_iop_roi_t *roi_in,
                     const dt_iop_roi_t *roi_out,
                     dt_develop_tiling_t *tiling)
{
  tiling->factor = 2.5f;     // need input + 1.5× temp buffers
  tiling->factor_cl = 2.5f;
  tiling->maxbuf = 1.0f;     // largest single buffer = 1× input
  tiling->maxbuf_cl = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 4;       // 4-pixel overlap for a 3×3 kernel
  tiling->xalign = 1;
  tiling->yalign = 1;
}
```

---

---

## History, Callbacks, and the Reset Flag

This section covers three critical mechanisms for module development:
1. When to call `dt_dev_add_history_item()`
2. When to call `dt_iop_gui_update()`
3. The role of `darktable.gui->reset`

### `dt_dev_add_history_item()`

```c
void dt_dev_add_history_item(dt_develop_t *dev,
                             dt_iop_module_t *module,
                             gboolean enable);
```

**Purpose:** Records the current state of `module->params` to the history stack, triggering a pixelpipe reprocess.

**The `enable` parameter:**
- `TRUE`: Also sets `module->enabled = TRUE` and updates the on/off button. Use this when the user's action should turn on the module.
- `FALSE`: Only records params without changing the enabled state. Use this during continuous adjustments (like dragging) where the module is already enabled.

**When to call it:**

| Situation | Call? | Notes |
|-----------|-------|-------|
| Manual callback (custom slider, button) | **Yes** | After modifying `self->params` directly |
| `color_picker_apply()` | **Yes** | After setting params from picked color |
| `gui_changed()` | **No** | Framework already handles this for `_from_params` widgets |
| `gui_update()` | **No** | You're syncing GUI from params, not changing params |
| Mouse drag on graph/area | **Yes** | Call with `FALSE` during drag, `TRUE` on release |

**Example - Manual callback:**
```c
static void my_button_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;  // Always check this first!

  dt_iop_mymodule_params_t *p = self->params;
  p->some_value = calculate_new_value();

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
```

**Example - Continuous adjustment (mouse drag):**
```c
int mouse_moved(dt_iop_module_t *self, float x, float y, ...)
{
  if(darktable.gui->reset) return 0;
  if(!g->dragging) return 0;

  dt_iop_mymodule_params_t *p = self->params;
  p->curve_point = new_value_from_mouse(x, y);

  // FALSE = don't auto-enable, just record (during drag)
  dt_dev_add_history_item(darktable.develop, self, FALSE);
  return 1;
}

int button_released(dt_iop_module_t *self, float x, float y, ...)
{
  if(g->dragging)
  {
    g->dragging = FALSE;
    // TRUE = final commit, enable module if not already
    dt_dev_add_history_item(darktable.develop, self, TRUE);
    return 1;
  }
  return 0;
}
```

### `dt_iop_gui_update()`

```c
void dt_iop_gui_update(dt_iop_module_t *module);
```

**Purpose:** Syncs all widgets from `module->params`. Called by the framework when params change externally.

**When the framework calls it:**
- Switching images in darkroom
- Navigating history (undo/redo)
- Loading presets
- Copying/pasting module settings

**When YOU call it:** Almost never from module code. The only exception is after programmatically changing multiple params that bypass the normal callback system (rare). If you find yourself wanting to call this, you're probably doing something wrong.

**Instead, use `gui_update()` (your callback):** The framework calls your `gui_update()` function, which should sync widgets and then call `gui_changed(self, NULL, NULL)`.

### `darktable.gui->reset`

**Purpose:** A counter that, when non-zero, suppresses callback processing. This prevents infinite loops and unwanted side effects when programmatically updating widgets.

**How it works:**
- Callbacks should check `if(darktable.gui->reset) return;` at the start
- `dt_dev_add_history_item()` does nothing if reset is non-zero
- The framework sets it during `gui_update()` to prevent cascading callbacks

**When to use it:**

```c
// Pattern 1: Check at start of every callback
static void my_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;  // ← Always do this
  // ... rest of callback
}

// Pattern 2: Temporarily suppress callbacks when setting widgets
void some_function(dt_iop_module_t *self)
{
  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  dt_iop_mymodule_params_t *p = self->params;

  // Setting this slider would normally trigger its callback,
  // which would modify params and add history. We don't want that.
  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->some_slider, p->some_value);
  --darktable.gui->reset;

  // Now safe to continue - slider is set but callback was suppressed
}
```

**Common patterns:**

```c
// Pattern A: Callback that updates another widget
static void slider1_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_mymodule_params_t *p = self->params;
  dt_iop_mymodule_gui_data_t *g = self->gui_data;

  // This callback was triggered by user changing slider1.
  // We need to update slider2 based on the new value,
  // but we don't want slider2's callback to fire.
  ++darktable.gui->reset;
  dt_bauhaus_slider_set(g->slider2, compute_from(p->value1));
  --darktable.gui->reset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

// Pattern B: gui_update uses reset internally (via framework)
// You don't need to set reset in gui_update - framework does it.
void gui_update(dt_iop_module_t *self)
{
  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  dt_iop_mymodule_params_t *p = self->params;

  // These won't trigger callbacks - framework set reset
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->toggle), p->enabled);

  gui_changed(self, NULL, NULL);
}
```

### Summary: Data Flow and Callbacks

```
User drags slider (from_params widget)
    ↓
[Framework sets value in self->params]
    ↓
[Framework calls gui_changed(self, widget, previous)]
    ↓
[Framework calls dt_dev_add_history_item internally]
    ↓
[Pixelpipe reprocesses]

User clicks custom button
    ↓
your_callback() called
    ↓
Check: if(darktable.gui->reset) return;
    ↓
Modify self->params directly
    ↓
Call dt_dev_add_history_item(darktable.develop, self, TRUE);
    ↓
[Pixelpipe reprocesses]

Image switched / history navigation
    ↓
[Framework loads new params into self->params]
    ↓
[Framework sets darktable.gui->reset]
    ↓
[Framework calls your gui_update()]
    ↓
You sync widgets, call gui_changed(self, NULL, NULL)
    ↓
[Framework clears darktable.gui->reset]
```

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
