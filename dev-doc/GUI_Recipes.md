# GUI Recipes - Common Patterns

Practical code patterns for common darktable IOP GUI tasks.

---

## Recipe 1: Percentage Slider

Internal range 0.0-1.0 displayed as 0%-100%:

```c
// In params_t:
// float saturation; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5

g->saturation = dt_bauhaus_slider_from_params(self, "saturation");
dt_bauhaus_slider_set_factor(g->saturation, 100.0f);   // 1. Factor first
dt_bauhaus_slider_set_format(g->saturation, " %");     // 2. Then format
dt_bauhaus_slider_set_digits(g->saturation, 1);        // 3. Digits last
```

---

## Recipe 2: EV (Exposure Value) Slider

```c
// In params_t:
// float exposure; // $MIN: -10.0 $MAX: 10.0 $DEFAULT: 0.0

g->exposure = dt_bauhaus_slider_from_params(self, N_("exposure"));
dt_bauhaus_slider_set_format(g->exposure, _(" EV"));
dt_bauhaus_slider_set_soft_range(g->exposure, -4.0, 4.0);  // Reasonable UI range
dt_bauhaus_slider_set_digits(g->exposure, 2);
```

---

## Recipe 3: Angle/Rotation Slider (Degrees)

```c
// In params_t:
// float rotation; // $MIN: -180.0 $MAX: 180.0 $DEFAULT: 0.0

g->rotation = dt_bauhaus_slider_from_params(self, "rotation");
dt_bauhaus_slider_set_format(g->rotation, "°");
dt_bauhaus_slider_set_digits(g->rotation, 1);
```

---

## Recipe 3b: Angle Slider (Radians Internal, Degrees Display)

```c
// In params_t (value stored in radians):
// float angle; // $MIN: -3.14159 $MAX: 3.14159 $DEFAULT: 0.0

g->angle = dt_bauhaus_slider_from_params(self, "angle");
dt_bauhaus_slider_set_factor(g->angle, RAD_2_DEG);  // from common/math.h
dt_bauhaus_slider_set_format(g->angle, "°");
dt_bauhaus_slider_set_digits(g->angle, 1);
// User sees -180° to 180°, internal value is -π to π
```

---

## Recipe 4: Hue Slider with Rainbow Gradient

```c
g->hue = dt_bauhaus_slider_from_params(self, "hue");
dt_bauhaus_slider_set_feedback(g->hue, 0);  // No moving bar
dt_bauhaus_slider_set_factor(g->hue, 360.0f);
dt_bauhaus_slider_set_format(g->hue, "°");

// Rainbow gradient stops
dt_bauhaus_slider_set_stop(g->hue, 0.000f, 1.0f, 0.0f, 0.0f);  // Red
dt_bauhaus_slider_set_stop(g->hue, 0.166f, 1.0f, 1.0f, 0.0f);  // Yellow
dt_bauhaus_slider_set_stop(g->hue, 0.333f, 0.0f, 1.0f, 0.0f);  // Green
dt_bauhaus_slider_set_stop(g->hue, 0.500f, 0.0f, 1.0f, 1.0f);  // Cyan
dt_bauhaus_slider_set_stop(g->hue, 0.666f, 0.0f, 0.0f, 1.0f);  // Blue
dt_bauhaus_slider_set_stop(g->hue, 0.833f, 1.0f, 0.0f, 1.0f);  // Magenta
dt_bauhaus_slider_set_stop(g->hue, 1.000f, 1.0f, 0.0f, 0.0f);  // Red (wrap)
```

---

## Recipe 5: Slider with Color Picker

```c
// Create slider
GtkWidget *slider = dt_bauhaus_slider_from_params(self, "white_point");
dt_bauhaus_slider_set_format(slider, " EV");
dt_bauhaus_slider_set_soft_range(slider, 1.0f, 10.0f);

// Wrap with color picker - store the wrapper, not the slider
g->white_point_picker = dt_color_picker_new(
    self,
    DT_COLOR_PICKER_AREA,
    slider
);

// Set tooltip on the combined widget
gtk_widget_set_tooltip_text(g->white_point_picker, _("white point exposure"));
```

Then in `color_picker_apply()`:
```c
void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker, dt_dev_pixelpipe_t *pipe)
{
  if(picker == g->white_point_picker)
  {
    // self->picked_color[0..2] = RGB, self->picked_color[3] = luminance
    p->white_point = log2f(self->picked_color[3]) + some_offset;
  }
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
```

---

## Recipe 6: Manual Slider (Not Linked to Params)

```c
// Create without introspection
g->extra = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0, 0.0, 2);
dt_bauhaus_widget_set_label(g->extra, NULL, N_("extra adjustment"));
dt_gui_box_add(self->widget, g->extra);

// Manual callback required
g_signal_connect(G_OBJECT(g->extra), "value-changed",
                 G_CALLBACK(extra_callback), self);
```

Callback:
```c
static void extra_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  float value = dt_bauhaus_slider_get(widget);
  // Do something with value...

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
```

---

## Recipe 7: Two-Tab Notebook UI

```c
void gui_init(dt_iop_module_t *self)
{
  dt_iop_mymodule_gui_data_t *g = IOP_GUI_ALLOC(mymodule);

  // Main container
  GtkWidget *main_box = dt_gui_vbox();

  // Create notebook
  static dt_action_def_t notebook_def = { };
  g->notebook = dt_ui_notebook_new(&notebook_def);
  dt_gui_box_add(main_box, GTK_WIDGET(g->notebook));

  // --- Page 1: Basic ---
  GtkWidget *page_basic = dt_ui_notebook_page(g->notebook, N_("basic"), _("Basic controls"));
  self->widget = page_basic;  // Redirect packing

  g->brightness = dt_bauhaus_slider_from_params(self, "brightness");
  g->contrast = dt_bauhaus_slider_from_params(self, "contrast");

  // --- Page 2: Advanced ---
  GtkWidget *page_adv = dt_ui_notebook_page(g->notebook, N_("advanced"), _("Advanced controls"));
  self->widget = page_adv;

  g->gamma = dt_bauhaus_slider_from_params(self, "gamma");
  g->method = dt_bauhaus_combobox_from_params(self, "method");

  // --- Final ---
  self->widget = main_box;  // Set final top-level widget

  // Register notebook for shortcuts
  notebook_def.name = N_("page");
  dt_action_define_iop(self, NULL, N_("page"), GTK_WIDGET(g->notebook), &notebook_def);
}
```

---

## Recipe 8: Visual Section Labels

```c
// Add a visual divider/header within a page
dt_gui_box_add(self->widget, dt_ui_section_label_new(_("shadows")));

g->shadow_amount = dt_bauhaus_slider_from_params(self, "shadow_amount");
g->shadow_tonality = dt_bauhaus_slider_from_params(self, "shadow_tonality");

dt_gui_box_add(self->widget, dt_ui_section_label_new(_("highlights")));

g->highlight_amount = dt_bauhaus_slider_from_params(self, "highlight_amount");
```

---

## Recipe 9: Icon Toggle Button for Mask Display

```c
g->show_mask = dt_iop_togglebutton_new(
    self,
    N_("display"),                      // Shortcut section
    N_("show processing mask"),         // Action label
    NULL,                               // No Ctrl+click action
    G_CALLBACK(show_mask_callback),     // Toggle callback
    FALSE,                              // Not a local shortcut
    0, 0,                               // No accelerator
    dtgtk_cairo_paint_showmask,         // Mask icon
    self->widget                        // Pack into module
);
```

Callback:
```c
static void show_mask_callback(GtkWidget *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  g->mask_display = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));

  dt_iop_refresh_center(self);  // Redraw center view
}
```

---

## Recipe 10: Shortcut Sections for Organized Controls

```c
// Controls will be registered as "module/chroma/global", "module/chroma/shadows", etc.
dt_iop_module_t *sect_chroma = DT_IOP_SECTION_FOR_PARAMS(self, N_("chroma"));

g->chroma_global = dt_bauhaus_slider_from_params(sect_chroma, "chroma_global");
g->chroma_shadows = dt_bauhaus_slider_from_params(sect_chroma, "chroma_shadows");
g->chroma_highlights = dt_bauhaus_slider_from_params(sect_chroma, "chroma_highlights");

// Different section
dt_iop_module_t *sect_sat = DT_IOP_SECTION_FOR_PARAMS(self, N_("saturation"));

g->saturation_global = dt_bauhaus_slider_from_params(sect_sat, "saturation_global");
```

---

## Recipe 11: Array Parameter Sliders (RGB)

```c
// In params_t:
// float color[3]; // $MIN: 0.0 $MAX: 1.0 $DEFAULT: 0.5

g->color_r = dt_bauhaus_slider_from_params(self, "color[0]");
dt_bauhaus_widget_set_label(g->color_r, NULL, N_("red"));

g->color_g = dt_bauhaus_slider_from_params(self, "color[1]");
dt_bauhaus_widget_set_label(g->color_g, NULL, N_("green"));

g->color_b = dt_bauhaus_slider_from_params(self, "color[2]");
dt_bauhaus_widget_set_label(g->color_b, NULL, N_("blue"));
```

---

## Recipe 12: Conditional Widget Visibility

Put all UI state logic (visibility, sensitivity, dynamic labels) in `gui_changed()`:

```c
void gui_changed(dt_iop_module_t *self, GtkWidget *widget, void *previous)
{
  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  dt_iop_mymodule_params_t *p = self->params;

  // Show/hide advanced slider based on mode selection
  gtk_widget_set_visible(g->advanced_slider, p->mode == MODE_ADVANCED);

  // Disable saturation when in monochrome mode
  gtk_widget_set_sensitive(g->saturation, p->mode != MODE_MONOCHROME);
}
```

**Always** call `gui_changed()` at the end of `gui_update()`:
```c
void gui_update(dt_iop_module_t *self)
{
  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  dt_iop_mymodule_params_t *p = self->params;

  // Update toggle buttons (sliders/comboboxes auto-sync)
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->my_toggle), p->my_bool);

  // Apply all UI state adjustments
  gui_changed(self, NULL, NULL);
}
```

This pattern ensures all code paths that change params go through the same UI adjustment phase.

---

## Recipe 13: Creating Labels

**Always** use `dt_ui_label_new()` instead of `gtk_label_new()` to ensure proper text ellipsization:

```c
// WRONG - long text may stretch the panel width
GtkWidget *label = gtk_label_new(_("Description that might be very long"));

// CORRECT - text will be ellipsized if too long
GtkWidget *label = dt_ui_label_new(_("Description that might be very long"));
dt_gui_box_add(self->widget, label);
```

For section headers/dividers, use `dt_ui_section_label_new()` instead:
```c
dt_gui_box_add(self->widget, dt_ui_section_label_new(_("Advanced Options")));
```
