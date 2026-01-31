# imageop_gui.c/h - GUI Helper Functions for IOP Modules

This document covers the helper functions in `src/develop/imageop_gui.c` and `imageop_gui.h` that simplify creating GUI widgets for darktable's image operation (IOP) modules.

## Overview

These functions work with darktable's **introspection system** to automatically:
- Configure widget ranges, defaults, and labels from struct definitions
- Set up callbacks that sync widget values to module parameters
- Register widgets with the shortcut/action system

---

## Primary Widget Creation Functions

### `dt_bauhaus_slider_from_params()`

```c
GtkWidget *dt_bauhaus_slider_from_params(dt_iop_module_t *self, const char *param);
```

Creates a slider widget automatically configured from the module's parameter struct definition.

**Parameters:**
- `self`: The module instance
- `param`: Name of the field in `params_t` struct (or use `N_("field")` to mark for translation)

**What it does:**
1. Reads `$MIN`, `$MAX`, `$DEFAULT` comments from the struct definition
2. Sets up the slider range and default value
3. Uses `$DESCRIPTION` as the widget label
4. Creates an automatic callback that syncs slider → `self->params->field`
5. Packs the widget into `self->widget` (the current container)
6. Registers with the action/shortcut system

**Array Indexing:**
For array parameters, use bracket notation:
```c
// For: float Dmin[3]; in params_t
g->Dmin_R = dt_bauhaus_slider_from_params(self, "Dmin[0]");  // Red
g->Dmin_G = dt_bauhaus_slider_from_params(self, "Dmin[1]");  // Green
g->Dmin_B = dt_bauhaus_slider_from_params(self, "Dmin[2]");  // Blue
```

**Example (from exposure.c):**
```c
g->exposure = dt_bauhaus_slider_from_params(self, N_("exposure"));
dt_bauhaus_slider_set_digits(g->exposure, 3);
dt_bauhaus_slider_set_format(g->exposure, _(" EV"));
dt_bauhaus_slider_set_soft_range(g->exposure, -3.0, 4.0);
```

---

### `dt_bauhaus_combobox_from_params()`

```c
GtkWidget *dt_bauhaus_combobox_from_params(dt_iop_module_t *self, const char *param);
```

Creates a combobox automatically populated from an enum definition.

**Parameters:**
- `self`: The module instance
- `param`: Name of the enum field in `params_t`

**What it does:**
1. Reads enum values and their `$DESCRIPTION` comments
2. Populates the combobox with all enum entries
3. Sets up callback to store the **enum value** (not index) in `self->params`
4. Packs into `self->widget` and registers with action system

**Example (from filmicrgb.c):**
```c
// In params_t:
// typedef enum { PRESERVE_NONE, PRESERVE_LUMINANCE, ... } dt_preserve_color_t;
// dt_preserve_color_t preserve_color; // $DESCRIPTION: "preserve chrominance"

g->preserve_color = dt_bauhaus_combobox_from_params(self, "preserve_color");
```

---

### `dt_bauhaus_toggle_from_params()`

```c
GtkWidget *dt_bauhaus_toggle_from_params(dt_iop_module_t *self, const char *param);
```

Creates a toggle button (checkbox) linked to a boolean parameter.

**Parameters:**
- `self`: The module instance
- `param`: Name of the `gboolean` field in `params_t`

**Returns:** A `GtkToggleButton` widget.

**Important:** Unlike sliders/comboboxes, toggle buttons require manual update in `gui_update()`:
```c
void gui_update(dt_iop_module_t *self)
{
  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  dt_iop_mymodule_params_t *p = self->params;

  // Sliders/comboboxes update automatically, but toggles need this:
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->my_toggle), p->my_bool);
}
```

**Example (from vignette.c):**
```c
g->autoratio = dt_bauhaus_toggle_from_params(self, "autoratio");
```

---

## Button Creation Functions

### `dt_iop_togglebutton_new()`

```c
GtkWidget *dt_iop_togglebutton_new(
    dt_iop_module_t *self,
    const char *section,        // Section name for shortcuts (or NULL)
    const gchar *label,         // Label for action system (N_("label"))
    const gchar *ctrl_label,    // Ctrl+click action label (or NULL)
    GCallback callback,         // Function called on toggle
    gboolean local,             // TRUE = local shortcut, FALSE = global
    guint accel_key,            // Accelerator key (0 for none)
    GdkModifierType mods,       // Modifier keys
    DTGTKCairoPaintIconFunc paint,  // Icon paint function
    GtkWidget *box              // Container to pack into (or NULL)
);
```

Creates a toggle button with an icon, typically used for mode switches or mask display toggles.

**Icon Paint Functions** (from `dtgtk/paint.h`):
- `dtgtk_cairo_paint_showmask` - Mask display icon
- `dtgtk_cairo_paint_eye` / `dtgtk_cairo_paint_eye_toggle` - Eye icon
- `dtgtk_cairo_paint_colorpicker` - Color picker pipette
- `dtgtk_cairo_paint_masks_brush` - Brush tool
- `dtgtk_cairo_paint_masks_circle` - Circle mask
- `dtgtk_cairo_paint_masks_ellipse` - Ellipse mask
- `dtgtk_cairo_paint_masks_path` - Path/bezier mask

**Example (from toneequal.c):**
```c
g->show_luminance_mask = dt_iop_togglebutton_new(
    self,                           // module
    N_("display"),                  // section for shortcuts
    N_("show luminance mask"),      // action label
    NULL,                           // no ctrl+click action
    G_CALLBACK(show_luminance_mask_callback),
    FALSE,                          // global shortcut
    0, 0,                           // no accelerator
    dtgtk_cairo_paint_showmask,     // mask icon
    self->widget                    // pack into module widget
);
```

---

### `dt_iop_button_new()`

```c
GtkWidget *dt_iop_button_new(
    dt_iop_module_t *self,
    const gchar *label,         // Label for action system
    GCallback callback,         // Function called on click
    gboolean local,             // TRUE = local shortcut
    guint accel_key,            // Accelerator key
    GdkModifierType mods,       // Modifier keys
    DTGTKCairoPaintIconFunc paint,  // Icon paint function
    gint paintflags,            // Paint flags (CPF_DIRECTION_*, etc.)
    GtkWidget *box              // Container to pack into
);
```

Creates a regular (non-toggle) button with an icon.

**Example (from flip.c):**
```c
dt_iop_button_new(self, N_("rotate 90 degrees CCW"),
                  G_CALLBACK(rotate_ccw), FALSE, 0, 0,
                  dtgtk_cairo_paint_refresh,
                  CPF_DIRECTION_UP,  // Icon direction flag
                  self->widget);
```

---

## Section Organization

### `DT_IOP_SECTION_FOR_PARAMS` Macro

```c
dt_iop_module_t *DT_IOP_SECTION_FOR_PARAMS(dt_iop_module_t *self, const char *name);
// Or with explicit container:
dt_iop_module_t *DT_IOP_SECTION_FOR_PARAMS(dt_iop_module_t *self, const char *name, GtkWidget *box);
```

Creates a logical section for organizing shortcuts hierarchically. Widgets created with this "section module" appear under `module/section/widget` in the shortcut system.

**Important:** This does NOT create visual sections - use `dt_ui_section_label_new()` for that.

**Example (from colorbalancergb.c):**
```c
// Create section for "chroma" controls
dt_iop_module_t *sect = DT_IOP_SECTION_FOR_PARAMS(self, N_("chroma"));

// These sliders will be registered as "colorbalancergb/chroma/global chroma"
dt_bauhaus_slider_from_params(sect, "chroma_global");
dt_bauhaus_slider_from_params(sect, "chroma_highlights");
dt_bauhaus_slider_from_params(sect, "chroma_midtones");
dt_bauhaus_slider_from_params(sect, "chroma_shadows");
```

---

## Helper Function

### `dt_mask_scroll_increases()`

```c
gboolean dt_mask_scroll_increases(int up);
```

Returns `TRUE` if scrolling "up" should increase a value, respecting user preferences. Use this when implementing custom scroll behavior.

---

## Interpolation Combobox

### `dt_bauhaus_combobox_new_interpolation()`

```c
GtkWidget *dt_bauhaus_combobox_new_interpolation(dt_iop_module_t *self);
```

Creates a standard combobox pre-populated with interpolation methods (bilinear, bicubic, lanczos, etc.). Used by modules that need to offer interpolation choices.

---

## Post-Creation Slider Configuration

After creating a slider with `dt_bauhaus_slider_from_params()`, you typically configure it further using functions from `bauhaus/bauhaus.h`:

### Range Limits

```c
// Soft limits: Default visible range (user can exceed with Ctrl+Shift)
dt_bauhaus_slider_set_soft_range(widget, soft_min, soft_max);
dt_bauhaus_slider_set_soft_min(widget, val);
dt_bauhaus_slider_set_soft_max(widget, val);

// Hard limits: Absolute boundaries (cannot be exceeded)
dt_bauhaus_slider_set_hard_min(widget, val);
dt_bauhaus_slider_set_hard_max(widget, val);
```

### Display Formatting

```c
// Number of decimal places
dt_bauhaus_slider_set_digits(widget, 2);

// Unit suffix (e.g., " EV", " %", "°")
dt_bauhaus_slider_set_format(widget, " EV");

// Transform internal value for display: displayed = (internal * factor) + offset
dt_bauhaus_slider_set_factor(widget, 100.0f);  // For percentages
dt_bauhaus_slider_set_offset(widget, 0.0f);
```

### Visual Gradient Stops

```c
// Add color stops for a gradient background (position 0.0-1.0, RGB 0.0-1.0)
dt_bauhaus_slider_set_stop(widget, 0.0f, 1.0f, 0.0f, 0.0f);  // Red at start
dt_bauhaus_slider_set_stop(widget, 0.5f, 0.0f, 1.0f, 0.0f);  // Green at middle
dt_bauhaus_slider_set_stop(widget, 1.0f, 0.0f, 0.0f, 1.0f);  // Blue at end

// Or use a hue gradient (for hue sliders)
// See colorize.c for a complete example
```

### Other Options

```c
// Default value (for reset)
dt_bauhaus_slider_set_default(widget, def);

// Step size for keyboard/scroll
dt_bauhaus_slider_set_step(widget, 0.1f);

// Disable feedback (the moving bar) - useful for special displays
dt_bauhaus_slider_set_feedback(widget, 0);
```

---

## Attaching Color Pickers

To attach a color picker to a slider:

```c
#include "gui/color_picker_proxy.h"

// Create slider first
GtkWidget *slider = dt_bauhaus_slider_from_params(self, "exposure");

// Wrap with color picker - this returns the combined widget
g->exposure_picker = dt_color_picker_new(
    self,
    DT_COLOR_PICKER_AREA | DT_COLOR_PICKER_DENOISE,  // Picker flags
    slider
);
```

Then implement `color_picker_apply()` in your module:
```c
void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_t *pipe)
{
  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  dt_iop_mymodule_params_t *p = self->params;

  if(picker == g->exposure_picker)
  {
    // self->picked_color contains [R, G, B] from the picked area
    p->exposure = /* calculate from picked color */;
  }

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
```

---

## Complete gui_init() Example

```c
void gui_init(dt_iop_module_t *self)
{
  dt_iop_mymodule_gui_data_t *g = IOP_GUI_ALLOC(mymodule);

  // Create main container (optional if first widget uses _from_params)
  self->widget = dt_gui_vbox();

  // Simple slider
  g->exposure = dt_bauhaus_slider_from_params(self, N_("exposure"));
  dt_bauhaus_slider_set_format(g->exposure, " EV");
  dt_bauhaus_slider_set_soft_range(g->exposure, -4.0, 4.0);

  // Combobox from enum
  g->method = dt_bauhaus_combobox_from_params(self, "method");

  // Toggle button
  g->enabled = dt_bauhaus_toggle_from_params(self, "enabled");

  // Visual section label
  dt_gui_box_add(self->widget, dt_ui_section_label_new(_("advanced")));

  // More sliders...
  g->gamma = dt_bauhaus_slider_from_params(self, "gamma");
  dt_bauhaus_slider_set_digits(g->gamma, 3);

  // Icon button
  g->reset_btn = dt_iop_button_new(self, N_("reset"),
                                   G_CALLBACK(reset_callback), FALSE, 0, 0,
                                   dtgtk_cairo_paint_reset, 0, self->widget);
}
```

---

## Key Points to Remember

1. **`_from_params` functions auto-pack** widgets into `self->widget`. Set `self->widget` to the correct container before calling them.

2. **Toggles need manual gui_update()** - unlike sliders/comboboxes which auto-sync.

3. **Slider format order matters** - Set `factor` before `format` before `digits` to avoid rounding issues (see sliders.md).

4. **Sections are for shortcuts**, not visual organization. Use `dt_ui_section_label_new()` for visual headers.

5. **Color pickers wrap sliders** - The picker becomes the widget to store/pack, not the original slider.

6. **Don't set widget values in gui_init()** - Only create and configure widgets. Setting values belongs in `gui_update()`, which is called when params are available.

7. **Always call gui_changed() from gui_update()** - End `gui_update()` with `gui_changed(self, NULL, NULL)` to ensure all UI state adjustments (visibility, sensitivity) are applied consistently.

8. **Use dt_ui_label_new() for labels** - Never use `gtk_label_new()` directly. The wrapper ensures long text is ellipsized and doesn't stretch the panel width.
