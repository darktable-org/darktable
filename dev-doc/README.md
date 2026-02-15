# Darktable IOP Developer's Guide - Overview

This guide covers building Image Operation (IOP) modules for darktable's darkroom.

## Documentation Files

### Module API
| File | Description |
|------|-------------|
| **[IOP_Module_API.md](IOP_Module_API.md)** | Complete module API reference (required/optional functions) |
| **[pixelpipe_architecture.md](pixelpipe_architecture.md)** | Pixelpipe architecture and data flow |
| **[introspection.md](introspection.md)** | Introspection system for parameters and GUI |
### GUI Development
| File | Description |
|------|-------------|
| **[imageop_gui.md](imageop_gui.md)** | Widget creation functions (`dt_bauhaus_*_from_params`, buttons) |
| **[sliders.md](sliders.md)** | Detailed slider configuration (ranges, formatting, color stops) |
| **[Notebook_UI.md](Notebook_UI.md)** | Creating tabbed interfaces with `GtkNotebook` |
| **[Containers and packing.md](Containers%20and%20packing.md)** | GTK container basics and widget packing |
| **[self-widget.md](self-widget.md)** | Understanding `self->widget` and temporary reassignment |
| **[GUI_Recipes.md](GUI_Recipes.md)** | Copy-paste patterns for common GUI tasks |

---

## Quick Reference

### Essential Headers

```c
// GUI / widget creation
#include "develop/imageop.h"          // IOP infrastructure, copy_pixel_nontemporal
#include "develop/imageop_gui.h"      // Widget creation helpers (_from_params)
#include "bauhaus/bauhaus.h"          // Slider/combobox configuration
#include "gui/gtk.h"                  // Notebooks, sections, collapsibles
#include "dtgtk/paint.h"              // Icon paint functions for buttons
#include "gui/color_picker_proxy.h"   // Color picker attachment

// Processing
#include "common/darktable.h"         // DT_OMP_FOR, copy_pixel, dt_alloc_align
#include "common/imagebuf.h"          // dt_iop_alloc_image_buffers, dt_iop_copy_image_roi
#include "develop/tiling.h"           // dt_develop_tiling_t (for tiling_callback)
```

### Widget Creation Cheat Sheet

```c
// Slider linked to params_t field
GtkWidget *slider = dt_bauhaus_slider_from_params(self, "field_name");

// Combobox linked to enum field
GtkWidget *combo = dt_bauhaus_combobox_from_params(self, "enum_field");

// Toggle button linked to gboolean field
GtkWidget *toggle = dt_bauhaus_toggle_from_params(self, "bool_field");

// Icon toggle button (manual callback)
GtkWidget *btn = dt_iop_togglebutton_new(self, section, label, ctrl_label,
                                         callback, local, key, mods, paint, box);

// Regular icon button
GtkWidget *btn = dt_iop_button_new(self, label, callback, local, key, mods,
                                   paint, paintflags, box);
```

### Slider Configuration

```c
dt_bauhaus_slider_set_soft_range(slider, min, max);  // Default visible range
dt_bauhaus_slider_set_hard_min(slider, val);         // Absolute minimum
dt_bauhaus_slider_set_hard_max(slider, val);         // Absolute maximum
dt_bauhaus_slider_set_digits(slider, 2);             // Decimal places
dt_bauhaus_slider_set_format(slider, " EV");         // Unit suffix
dt_bauhaus_slider_set_factor(slider, 100.0f);        // Display multiplier
dt_bauhaus_slider_set_offset(slider, 0.0f);          // Display offset
dt_bauhaus_slider_set_step(slider, 0.1f);            // Scroll/key step
```

### Notebooks (Tabbed UI)

```c
static dt_action_def_t notebook_def = { };
g->notebook = dt_ui_notebook_new(&notebook_def);

// Each page returns a container to pack widgets into
GtkWidget *page1 = dt_ui_notebook_page(g->notebook, N_("basic"), _("Basic settings"));
self->widget = page1;  // Temporarily redirect widget packing
dt_bauhaus_slider_from_params(self, "param1");
// ... add more widgets to page1 ...

GtkWidget *page2 = dt_ui_notebook_page(g->notebook, N_("advanced"), _("Advanced settings"));
self->widget = page2;
// ... add widgets to page2 ...

self->widget = main_container;  // Restore main container
```

### Processing Cheat Sheet

```c
// Validate input channels (copies input→output and sets trouble message on mismatch)
if(!dt_iop_have_required_input_format(4, self, piece->colors, ivoid, ovoid, roi_in, roi_out))
  return;

// Parallel pixel loop with safe defaults
DT_OMP_FOR()
for(int j = 0; j < roi_out->height; j++)
{
  const float *in = ((float *)ivoid) + (size_t)4 * roi_in->width * j;
  float *out = ((float *)ovoid) + (size_t)4 * roi_out->width * j;
  for(int i = 0; i < roi_out->width; i++)
  {
    for_each_channel(c, aligned(in, out : 16))
      out[c] = in[c] * factor;
    in += 4; out += 4;
  }
}

// Allocate a temporary buffer (auto trouble message on failure)
float *tmp = NULL;
if(!dt_iop_alloc_image_buffers(self, roi_in, roi_out,
                                4 | DT_IMGSZ_OUTPUT, &tmp, NULL))
  return;
// ... use tmp ...
dt_free_align(tmp);

// Scale spatial parameters for current zoom level
const float sigma = user_radius * roi_out->scale / piece->iscale;

// Report trouble to the user (icon on module header)
dt_iop_set_module_trouble_message(self, _("warning text"), _("tooltip"), NULL);

// Check pipe type
if(piece->pipe->type & DT_DEV_PIXELPIPE_FULL) { /* full view only */ }
```

### Common Paint Functions

From `dtgtk/paint.h`:
- `dtgtk_cairo_paint_showmask` - Mask visibility toggle
- `dtgtk_cairo_paint_eye` / `dtgtk_cairo_paint_eye_toggle` - Eye icon
- `dtgtk_cairo_paint_colorpicker` - Color picker pipette
- `dtgtk_cairo_paint_reset` - Reset/refresh icon
- `dtgtk_cairo_paint_masks_brush` - Brush tool
- `dtgtk_cairo_paint_masks_circle` - Circle mask
- `dtgtk_cairo_paint_masks_ellipse` - Ellipse mask
- `dtgtk_cairo_paint_flip` - Flip horizontal/vertical

---

## Module Lifecycle

### Key Functions to Implement

| Function | Purpose |
|----------|---------|
| `gui_init()` | Create and configure all widgets (do NOT set values here) |
| `gui_update()` | Sync widget values from `self->params` (called when params change) |
| `gui_changed()` | Adjust UI based on current state (show/hide widgets, update labels) |
| `gui_cleanup()` | Free any manually allocated resources |
| `init_pipe()` | Allocate `piece->data` — required if using a custom `data_t` larger than `params_t` |
| `commit_params()` | Transform `self->params` (from database/UI) into processing-ready `piece->data` for `process()` |
| `cleanup_pipe()` | Free `piece->data` and any sub-allocations |
| `color_picker_apply()` | Handle color picker results (if using pickers) |
| `reload_defaults()` | Update defaults for different image types |
| `init_global()` | One-time setup per module type (OpenCL kernels, shared LUTs) |
| `cleanup_global()` | Free resources from `init_global()` |
| `process_cl()` | GPU (OpenCL) processing — optional, falls back to `process()` |
| `tiling_callback()` | Report memory requirements for tiled processing |

### Key Data Structures

| Struct | Stored in | Purpose |
|--------|-----------|---------|
| `params_t` | `self->params`, database | User-facing parameters — controlled by UI widgets, serialized to database |
| `data_t` (optional) | `piece->data` | Processing-optimized version of params — precomputed LUTs, transformed values, runtime state. Built by `commit_params()`, consumed by `process()`. If not defined, `piece->data` is a plain copy of `params_t`. |
| `gui_data_t` | `self->gui_data` | Widget references and GUI-only state — only exists in darkroom mode |

### Data Flow

```
gui_init()    →  Create widgets, configure ranges/formats
                 (do NOT call gui_update here - params may not be ready)
                      ↓
gui_update()  ←  Called when params change (image switch, history)
                      ↓
gui_changed() ←  Always call gui_changed(self, NULL, NULL) at end of gui_update
                 to apply any UI adjustments (show/hide, sensitivity, etc.)
                      ↓
User interacts with widget
                      ↓
Auto-callback (from_params) or manual callback
                      ↓
self->params updated → gui_changed() called automatically
                      ↓
dt_dev_add_history_item() → commit_params() → process()
         │                        │                │
    records params         transforms params    reads piece->data
    to history stack       into piece->data     (data_t or params_t)
```

### Recommended Pattern

Always call `gui_changed()` at the end of `gui_update()`:
```c
void gui_update(dt_iop_module_t *self)
{
  // ... sync widgets from params ...

  // Apply UI state adjustments (visibility, sensitivity, etc.)
  gui_changed(self, NULL, NULL);
}
```
This ensures all code paths that change underlying data go through the same UI adjustment phase, making the module easier to test and maintain.

---

## File Locations

| What | Where |
|------|-------|
| IOP modules | `src/iop/*.c` |
| GUI helpers | `src/develop/imageop_gui.c`, `imageop_gui.h` |
| Bauhaus widgets | `src/bauhaus/bauhaus.c`, `bauhaus.h` |
| GTK helpers | `src/gui/gtk.c`, `gtk.h` |
| Paint icons | `src/dtgtk/paint.c`, `paint.h` |
| Color picker | `src/gui/color_picker_proxy.c`, `.h` |
| Image buffer helpers | `src/common/imagebuf.c`, `imagebuf.h` |
| Pixel types/macros | `src/common/dttypes.h` (`for_each_channel`), `darktable.h` (`DT_OMP_FOR`) |
| Tiling support | `src/develop/tiling.c`, `tiling.h` |
| Sample module | `src/iop/useless.c` (extensively commented) |

---

## Example Modules to Study

| Module | Features Demonstrated |
|--------|----------------------|
| `useless.c` | **Start here** - Fully documented template; uses `params_t` directly as `piece->data` |
| `exposure.c` | Custom `data_t` (embeds `params_t` + precomputed fields), color picker, deflicker mode |
| `bloom.c` | Simple module: percentage sliders, basic processing |
| `sharpen.c` | Simple module: few parameters, straightforward processing |
| `vignette.c` | Toggle buttons, various slider types, coordinate parameters |
| `filmicrgb.c` | Complex `data_t` with spline LUT, tabbed UI, graphs, color science |
| `colorbalancergb.c` | Complex `data_t` with gamut LUT, color space transforms, many precomputations |
| `colorequal.c` | GtkStack with notebook, collapsible sections, advanced patterns |
| `toneequal.c` | `data_t` with interpolation matrix solving and precomputed correction LUT |
| `sigmoid.c` | `data_t` derived entirely from mathematical transformations of user params |
| `retouch.c` | Many icon toggle buttons, drawing tools, mask shapes |
| `ashift.c` | Geometry module with `modify_roi_*`, `distort_*` functions |
