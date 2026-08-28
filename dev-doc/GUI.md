# GUI Architecture for IOP Modules

This document covers building module UIs, handling events and callbacks, and widget reparenting patterns.

See also:
- [GUI_Threading.md](GUI_Threading.md) — Sharing `gui_data` between the GTK and pixelpipe threads
- [imageop_gui.md](imageop_gui.md) — Widget creation function reference (`_from_params`, buttons, sections)
- [sliders.md](sliders.md) — Slider configuration (ranges, formatting, color stops)
- [Notebook_UI.md](Notebook_UI.md) — Tabbed interfaces with `GtkNotebook`
- [GUI_Recipes.md](GUI_Recipes.md) — Copy-paste patterns for notebooks, sections, buttons, visibility

---

## 1. Constructing the Module UI

### `gui_init()` Overview

`gui_init()` is called once per module instance when entering the darkroom — and once more per module at startup: `_init_module_so()` builds a throw-away instance and runs `dt_iop_gui_init()` on it so that its widgets can register their accelerators, then tears it down again (`src/develop/imageop.c`). That startup instance is built with `dev == NULL`, which matters to any code that takes an arbitrary instance and reaches for `self->dev` — see [GUI_Threading.md](GUI_Threading.md#publishing-gui_data-through-a-proxy).

`gui_init()`'s job is to create and configure all widgets — but **not** to set their values (that happens in `gui_update()`).

```c
void gui_init(dt_iop_module_t *self)
{
  dt_iop_mymodule_gui_data_t *g = IOP_GUI_ALLOC(mymodule);
  // ... create widgets ...
}
```

`IOP_GUI_ALLOC(modulename)` allocates `gui_data_t` via `calloc` and assigns it to `self->gui_data`.

### `self->widget` — Dual Role

The `self->widget` pointer has two purposes:

1. **During `gui_init()`:** Acts as the "current packing target" for `dt_bauhaus_*_from_params()` functions, which implicitly pack into `self->widget`.
2. **After `gui_init()`:** Tells the framework which widget represents the entire module UI for display in the side panel.

For a simple module, set it once at the top:

```c
self->widget = dt_gui_vbox();
g->slider1 = dt_bauhaus_slider_from_params(self, "param1");  // auto-packs into self->widget
g->slider2 = dt_bauhaus_slider_from_params(self, "param2");
```

For tabbed or collapsible UIs, you temporarily redirect `self->widget` to sub-containers during construction, then restore it at the end. See [Section 4: Widget Reparenting](#4-widget-reparenting) below.

### Layout API

darktable provides wrapper functions for GTK4 compatibility. Always use these instead of raw GTK packing functions:

| Function | Replaces | Purpose |
|----------|----------|---------|
| `dt_gui_vbox()` / `dt_gui_hbox()` | `gtk_box_new()` | Create a box with standard spacing |
| `dt_gui_box_add(box, child)` | `gtk_box_pack_start()` | Add a widget to a container |
| `dt_ui_label_new(text)` | `gtk_label_new()` | Label with automatic ellipsization |
| `dt_ui_section_label_new(text)` | — | Visual section header/divider |

**Controlling layout** — set properties on the widget before adding:

```c
// Make a widget expand to fill available space
gtk_widget_set_hexpand(widget, TRUE);
gtk_widget_set_halign(widget, GTK_ALIGN_FILL);
```

- **Expansion** (`hexpand`/`vexpand`): Whether the widget claims extra space. Default is `FALSE`.
- **Alignment** (`halign`/`valign`): How the widget fits within its allocation. Use `GTK_ALIGN_FILL` for inputs, `GTK_ALIGN_CENTER` for checkboxes, `GTK_ALIGN_START`/`GTK_ALIGN_END` for labels.

### Widget Packing Order

`_from_params()` functions auto-pack into `self->widget` in the order you call them. For non-introspection widgets (labels, manual buttons), use `dt_gui_box_add()` explicitly:

```c
self->widget = dt_gui_vbox();

g->slider1 = dt_bauhaus_slider_from_params(self, "param1");   // auto-packed
dt_gui_box_add(self->widget, dt_ui_section_label_new(_("advanced")));  // manual
g->slider2 = dt_bauhaus_slider_from_params(self, "param2");   // auto-packed
```

---

## 2. GUI Events and Callbacks

### The Event Flow

There are three distinct paths through the callback system:

**Path A — `_from_params` widget changed by user:**
```
User drags slider
    ↓
Framework sets value in self->params
    ↓
Framework calls your gui_changed(self, widget, previous), if you have one
    ↓
Framework calls dt_dev_add_history_item() internally
    ↓
commit_params() → process()
```

**Path B — Custom widget changed by user:**
```
User clicks custom button
    ↓
your_callback() fires
    ↓
Check: DT_GUARD_GUI_UPDATE()   (suppresses callbacks during a programmatic sync)
    ↓
Modify self->params directly
    ↓
Call gui_changed(self, NULL, NULL) yourself, if the change
affects dependent UI state (the framework does not, on this route)
    ↓
Call dt_dev_add_history_item(darktable.develop, self, TRUE)
    ↓
commit_params() → process()
```

**Path C — External change (image switch, history, presets, styles):**

Params arrive from outside your widget callbacks, and your widgets are then re-synced from them with `DT_ENTER_GUI_UPDATE()` held across the sync so those callbacks do not fire. Where the guard opens differs by route, so they are shown separately.

Image switch (`src/views/darkroom.c`):
```
DT_ENTER_GUI_UPDATE()
    ↓
Framework calls reload_defaults(), then your change_image() [if implemented]
(the base instance is kept, so gui_data describing the old
 image must be reset there — see IOP_Module_API.md, GUI_Threading.md)
    ↓
Framework loads the image's history params into self->params
    ↓
Framework calls your gui_update()  →  you sync widgets,
                                      gui_changed(self, NULL, NULL) [if implemented]
    ↓
DT_LEAVE_GUI_UPDATE()
```

Params-only history navigation — undo or redo of a parameter change, or moving in the history stack (`dt_dev_pop_history_items()`): the same, without `reload_defaults()` and `change_image()`.

Preset applied directly (`dt_gui_presets_apply_preset()`): the params are copied into `self->params` **before** the guard, which `dt_iop_gui_update()` then opens around your `gui_update()`.

Routes that can change the module list are not just a params load, and a module may be built with a fresh `gui_init()` and `reload_defaults()`, or torn down with `gui_cleanup()`, rather than merely updated:

- **Undo or redo of a module *add or delete*** (`src/libs/history.c`) does both — it re-creates the instance an undone delete removed, and removes the one an undone add created.
- **Pasting history, or applying a style** (`dt_dev_reload_history_items()`) adds the instances the incoming history needs and re-synchronizes the rest; it does not run the instance teardown that undo/redo does.

See [GUI_Threading.md](GUI_Threading.md#the-callback-must-not-outlive-the-module-or-the-image) for what that means for work you have queued.

### `gui_update()` — Sync Widgets from Params

Called by the framework when params change externally (image switch, history navigation, preset load, copy/paste). The framework raises the GUI-update guard (`DT_ENTER_GUI_UPDATE()`) before calling it, so widget callbacks will not fire.

Widgets created with the `_from_params` helpers — sliders, comboboxes and toggles alike — are bound to a parameter field, and the framework syncs all of them for you: `dt_iop_gui_update()` runs `dt_bauhaus_update_from_field()` before it calls you (`src/develop/imageop.c`, `src/bauhaus/bauhaus.c`). You only need to sync widgets that carry no field — a plain `dt_iop_togglebutton_new()` button, or a custom widget of your own. If your module implements `gui_changed()`, end with `gui_changed(self, NULL, NULL)` so the dependent UI state it maintains — visibility, sensitivity, labels — is recomputed; the framework does not call it for you here:

```c
void gui_update(dt_iop_module_t *self)
{
  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  dt_iop_mymodule_params_t *p = self->params;

  // Only unbound widgets need this. dt_iop_togglebutton_new() gives you a real
  // GtkToggleButton, so the GTK setter is right here; a _from_params toggle is
  // synced for you, and is a Bauhaus widget that would take dt_bauhaus_toggle_set()
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->my_plain_button), p->my_bool);

  // Apply all UI state adjustments
  gui_changed(self, NULL, NULL);
}
```

### `gui_changed()` — UI State Adjustments

The single place for all conditional visibility, sensitivity, and dynamic label logic. Called:
- By the framework after a `_from_params` auto-callback (with `widget` = the changed widget, `previous` = old value) — the same route on which the framework also records the history item
- By you at the end of `gui_update()` (with `widget` = NULL)
- By you from a manual widget callback (with `widget` = NULL), when the change affects dependent UI state — a manual callback gets neither of the two things the framework does on the bound-widget route, so it records the history item itself as well

```c
void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  dt_iop_mymodule_params_t *p = self->params;

  // Show/hide based on mode
  if(!w || w == g->method)
    gtk_widget_set_visible(g->advanced_slider, p->mode == MODE_ADVANCED);

  // Disable when irrelevant
  gtk_widget_set_sensitive(g->saturation, p->mode != MODE_MONOCHROME);
}
```

### The GUI-Update Guard (`DT_GUARD_GUI_UPDATE`)

A counter (not a boolean) that suppresses callback processing when non-zero. The framework uses it during `gui_update()`.

**Pattern 1: Check at the start of every manual callback:**
```c
static void my_callback(GtkWidget *w, dt_iop_module_t *self)
{
  DT_GUARD_GUI_UPDATE()  // Always do this

  dt_iop_mymodule_params_t *p = self->params;
  p->value = calculate_new_value();
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
```

**Pattern 2: Suppress callbacks when programmatically updating widgets:**
```c
// Setting slider2 in response to slider1 changing
DT_ENTER_GUI_UPDATE()
dt_bauhaus_slider_set(g->slider2, compute_from(p->value1));
DT_LEAVE_GUI_UPDATE()
```

### `dt_dev_add_history_item()`

Records the current state of `self->params` to the history stack, triggering a pixelpipe reprocess.

```c
void dt_dev_add_history_item(dt_develop_t *dev, dt_iop_module_t *module, gboolean enable);
```

The `enable` parameter:
- `TRUE`: Also sets `module->enabled = TRUE`. Use when the user's action should turn on the module.
- `FALSE`: Only records params. Use during continuous adjustments (drag) where the module is already enabled.

**When to call it:**

| Situation | Call? | Notes |
|-----------|-------|-------|
| Manual callback (custom slider, button) | **Yes** | After modifying `self->params` |
| `color_picker_apply()` | **Yes** | After setting params from picked color |
| Mouse drag on graph/area | **Yes** | `FALSE` during drag, `TRUE` on release |
| `gui_changed()` | **No** | Framework handles this for `_from_params` widgets |
| `gui_update()` | **No** | Syncing GUI from params, not changing params |

### Color Picker Callbacks

Attach a color picker to a slider with `dt_color_picker_new()`:

```c
GtkWidget *slider = dt_bauhaus_slider_from_params(self, "white_point");
g->white_point_picker = dt_color_picker_new(self, DT_COLOR_PICKER_AREA, slider);
```

**Picker lifecycle:** button click → sets `request_color_pick` on module → pipeline processes → pixel data sampled → framework calls `color_picker_apply()`.

Available data:
- `self->picked_color[0..2]` — mean RGB from picked area
- `self->picked_color[3]` — luminance (if available)
- `self->picked_color_min[0..2]`, `self->picked_color_max[0..2]` — range

```c
void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker,
                        dt_dev_pixelpipe_t *pipe)
{
  dt_iop_mymodule_params_t *p = self->params;
  dt_iop_mymodule_gui_data_t *g = self->gui_data;

  if(picker == g->white_point_picker)
    p->white_point = log2f(self->picked_color[3]) + some_offset;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
```

For how a picker attaches to a slider — it wraps the slider, and the wrapper is the widget you store and pack — see [sliders.md](sliders.md#33-integration-with-color-pickers). The picker flags (`DT_COLOR_PICKER_POINT`, `DT_COLOR_PICKER_AREA`, `DT_COLOR_PICKER_DENOISE`, `DT_COLOR_PICKER_IO`) and `dt_color_picker_new_with_cst()`, which picks in a color space of your choosing, are declared in `src/gui/color_picker_proxy.h`.

### Mouse/Drawing Callbacks

For modules with canvas interaction (crop, masks):

- `mouse_moved()`, `button_pressed()`, `button_released()`, `scrolled()` — return 1 if event handled
- `gui_post_expose()` — draw overlays on the center view with Cairo

### Cursor Management

There are three levels of cursor change:

1. Regular cursor changes
2. Temporary cursor changes, which override regular cursor changes
3. Global cursor changes, which override regular or temporary cursor changes

Use `dt_control_change_cursor()` for regular cursor changes to set the mouse cursor shape during an interaction. This function uses **CSS cursor names**.

```c
// Example: set to crosshair during interaction
dt_control_change_cursor("crosshair");
```

Commonly used CSS cursor names in darktable:
- `"default"`: standard arrow
- `"pointer"`: clickable or draggable element
- `"move"`: dragging an object
- `"crosshair"`: precise selection/cropping
- `"wait"`: busy state (replaces legacy `GDK_WATCH`)
- `"not-allowed"`: invalid target or help-mode deselect
- `"help"`: help mode
- `"none"`: hidden cursor
- `"w-resize"`, `"e-resize"`, `"n-resize"`, `"s-resize"`: cardinal resizing
- `"nw-resize"`, `"ne-resize"`, `"se-resize"`, `"sw-resize"`: corner resizing
- `"ew-resize"`, `"ns-resize"`: bidirectional resizing

Refer to `src/control/control.c` for the implementation of fallbacks for backends with incomplete CSS support.

Use `dt_control_set_temp_cursor()` to set the mouse cursor shape temporarily, for example if a particular portion of a widget needs to override the cursor set widget-wide by `dt_control_change_cursor()`. Follow this up with `dt_control_clear_temp_cursor()` to restore the cursor to the shape set by the most recent call to `dt_control_change_cursor()`. It is possible to make successive calls to `dt_control_set_temp_cursor()` to update the temporary cursor before it is eventually cleared.

To make a UI-wide global cursor change, set the cursor (via a regular or temporary change) then call `dt_control_forbid_change_cursor()`. Successive calls to `dt_control_change_cursor()` or `dt_control_set_temp_cursor()` will not modify the cursor. To end this global change, call `dt_control_allow_change_cursor()` and then clear any temporary cursor.

Use `dt_gui_cursor_set_busy()` to set a UI-wide busy cursor. This is meant to be used for modal operations which can only be halted by clicking cancel in the job progress widget. Remove the busy cursor with `dt_gui_cursor_clear_busy()`.

In general, darktable widgets do not set their GDK window cursors. If a widget needs to display a particular cursor, it will catch enter/leave events and make appropriate calls to `dt_control_change_cursor()` and/or `dt_control_set_temp_cursor()`. This allows for the global busy, help, and keyboard mapping cursors to override GDK window cursors.

---

## 3. Thread Safety — Where It Is Covered

`process()` is not a GTK callback and has no guaranteed thread affinity. GTK is not thread-safe. You **cannot** call GTK functions directly from `process()`.

Which callback runs on which thread, when the GUI critical section is required, how to hand an update to the main loop and how to keep it from outliving the module are covered in **[GUI_Threading.md](GUI_Threading.md)**, which also sets out the three directions in which `gui_data` crosses threads.

---

## 4. Widget Reparenting

### What Is Reparenting

GTK widgets have a single parent. Reparenting means removing a widget from one container and adding it to another. In darktable, this is used during `gui_init()` to build tabbed and collapsible UIs, and by the framework for the Quick Access Panel.

### Notebook Page Pattern

The most common reparenting pattern during `gui_init()`: temporarily point `self->widget` at each notebook page so `_from_params()` helpers pack widgets into the correct page.

**WRONG — All widgets end up in `main_vbox`, not in notebook pages:**
```c
void gui_init(dt_iop_module_t *self)
{
  dt_iop_mymodule_gui_data_t *g = IOP_GUI_ALLOC(mymodule);
  GtkWidget *main_vbox = dt_gui_vbox();

  self->widget = main_vbox;  // Set too early!

  static dt_action_def_t notebook_def = { };
  g->notebook = dt_ui_notebook_new(&notebook_def);
  dt_gui_box_add(main_vbox, GTK_WIDGET(g->notebook));

  GtkWidget *page1 = dt_ui_notebook_page(g->notebook, N_("basic"), NULL);
  g->brightness = dt_bauhaus_slider_from_params(self, "brightness");  // Goes into main_vbox!

  GtkWidget *page2 = dt_ui_notebook_page(g->notebook, N_("advanced"), NULL);
  g->gamma = dt_bauhaus_slider_from_params(self, "gamma");  // Also goes into main_vbox!
}
```

**CORRECT — Temporarily redirect `self->widget` for each page:**
```c
void gui_init(dt_iop_module_t *self)
{
  dt_iop_mymodule_gui_data_t *g = IOP_GUI_ALLOC(mymodule);
  GtkWidget *main_vbox = dt_gui_vbox();

  static dt_action_def_t notebook_def = { };
  g->notebook = dt_ui_notebook_new(&notebook_def);
  dt_gui_box_add(main_vbox, GTK_WIDGET(g->notebook));

  // --- Page 1 ---
  GtkWidget *page1 = dt_ui_notebook_page(g->notebook, N_("basic"), NULL);
  self->widget = page1;  // Redirect packing to page1
  g->brightness = dt_bauhaus_slider_from_params(self, "brightness");  // → page1
  g->contrast = dt_bauhaus_slider_from_params(self, "contrast");      // → page1

  // --- Page 2 ---
  GtkWidget *page2 = dt_ui_notebook_page(g->notebook, N_("advanced"), NULL);
  self->widget = page2;  // Redirect packing to page2
  g->gamma = dt_bauhaus_slider_from_params(self, "gamma");            // → page2

  // --- Final ---
  self->widget = main_vbox;  // Set to top-level container at the end
}
```

See [Notebook_UI.md](Notebook_UI.md) for the complete pattern with shortcut registration.

### Collapsible Section Pattern

Same invariant, same technique: `_from_params()` helpers pack into whatever `self->widget` points at, so redirect it to the collapsible container while you build the section's contents, then restore it. For the function's signature, its config key and the helpers that go with it, see [imageop_gui.md](imageop_gui.md#dt_gui_new_collapsible_section).

```c
GtkWidget *main_box = self->widget = dt_gui_vbox();

g->amount = dt_bauhaus_slider_from_params(self, "amount");  // main level

dt_gui_new_collapsible_section(&g->cs, "plugins/darkroom/mymodule/expand_advanced",
    _("advanced"), GTK_BOX(main_box), DT_ACTION(self));

self->widget = GTK_WIDGET(g->cs.container);  // redirect
g->detail = dt_bauhaus_slider_from_params(self, "detail");   // → collapsible
g->quality = dt_bauhaus_combobox_from_params(self, "quality"); // → collapsible

self->widget = main_box;  // restore
```

### QAP Reparenting (Framework-Managed)

When a user adds a module's widget to the Quick Access Panel, the framework (`src/libs/modulegroups.c`) reparents it out of your module UI and back again. That is its business, not yours; what it asks of a module author is that the widget survives the trip:

- It must sit in a `GtkBox` or `GtkGrid` parent — that is what the framework knows how to remove from and restore into.
- It must work in isolation: clear label, good tooltip.
- A complex custom widget that depends on its siblings or its parent will not reparent cleanly.

For the steps the framework takes, and the restrictions that follow from them, see [Quick_Access_Panel.md](Quick_Access_Panel.md).
