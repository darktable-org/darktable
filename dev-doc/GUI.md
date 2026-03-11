# GUI Architecture for IOP Modules

This document covers building module UIs, handling events and callbacks, updating the GUI from worker threads, and widget reparenting patterns.

See also:
- [imageop_gui.md](imageop_gui.md) — Widget creation function reference (`_from_params`, buttons, sections)
- [sliders.md](sliders.md) — Slider configuration (ranges, formatting, color stops)
- [Notebook_UI.md](Notebook_UI.md) — Tabbed interfaces with `GtkNotebook`
- [GUI_Recipes.md](GUI_Recipes.md) — Copy-paste patterns for notebooks, sections, buttons, visibility

---

## 1. Constructing the Module UI

### `gui_init()` Overview

`gui_init()` is called once per module instance when entering the darkroom. Its job is to create and configure all widgets — but **not** to set their values (that happens in `gui_update()`).

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
Framework calls gui_changed(self, widget, previous)
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
Check: if(darktable.gui->reset) return;
    ↓
Modify self->params directly
    ↓
Call dt_dev_add_history_item(darktable.develop, self, TRUE)
    ↓
commit_params() → process()
```

**Path C — External change (image switch, undo, preset):**
```
Framework loads new params into self->params
    ↓
Framework sets darktable.gui->reset
    ↓
Framework calls your gui_update()
    ↓
You sync widgets, call gui_changed(self, NULL, NULL)
    ↓
Framework clears darktable.gui->reset
```

### `gui_update()` — Sync Widgets from Params

Called by the framework when params change externally (image switch, history navigation, preset load, copy/paste). The framework sets `darktable.gui->reset` before calling it, so widget callbacks won't fire.

Sliders and comboboxes created with `_from_params` auto-sync. You only need to manually sync toggle buttons and custom widgets. Always end with `gui_changed(self, NULL, NULL)`:

```c
void gui_update(dt_iop_module_t *self)
{
  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  dt_iop_mymodule_params_t *p = self->params;

  // Toggle buttons need manual sync
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->my_toggle), p->my_bool);

  // Apply all UI state adjustments
  gui_changed(self, NULL, NULL);
}
```

### `gui_changed()` — UI State Adjustments

The single place for all conditional visibility, sensitivity, and dynamic label logic. Called:
- By the framework after a `_from_params` auto-callback (with `widget` = the changed widget, `previous` = old value)
- By you at the end of `gui_update()` (with `widget` = NULL)

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

### The Reset Flag (`darktable.gui->reset`)

A counter (not a boolean) that suppresses callback processing when non-zero. The framework uses it during `gui_update()`.

**Pattern 1: Check at the start of every manual callback:**
```c
static void my_callback(GtkWidget *w, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;  // Always do this

  dt_iop_mymodule_params_t *p = self->params;
  p->value = calculate_new_value();
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
```

**Pattern 2: Suppress callbacks when programmatically updating widgets:**
```c
// Setting slider2 in response to slider1 changing
++darktable.gui->reset;
dt_bauhaus_slider_set(g->slider2, compute_from(p->value1));
--darktable.gui->reset;
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

For picker flags and `dt_color_picker_new_with_cst()`, see [imageop_gui.md](imageop_gui.md).

### Mouse/Drawing Callbacks

For modules with canvas interaction (crop, masks):

- `mouse_moved()`, `button_pressed()`, `button_released()`, `scrolled()` — return 1 if event handled
- `gui_post_expose()` — draw overlays on the center view with Cairo

---

## 3. Thread Safety — Updating GUI from `process()`

### The Problem

`process()` runs on worker threads. GTK+ is not thread-safe. You **cannot** call GTK functions directly from `process()`.

### Guards Before Sending GUI Updates

Always check these conditions before scheduling a GUI update from `process()`:

```c
dt_iop_mymodule_gui_data_t *g = self->gui_data;

if(g != NULL                                          // GUI exists (not export)
   && self->dev->gui_attached                         // darkroom active
   && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))   // not preview/thumbnail
{
  // Schedule GUI update...
}
```

### Pattern A: Critical Section + `g_idle_add`

Store computed values in `gui_data` under mutex, then schedule a GTK-thread callback.

```c
// In process():
dt_iop_gui_enter_critical_section(self);
g->computed_exposure = exposure;
dt_iop_gui_leave_critical_section(self);
g_idle_add(_show_computed, self);

// Callback (GTK main thread):
static gboolean _show_computed(gpointer user_data)
{
  dt_iop_module_t *self = user_data;
  dt_iop_mymodule_gui_data_t *g = self->gui_data;

  dt_iop_gui_enter_critical_section(self);
  float val = g->computed_exposure;
  dt_iop_gui_leave_critical_section(self);

  gchar *str = g_strdup_printf(_("%.2f EV"), val);
  gtk_label_set_text(g->label, str);
  g_free(str);

  return G_SOURCE_REMOVE;  // Run once, then remove
}
```

### Pattern B: Message Passing (Preferred)

Allocate a message struct that the callback owns and frees. No critical sections needed.

```c
typedef struct
{
  dt_iop_module_t *self;
  float values[3];
} mymodule_gui_msg_t;

// Callback (GTK main thread):
static gboolean _update_gui(gpointer data)
{
  mymodule_gui_msg_t *msg = data;
  dt_iop_mymodule_gui_data_t *g = msg->self->gui_data;

  if(g)  // GUI might have been destroyed
  {
    memcpy(g->display_values, msg->values, sizeof(g->display_values));
    gtk_widget_queue_draw(msg->self->widget);
  }

  g_free(msg);  // Callback owns the message
  return G_SOURCE_REMOVE;
}

// At end of process():
if(g != NULL && self->dev->gui_attached
   && (piece->pipe->type & DT_DEV_PIXELPIPE_FULL))
{
  mymodule_gui_msg_t *msg = g_malloc(sizeof(*msg));
  msg->self = self;
  memcpy(msg->values, local_values, sizeof(msg->values));
  g_idle_add(_update_gui, msg);
}
```

### Thread-Safe Redraw Helpers

These use `g_idle_add` internally and are safe to call from any thread:

- `dt_control_queue_redraw_widget(widget)` — redraw a specific widget
- `dt_control_queue_redraw_center()` — redraw the center view

### Common Mistakes

```c
// WRONG — GTK+ call directly in process()
void process(...) {
  gtk_label_set_text(g->label, "value");  // Crash or undefined behavior
}

// WRONG — No mutex when writing gui_data
void process(...) {
  g->computed_value = result;  // Race condition
  g_idle_add(update_gui, self);
}

// WRONG — Forgetting to free message
static gboolean callback(gpointer data) {
  // ... use data ...
  return G_SOURCE_REMOVE;  // Memory leak — must g_free(data)
}

// WRONG — Sending updates for preview/thumbnail pipes
if(g != NULL) {  // Missing pipe type check — floods with updates
  g_idle_add(...);
}
```

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

Same technique — temporarily redirect `self->widget` to the collapsible container:

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

When a user adds a module's widget to the Quick Access Panel, the framework (`libs/modulegroups.c`) automatically:

1. `g_object_ref()` the widget to prevent destruction on removal
2. Removes it from its original parent
3. Inserts a placeholder at the original position to preserve layout
4. Packs the widget into the QAP container
5. Connects `notify::visible` signals to keep visibility in sync
6. On QAP hide: reverses the process, restoring the widget at its original position

**Constraints for QAP-compatible widgets:**
- Must be in a `GtkBox` or `GtkGrid` parent
- Must work in isolation (clear label, good tooltip)
- Complex custom widgets with parent dependencies won't reparent cleanly
- Multi-instance modules disable QAP activation button
