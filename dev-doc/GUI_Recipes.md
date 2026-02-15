# GUI Recipes - Common Patterns

Practical code patterns for common darktable IOP GUI tasks.

For slider configuration recipes (percentage, EV, degrees, radians, hue gradients, color pickers, manual sliders), see [sliders.md](sliders.md).

---

## Recipe 1: Two-Tab Notebook UI

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

## Recipe 2: Visual Section Labels

```c
// Add a visual divider/header within a page
dt_gui_box_add(self->widget, dt_ui_section_label_new(_("shadows")));

g->shadow_amount = dt_bauhaus_slider_from_params(self, "shadow_amount");
g->shadow_tonality = dt_bauhaus_slider_from_params(self, "shadow_tonality");

dt_gui_box_add(self->widget, dt_ui_section_label_new(_("highlights")));

g->highlight_amount = dt_bauhaus_slider_from_params(self, "highlight_amount");
```

---

## Recipe 3: Icon Toggle Button for Mask Display

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

## Recipe 4: Shortcut Sections for Organized Controls

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

## Recipe 5: Array Parameter Sliders (RGB)

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

## Recipe 6: Conditional Widget Visibility

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

## Recipe 7: Creating Labels

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

---

## Recipe 8: Collapsible Section

```c
void gui_init(dt_iop_module_t *self)
{
  dt_iop_mymodule_gui_data_t *g = IOP_GUI_ALLOC(mymodule);
  GtkWidget *main_box = self->widget = dt_gui_vbox();

  // Main controls (always visible)
  g->amount = dt_bauhaus_slider_from_params(self, "amount");

  // Collapsible "advanced" section
  dt_gui_new_collapsible_section(&g->advanced_section,
      "plugins/darkroom/mymodule/expand_advanced",
      _("advanced"),
      GTK_BOX(main_box),
      DT_ACTION(self));

  // Pack widgets into the collapsible container
  self->widget = GTK_WIDGET(g->advanced_section.container);
  g->detail = dt_bauhaus_slider_from_params(self, "detail");
  g->quality = dt_bauhaus_combobox_from_params(self, "quality");

  // Restore main container
  self->widget = main_box;
}
```

The section remembers its expand/collapse state across sessions via the config key.
