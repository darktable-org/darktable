# Thread Safety — Sharing `gui_data` Between Threads

How an IOP module shares `gui_data` between the GTK main thread and the pixelpipe
threads: which callback runs where, when the GUI critical section is required, and how
to send an update to the GUI without outliving the module.

See also:
- [GUI.md](GUI.md) — GUI architecture: UI construction, events and callbacks, reparenting
- [IOP_Module_API.md](IOP_Module_API.md) — Module API reference, `commit_params()`, lifecycle
- [pixelpipe_architecture.md](pixelpipe_architecture.md) — Pixelpipe data flow, caching, ROI, ordering

---

## The Problem

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

## Which Thread Am I On?

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

## Using `gui_data` from `commit_params()`

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

## Writing `gui_data` from a Widget Callback

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

## Publishing `gui_data` Through a Proxy

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

## The Lock Is Not Recursive

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

## Short Is Not The Same As Correct

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

## Guards Before Sending GUI Updates

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

## Pattern A: Critical Section + `g_idle_add`

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

## Pattern B: Message Passing

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

## The Callback Must Not Outlive the Module

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
`dev->alliop`. On **darkroom exit or an image switch** it is not — the struct itself is
freed right after its GUI, so the test reads freed memory. Where the check does work it
has to come first, ahead of `dt_iop_gui_enter_critical_section()`, which would otherwise
lock a destroyed mutex. Treat it as a second net, never as a substitute for cancelling.

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
Draining there is therefore sufficient — as long as it actually drains. What a survivor
does depends on the path, and that is what makes the NULL check a second net rather
than a guard. After an instance delete the struct is still allocated and `gui_data` has
been cleared, so the check sees NULL and returns; without it the callback dereferences
NULL and the process stops there. After a darkroom exit or an image switch the struct
itself has been freed, so reading `self->gui_data` *is* the use-after-free — the check
comes too late whatever it returns, and what it returns is undefined: the freed bytes
may still hold the NULL that cleanup wrote, or anything the allocator has since put
there. Only the drain covers both paths.

If the source data is a heap message (Pattern B), `g_idle_remove_by_data()` cannot find
it — the source is keyed on the message, not on the module. You then have to track the
source ids yourself: keep the id in `gui_data`, and queue with `g_idle_add_full()`
passing `g_free` as the `GDestroyNotify`. Drop the `g_free(msg)` from the callback if
you do — the notification runs after a normal dispatch as well as on cancellation, so
keeping both frees the message twice. A second update also has to supersede the first
rather than overwrite its id unremoved. That is more bookkeeping than the critical
section Pattern B set out to avoid, so prefer Pattern A unless the payload genuinely
cannot live in `gui_data`.

## Thread-Safe Redraw Helpers

These marshal the redraw onto the GTK main context internally and are safe to call from
any thread:

- `dt_control_queue_redraw_widget(widget)` — redraw a specific widget
- `dt_control_queue_redraw_center()` — redraw the center view

## Common Mistakes

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
