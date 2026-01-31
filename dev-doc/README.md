# Darktable IOP Developer's Guide - Overview

This guide covers building Image Operation (IOP) modules for darktable's darkroom.

## Documentation Files

### Module API
| File | Description |
|------|-------------|
| **[IOP_Module_API.md](IOP_Module_API.md)** | Complete module API reference (required/optional functions) |

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
#include "develop/imageop.h"          // IOP infrastructure
#include "develop/imageop_gui.h"      // Widget creation helpers
#include "bauhaus/bauhaus.h"          // Slider/combobox configuration
#include "gui/gtk.h"                  // Notebooks, sections, collapsibles
#include "dtgtk/paint.h"              // Icon paint functions for buttons
#include "gui/color_picker_proxy.h"   // Color picker attachment
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
| `color_picker_apply()` | Handle color picker results (if using pickers) |
| `reload_defaults()` | Update defaults for different image types |

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
dt_dev_add_history_item() if needed
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
| Sample module | `src/iop/useless.c` (extensively commented) |

---

## Example Modules to Study

| Module | Features Demonstrated |
|--------|----------------------|
| `useless.c` | **Start here** - Fully documented template with all callbacks explained |
| `exposure.c` | Simple module: sliders with EV formatting, color picker, auto-exposure |
| `bloom.c` | Simple module: percentage sliders, basic processing |
| `sharpen.c` | Simple module: few parameters, straightforward processing |
| `vignette.c` | Toggle buttons, various slider types, coordinate parameters |
| `filmicrgb.c` | Complex tabbed UI, graphs, many options, color science |
| `colorequal.c` | GtkStack with notebook, collapsible sections, advanced patterns |
| `toneequal.c` | Histogram graph, mask overlay toggle, interpolation |
| `retouch.c` | Many icon toggle buttons, drawing tools, mask shapes |
| `ashift.c` | Geometry module with `modify_roi_*`, `distort_*` functions |
