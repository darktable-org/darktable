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
Check: DT_GUARD_GUI_UPDATE()
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
Framework call DT_ENTER_GUI_UPDATE()
    ↓
Framework calls your gui_update()
    ↓
You sync widgets, call gui_changed(self, NULL, NULL)
    ↓
Framework call DT_LEAVE_GUI_UPDATE()
```

### `gui_update()` — Sync Widgets from Params

Called by the framework when params change externally (image switch, history navigation, preset load, copy/paste). The framework increments atomically gui state `DT_ENTER_GUI_UPDATE()` before calling it, so widget callbacks won't fire.

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

### The Reset Flag (`DT_GUARD_GUI_UPDATE`)

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

For picker flags and `dt_color_picker_new_with_cst()`, see [imageop_gui.md](imageop_gui.md).

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

## 3. Thread Safety — Sharing `gui_data` Between Threads

### The Problem

`process()` is not a GTK callback and has no guaranteed thread affinity. GTK+ is not thread-safe. You **cannot** call GTK functions directly from `process()`.

The same split applies to data: `gui_data` is not owned by the GTK thread alone. Both
directions need care — the pipe writing values for the GUI to display, and the GUI
writing values the pipe will read.

The rules, in short:

- [Know which thread you are on](#which-thread-am-i-on). `commit_params()` and
  `process()` are pipe callbacks, not GUI ones.
- [Take the GUI critical section](#using-gui_data-from-commit_params) for every field
  both sides touch — and only when a GUI exists.
- [Hold it as long as the value must stay valid](#short-is-not-the-same-as-correct) —
  not just for the load.
- Never call GTK from a pipe thread. Hand the update to the main loop
  ([Pattern A](#pattern-a-critical-section--g_idle_add)), and
  [cancel it in `gui_cleanup()`](#the-callback-must-not-outlive-the-module).

### Which Thread Am I On?

| Always the GTK main thread | Pipeline callbacks — no thread guarantee |
| --- | --- |
| `gui_init()`, `gui_cleanup()` | `commit_params()` |
| `gui_update()`, `gui_changed()` | `process()`, `process_cl()` |
| widget callbacks (sliders, buttons, combos) | `process_tiling()`, `process_tiling_cl()` |
| draw / expose callbacks, `gui_post_expose()` | `modify_roi_in()`, `modify_roi_out()`, `tiling_callback()` |
| mouse and scroll handlers | `init_pipe()`, `cleanup_pipe()` |
| | `output_format()`, the four colorspace callbacks |
| | `distort_transform()`, `distort_backtransform()`, `distort_mask()` |

The right column is the per-instance `src/iop/iop_api.h` callbacks the pipe drives that
can reach `gui_data`. It is not the whole API surface — the pipe also calls metadata
callbacks such as `flags()` and `operation_tags()`, but their signatures give them no
module instance, so they cannot touch `gui_data`. (`input_format()` is declared but has
no caller in the tree, so it is not listed.)

Three entries in the column are also called directly from GTK-thread code, so they run
on either thread and need the same care for a different reason:
`distort_transform()` and `distort_backtransform()` (mask handling and darkroom zoom
both call them), and `blend_colorspace()` (the blend GUI calls it). `distort_mask()`
does *not* have that property despite the similar name — it is invoked only from
pipeline code.

If you override `blend_colorspace()`, note that the GTK-side callers pass `NULL` for
both `pipe` and `piece`. Inheriting the default is not automatically safe either:
`default_blend_colorspace()` forwards both arguments straight to your
`default_colorspace()`, so that one has to tolerate `NULL` too.

Each column covers the static helpers called from it too. A `process()` that hands
`gui_data` to a helper does not make the access GTK-thread-safe, and that is where
in-tree mistakes hide: `denoiseprofile` writes its variance readout from
`process_variance()`, not from `process()` itself. When you audit a field, follow the
call chain, not just the callback name.

The right column has no fixed thread, but for the instance that owns your `gui_data`
the picture is narrower:

- **Your darkroom instance** — `commit_params()` and `process()` run on one of the
  three darkroom pipe threads (full, preview, preview2), never on the GTK main thread.
- **Every other instance** — export, thumbnail generation, snapshots, the duplicate
  manager, the tethering histogram and the `overlay` module rendering its second image,
  among others — builds a throw-away `dt_develop_t` with its own module instances and
  no GUI (`gui_data == NULL`). Several of those run the pipe synchronously from a GTK
  draw handler, so there the very same callbacks *do* run on the GTK main thread.

Hence the two-sided rule: `commit_params()` must never touch GTK, and must never block
waiting for the GTK thread either.

### Using `gui_data` from `commit_params()`

`commit_params()` is the one that surprises people. It reads like module setup code
that belongs to the GUI, but it is called while a pipe is being synchronised. The
three darkroom pipes are separate threads, yet synchronisation holds
`dev->history_mutex` across the commit, so `commit_params()` does not run concurrently
with itself for one module instance. It can still overlap with `process()` on another
pipe and with any GTK-thread callback — that is what the lock is for.

Two preconditions, both mandatory:

- **The GUI may not exist.** `gui_data` is `NULL` in export and batch mode, and
  `gui_lock` — the mutex behind `dt_iop_gui_enter_critical_section()` — is only
  initialised by `dt_iop_gui_init()`. Entering the critical section without a GUI
  locks a mutex that was never set up.
- **Processing must not depend on the GUI cache.** The same `commit_params()` runs
  during export, where there is nothing to read. Whatever the cache saves for the
  darkroom, the non-GUI branch has to compute independently.

`toneequal` is the reference for the shape:

```c
void commit_params(dt_iop_module_t *self, ...)
{
  dt_iop_toneequalizer_gui_data_t *g = self->gui_data;
  ...
  if(self->dev->gui_attached && g)
  {
    // darkroom: refresh and reuse the GUI-side cache
    dt_iop_gui_enter_critical_section(self);
    if(g->sigma != p->smoothing) g->interpolation_valid = FALSE;
    g->sigma = p->smoothing;
    dt_iop_gui_leave_critical_section(self);

    update_curve_lut(self);   // takes the lock itself — see below
    ...
  }
  else
  {
    // export / headless: solve from scratch, no cache
    build_interpolation_matrix(A, p->smoothing);
    pseudo_solve(A, factors, CHANNELS, PIXEL_CHAN, TRUE);
    ...
  }
}
```

`g != NULL` is the load-bearing half of the guard; `self->dev->gui_attached` adds that
the darkroom is live.

### Writing `gui_data` from a Widget Callback

This is allowed and common. The question is only whether you need the lock:

- **Field touched by the GTK thread only** — no lock. Mouse position, which node is
  selected, a cached gradient used solely for drawing the widget.
- **Field also read or written by `commit_params()` or `process()`** — take the lock.
  Typically these are cached results and their validity flags.

```c
static void _slider_callback(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_mymodule_params_t *p = self->params;
  dt_iop_mymodule_gui_data_t *g = self->gui_data;

  p->xyz = dt_bauhaus_slider_get(slider);

  // g->cached_xyz is also written by commit_params() on a pipe thread
  dt_iop_gui_enter_critical_section(self);
  if(g->cached_xyz != p->xyz) g->cache_valid = FALSE;
  g->cached_xyz = p->xyz;
  dt_iop_gui_leave_critical_section(self);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
```

If you are unsure whether a field crosses threads, search for every read and write of
it and check which callback each one sits in, using the table above.

**Asking for a reprocess is not synchronisation.** A widget callback that writes a
field and then calls `dt_dev_reprocess_center()`, `dt_dev_reprocess_all()` or
`dt_dev_add_history_item()` still needs the lock. Those calls set `pipe->changed`,
invalidate buffers and queue a redraw; none of them waits for a running pipe or issues
a barrier, so a pipe already inside `process()` is not ordered against your write at
all — it may see the old value, the new one, or change its mind mid-frame if the
compiler re-loads the field.

### Publishing `gui_data` Through a Proxy

Some modules expose a `gui_data` field to the rest of darktable through
`dev->proxy` — `exposure` publishes its computed exposure that way, and `agx` reads it
from a GTK-thread helper. The caller is in another module and has no way to take your
`gui_lock`, so the accessor has to do it:

```c
static float _mymodule_proxy_get_value(dt_iop_module_t *self)
{
  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  if(!g) return 0.0f;

  dt_iop_gui_enter_critical_section(self);
  const float v = g->computed_value;
  dt_iop_gui_leave_critical_section(self);
  return v;
}
```

The producing side — usually `commit_params()` or `process()` — must take the same
lock. Publishing a raw pointer into `gui_data` through a proxy is worse still: the
reader then has no lock to take at all, and no way to know the GUI is being torn down.

### The Lock Is Not Recursive

`dt_iop_gui_enter_critical_section()` takes a plain, non-recursive mutex. Taking it
twice on the same thread deadlocks — and the second acquisition is usually invisible,
hidden inside a helper you call.

```c
// WRONG — self-deadlock if _rebuild_cache() takes the lock itself
dt_iop_gui_enter_critical_section(self);
g->cached_xyz = p->xyz;
_rebuild_cache(self);
dt_iop_gui_leave_critical_section(self);

// RIGHT — keep the section around the writes only
dt_iop_gui_enter_critical_section(self);
g->cached_xyz = p->xyz;
dt_iop_gui_leave_critical_section(self);

_rebuild_cache(self);   // takes the lock itself
```

So before calling anything from inside a critical section, check whether the callee
locks. Keep critical sections short and free of function calls where you can — that
avoids the problem instead of reasoning about it.

### Short Is Not The Same As Correct

Shortening a critical section past the point where the value is still needed is its own
bug, and a nastier one, because the module now looks locked.

```c
// WRONG — the pointer load is protected, the pointee is not
dt_iop_gui_enter_critical_section(self);
my_cache_t *c = g->cache;
dt_iop_gui_leave_critical_section(self);

use_cache(c);   // another thread may have freed g->cache by now
```

The lock has to cover the whole span in which the value must stay valid, not just the
load. For a plain scalar that span ends at the load, and a snapshot is exactly right.
For anything the other thread can free or resize — a heap pointer, a buffer plus the
width and height that describe it — it ends when you stop using the value, and you have
to pick one of:

- **Hold the lock through the last use.** The simplest thing that works — but only if
  *every* path that frees or replaces the object takes the same lock. `gui_cleanup()`
  does not, and does not have to — see
  [The Callback Must Not Outlive the Module](#the-callback-must-not-outlive-the-module).
  Every *other* path that frees or replaces the object — a widget callback, a reset, a
  reload — still has to take it. It also needs that use to be short: holding the mutex
  across an allocation, a device transfer or a full-buffer copy blocks the other thread
  for as long as it takes, and when the blocked thread is the GTK one the user sees it.
- **Copy the data, not the pointer**, inside the section, then work on your copy.
- **Reference-count the object, or hand ownership over explicitly**, so the other thread
  cannot free something you still hold.

The same trap catches tuples. If a buffer and its dimensions are written together under
the lock, read them together under the lock too. Reading the dimensions again after
releasing can pair a new size with an old allocation.

### Guards Before Sending GUI Updates

Always check these conditions before scheduling a GUI update from `process()`:

```c
dt_iop_mymodule_gui_data_t *g = self->gui_data;

if(g != NULL                          // GUI exists (not export)
   && self->dev->gui_attached         // darkroom active
   && dt_pipe_is_full(piece->pipe))   // not preview/preview2
{
  // Schedule GUI update...
}
```

Reading `g` once is enough for the length of the run: the framework holds the pipe
mutexes across module GUI teardown, so a non-NULL `g` cannot be freed while your
`process()` or `commit_params()` is executing. What the guard does not cover is work
you hand to the main loop — see
[The Callback Must Not Outlive the Module](#the-callback-must-not-outlive-the-module).

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
  // second net only — see "The Callback Must Not Outlive the Module"
  if(!g) return G_SOURCE_REMOVE;

  dt_iop_gui_enter_critical_section(self);
  float val = g->computed_exposure;
  dt_iop_gui_leave_critical_section(self);

  gchar *str = g_strdup_printf(_("%.2f EV"), val);
  gtk_label_set_text(g->label, str);
  g_free(str);

  return G_SOURCE_REMOVE;  // Run once, then remove
}

// In gui_cleanup(): drop callbacks still queued for this module
while(g_idle_remove_by_data(self)) ;
```

### Pattern B: Message Passing

Allocate a message struct that the callback owns and frees. The payload never lives in
`gui_data`, so no critical section is needed for it — but cancelling the callback is
harder, see below.

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

  memcpy(g->display_values, msg->values, sizeof(g->display_values));
  gtk_widget_queue_draw(msg->self->widget);

  g_free(msg);  // Callback owns the message
  return G_SOURCE_REMOVE;
}

// At end of process():
if(g != NULL && self->dev->gui_attached
   && dt_pipe_is_full(piece->pipe))
{
  mymodule_gui_msg_t *msg = g_malloc(sizeof(*msg));
  msg->self = self;
  memcpy(msg->values, local_values, sizeof(msg->values));
  // keyed on msg, so it cannot be cancelled by data — see the next section
  g_idle_add(_update_gui, msg);
}
```

### The Callback Must Not Outlive the Module

The framework guarantees one thing here: **no module GUI is torn down while a pipe is
running.** All four teardown sites — leaving the darkroom, switching image, deleting an
instance, and undoing or redoing a module add or delete — stop the three screen pipes
and hold their mutexes across `dt_iop_gui_cleanup_module()`.
`dt_dev_pixelpipe_stop_and_lock_all()` and `dt_dev_pixelpipe_unlock_all()`
(`src/develop/develop.c`) are the pair that does it. One consequence for module
authors: `gui_cleanup()` runs with all three pipe mutexes held, so it must not call
anything that waits on a pipe.

That closes the window against pipe threads. It does not close the one you open
yourself by handing work to the main loop.

`g_idle_add()` hands the main loop a raw `dt_iop_module_t *`. Teardown comes in two
shapes, and both pull the GUI out from under it:

- **Leaving the darkroom, or switching image** — `gui_cleanup()` runs, `gui_lock` is
  destroyed, `gui_data` is freed, and then the module itself is freed.
- **Deleting an instance, or undo/redo of an add or delete** — the same GUI teardown
  runs, but the module struct survives: it is parked in `dev->alliop` for pipes that
  may still reference it, and freed only when the darkroom is left.

Either way your source is still sitting in the main loop and fires afterwards, against
freed GUI state.

`dt_iop_gui_cleanup_module()` sets `module->gui_data` to NULL after freeing it, and
teardown and idle dispatch both run on the GTK main thread, so they cannot interleave.
For a **deleted instance** that makes `if(!self->gui_data) return G_SOURCE_REMOVE;` at
the very top of the callback a real guard: the module struct is still there, parked in
`dev->alliop`. On **darkroom exit** it is not — the struct itself is freed right after
its GUI, so the test reads freed memory. Where the check does work it has to come
first, ahead of `dt_iop_gui_enter_critical_section()`, which would otherwise lock a
destroyed mutex. Treat it as a second net, never as a substitute for cancelling.

Cancel in `gui_cleanup()` instead. If the source data is `self`, that is one line:

```c
void gui_cleanup(dt_iop_module_t *self)
{
  ...
  // g_idle_remove_by_data() drops one source per call, so drain
  while(g_idle_remove_by_data(self)) ;
}
```

`exposure` is the in-tree example of Pattern A, and it does cancel — but note that its
`gui_cleanup()` calls `g_idle_remove_by_data(self)` once rather than draining in a
loop. `process()` can queue a source on every qualifying preview run, so one call is
not guaranteed to remove them all. Follow the loop shown above, not `exposure`'s
version of the last line. `src/bauhaus/bauhaus.c` drains in a loop and is the better
model for that one detail.

**Why the loop is not a detail.** Every teardown path stops the pipes before it touches
a module GUI, so nothing can queue a fresh source once `gui_cleanup()` has started.
Draining there is therefore sufficient — as long as it actually drains. A source that a
single `g_idle_remove_by_data()` failed to remove no longer scribbles over freed
memory, because `gui_data` is NULL by the time it fires; it dereferences NULL instead
and takes the process down.

If the source data is a heap message (Pattern B), `g_idle_remove_by_data()` cannot find
it — the source is keyed on the message, not on the module. You then have to track the
source ids yourself: keep the id in `gui_data`, and queue with `g_idle_add_full()`
passing `g_free` as the `GDestroyNotify`. Drop the `g_free(msg)` from the callback if
you do — the notification runs after a normal dispatch as well as on cancellation, so
keeping both frees the message twice. A second update also has to supersede the first
rather than overwrite its id unremoved. That is more bookkeeping than the critical
section Pattern B set out to avoid, so prefer Pattern A unless the payload genuinely
cannot live in `gui_data`.

### Thread-Safe Redraw Helpers

These marshal the redraw onto the GTK main context internally and are safe to call from
any thread:

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
  return G_SOURCE_REMOVE;  // Memory leak — g_free(data) here, or pass g_free as
}                          // the GDestroyNotify of g_idle_add_full(). Not both:
                           // the notify also runs after a normal dispatch

// WRONG — Queued callback with no cancellation
g_idle_add(_update_gui, self);  // Fires after gui_cleanup() freed the GUI state,
                                // unless gui_cleanup() drains the queued sources

// WRONG — Sending updates for preview/preview2 pipes
if(g != NULL) {  // Missing pipe type check — floods with updates
  g_idle_add(...);
}

// WRONG — No mutex in a widget callback either, if the pipe reads the field
static void _callback(GtkWidget *w, dt_iop_module_t *self) {
  g->cache_valid = FALSE;   // commit_params() reads this on a pipe thread
}

// WRONG — Treating a reprocess request as a barrier
static void _callback(GtkWidget *w, dt_iop_module_t *self) {
  g->show_mask = TRUE;                  // process() reads this on a pipe thread
  dt_dev_reprocess_center(self->dev,    // only flags the pipe and queues a redraw:
                          self->iop_order);  // it does not wait for the pipe that is
}                                            // already running, so the write still
                                             // needs the lock

// WRONG — Assuming commit_params() runs on the GTK thread
void commit_params(...) {
  gtk_widget_queue_draw(g->area);  // No thread guarantee; use a thread-safe redraw
}

// WRONG — Locking in commit_params() without checking that the GUI exists
void commit_params(...) {
  dt_iop_gui_enter_critical_section(self);  // gui_lock is only initialised by
  g->cache_valid = FALSE;                   // dt_iop_gui_init(); g is NULL on export
  dt_iop_gui_leave_critical_section(self);
}

// WRONG — Calling a locking helper from inside a critical section
dt_iop_gui_enter_critical_section(self);
_update_cache(self);   // Deadlock: the mutex is not recursive
dt_iop_gui_leave_critical_section(self);
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
