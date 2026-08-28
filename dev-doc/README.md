# Darktable IOP Developer's Guide - Overview

This guide covers building Image Operation (IOP) modules for darktable's darkroom.

## Documentation Files

### Module API
| File | Description |
|------|-------------|
| **[IOP_Module_API.md](IOP_Module_API.md)** | Module API reference: params_t vs data_t, processing, commit_params, lifecycle |
| **[pixelpipe_architecture.md](pixelpipe_architecture.md)** | Pixelpipe data flow, caching, ROI, ordering asymmetry |
| **[introspection.md](introspection.md)** | Introspection system for parameters and GUI |
| **[Shortcuts.md](Shortcuts.md)** | The Action/Shortcut system and `dt_action_def_t` |
| **[Module_Groups.md](Module_Groups.md)** | Module grouping explanation and `default_group()` |
| **[maths.md](maths.md)** | Core Math, transposed matrices, and color science helpers |

### GUI Development
| File | Description |
|------|-------------|
| **[GUI.md](GUI.md)** | GUI architecture: UI construction, events/callbacks, reparenting |
| **[GUI_Threading.md](GUI_Threading.md)** | Sharing `gui_data` between the GTK and pixelpipe threads: locking, GUI updates, callback lifetime |
| **[imageop_gui.md](imageop_gui.md)** | Widget creation functions (`dt_bauhaus_*_from_params`, buttons, sections) |
| **[sliders.md](sliders.md)** | Slider configuration (ranges, formatting, color stops, recipes) |
| **[Notebook_UI.md](Notebook_UI.md)** | Creating tabbed interfaces with `GtkNotebook` |
| **[Quick_Access_Panel.md](Quick_Access_Panel.md)** | Quick Access Panel integration for widgets |
| **[GUI_Recipes.md](GUI_Recipes.md)** | Copy-paste patterns for notebooks, sections, buttons, visibility |

### AI Subsystem
| File | Description |
|------|-------------|
| **[AI.md](AI.md)** | AI subsystem architecture, backend API, and how to add new AI features |
| **[AI_Tasks.md](AI_Tasks.md)** | Reference for each AI task: mask, denoise, upscale |

### Guides
| File | Description |
|------|-------------|
| **[New_Module_Guide.md](New_Module_Guide.md)** | Step-by-step guide to creating a new IOP module |

---

## Quick Reference

### Essential Headers

```c
// GUI / widget creation
#include "develop/imageop.h"          // IOP infrastructure, copy_pixel_nontemporal
#include "develop/imageop_gui.h"      // Widget creation helpers (_from_params)
#include "bauhaus/bauhaus.h"          // Slider/combobox configuration
#include "gui/gtk.h"                  // Notebooks, sections, collapsibles
#include "dtgtk/paint.h"             // Icon paint functions for buttons
#include "gui/color_picker_proxy.h"   // Color picker attachment

// Processing
#include "common/darktable.h"         // DT_OMP_FOR, copy_pixel, dt_alloc_align
#include "common/imagebuf.h"          // dt_iop_alloc_image_buffers, dt_iop_copy_image_roi
#include "develop/tiling.h"           // dt_develop_tiling_t (for tiling_callback)
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
if(dt_pipe_is_full(piece->pipe)) { /* full view only */ }
```

For widget creation, slider configuration, and notebook patterns, see [imageop_gui.md](imageop_gui.md), [GUI.md](GUI.md), [sliders.md](sliders.md), and [GUI_Recipes.md](GUI_Recipes.md).

Before hand-rolling buffer, hash and lock plumbing to show a per-pixel value under the mouse cursor, check `src/develop/preview_data.h` — the framework already owns that case; see [GUI_Threading.md](GUI_Threading.md#the-framework-service-for-per-pixel-readouts).

---

## Module Lifecycle

### Key Functions to Implement

| Function | Purpose |
|----------|---------|
| `gui_init()` | Create and configure all widgets (do NOT set values here) |
| `gui_update()` | Sync widget values from `self->params` (called when params change) |
| `gui_changed()` | Adjust UI based on current state (show/hide widgets, update labels) |
| `gui_cleanup()` | Free any manually allocated resources |
| `change_image()` | Clear GUI state describing the old image; also where pipe-scheduled GUI updates are cancelled ([why](IOP_Module_API.md#change_image---reset-gui-state-for-the-new-image)) |
| `init_pipe()` | Allocate `piece->data` — required if using a custom `data_t` larger than `params_t` |
| `commit_params()` | Transform the `params` argument it is handed into processing-ready `piece->data` for `process()` ([which argument](IOP_Module_API.md#commit_params---transform-parameters-into-processing-data)) |
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
| `gui_data_t` | `self->gui_data` | Widget references and GUI-only state — exists only where the module has a GUI: the darkroom, plus a throw-away instance built at startup |

### Data Flow

```
gui_init()    →  Create widgets, configure ranges/formats           [1]
                      ↓
gui_update()  ←  Called when params change (image switch, history)
                      ↓
gui_changed() ←  Apply UI adjustments (show/hide, sensitivity)      [2]
                      ↓
User interacts with widget
                      ↓
Auto-callback (from_params) or manual callback                      [3]
                      ↓
self->params updated
                      ↓
dt_dev_add_history_item() → commit_params() → process()
         │                        │                │
    records params         transforms params    reads piece->data
    to history stack       into piece->data     (data_t or params_t)
```

1. Do **not** call `gui_update()` from `gui_init()` — params may not be ready yet.
2. If your module implements `gui_changed()`, call `gui_changed(self, NULL, NULL)` at the end of `gui_update()`; the framework does not call it for you there.
3. On the `_from_params` route the framework calls your `gui_changed()` (if implemented) and records the history item. A manual callback does both itself.

See [GUI.md](GUI.md) for the full event flow and callback patterns, and [GUI_Threading.md](GUI_Threading.md) for thread safety.

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
