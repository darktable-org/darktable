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
  [cancel it before the module or the image goes away](#the-callback-must-not-outlive-the-module-or-the-image).

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

Four entries in the column are also called directly from GTK-thread code, so they run
on either thread and need the same care for a different reason:
`distort_transform()` and `distort_backtransform()` (mask handling and darkroom zoom
both call them), `blend_colorspace()` (the blend GUI calls it) and
`default_colorspace()` (the color picker calls it while building a picker button,
`src/gui/color_picker_proxy.c`). `distort_mask()` does *not* have that property despite
the similar name — it is invoked only from pipeline code.

Those two colorspace callbacks are called with `NULL` for both `pipe` and `piece` from
the GTK side, so each has to tolerate that. `default_colorspace()` is also reached
indirectly: if you leave `blend_colorspace()` at its default,
`default_blend_colorspace()` forwards its two arguments straight through.

Each column covers the static helpers called from it too. A `process()` that hands
`gui_data` to a helper does not make the access GTK-thread-safe, and that is where
in-tree mistakes hide: `denoiseprofile` writes its variance readout from
`process_variance()`, not from `process()` itself. When you audit a field, follow the
call chain, not just the callback name.

`overlay` is the sharper example, and a live violation of the no-GTK-from-a-pipe-thread
rule rather than a pattern to copy: `process()` calls `_get_overlay_rgba_f()` / `_get_overlay_argb()`,
which call `_setup_overlay()` whenever the overlay buffer has to be built, and that
helper calls `gtk_widget_set_tooltip_text()` — and on a re-imported overlay image
`gtk_widget_queue_draw()` — on the drawing area held in `gui_data`
(`src/iop/overlay.c`). Its `overlay_threadsafe` mutex covers the overlay cache, not the
widgets, so nothing serialises those calls against the GTK main loop.

The right column has no fixed thread, but for the instance that owns your `gui_data`
the picture is narrower:

- **Your darkroom instance** — `process()` runs on one of the three darkroom pipe
  threads (full, preview, preview2), never on the GTK main thread. `commit_params()`
  usually does too, as part of synchronising a pipe, but it has a route onto the GTK
  thread as well: rebuilding a pipe's nodes calls every module's `init_pipe()`, and
  `basecurve`'s calls `commit_params()` straight back (`src/iop/basecurve.c`) — while
  the image switch rebuilds the screen pipes from an idle callback
  (`src/views/darkroom.c`). So not even this instance gives `commit_params()` an
  affinity you can rely on.
- **Every other instance** — export, thumbnail generation, snapshots, the duplicate
  manager, the tethering histogram and the `overlay` module rendering its second image,
  among others — builds a throw-away `dt_develop_t` with its own module instances and
  no GUI (`gui_data == NULL`). Several of those run the pipe synchronously from a GTK
  draw handler, so there the very same callbacks *do* run on the GTK main thread.

Hence the two-sided rule: `commit_params()` must never touch GTK, and must never wait
for the GTK main loop to run anything. Contending for `gui_lock` is not that — it is a
short mutex both sides release quickly, and taking it is the whole point of the next
section. A *synchronous round-trip* through the main loop is the thing that deadlocks,
because the GTK thread may itself be waiting on a pipe.

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

One detail in that quote is not a pattern to copy: `update_curve_lut()` takes only
`self` and reads `self->params`, not the `p` this commit was handed. Those are the same
object in the normal case but not on the pipe's defaults sync, which passes
`default_params` — so a helper of your own should take `p` as an argument. See
[IOP_Module_API.md](IOP_Module_API.md) for that rule. What the quote is here for is the
locking and the GUI/non-GUI split.

`g != NULL` is the whole guard, because a dev whose modules were loaded without a GUI
leaves `gui_data` NULL, and on the normal lifecycle the lock is ready by then:
`dt_iop_gui_init()` initialises `gui_lock` immediately before it calls `gui_init()`.

Treat that as the lifecycle, not as an invariant you can lean on. It has one exception
in tree: when undo/redo has to recreate a deleted instance, `_create_deleted_modules()`
(`src/libs/history.c`) calls `module->gui_init()` *directly* on a freshly zeroed module
rather than going through the wrapper, so that instance ends up with `gui_data`
allocated and `gui_lock` never initialised — and because the same path installs an
expander, `dt_dev_reload_history_items()` skips the `dt_iop_gui_init()` it would
otherwise do (`src/develop/develop.c`). Nothing in a module can detect this, and nothing
you write in `commit_params()` fixes it; it is noted so that the implication is not read
as a guarantee.

`self->dev->gui_attached` adds nothing to that in current code. It is fixed while the
`dt_develop_t` is being built and never toggled afterwards. Most non-GUI contexts pass
FALSE straight to `dt_dev_init()` — export, thumbnailing, style application, mask
objects. Exactly one passes TRUE and overwrites it: the throw-away dev `dt_dev_image()`
renders into, which clears the field before it loads a single module
(`src/develop/develop.c`). `darktable.develop` is given TRUE before
darktable decides whether to start a GUI at all (`src/common/darktable.c`) — so it is
TRUE under `darktable-cli` too, and on its own it is not even a test for "a GUI
exists". Write it
anyway: it is what the tree writes, at about forty places under `src/iop`, and it
records the intent. Just do not read it as a test that anything holds.

Dereferencing `self->dev` to reach it is safe here, because a module only ever reaches
a pipe through `dev->iop` and so always has a `dev` by the time `commit_params()` or
`process()` runs. That is not true everywhere — one instance in the tree has
`gui_data` without a `dev`; see
[Publishing `gui_data` Through a Proxy](#publishing-gui_data-through-a-proxy).

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

Some modules publish a value to the rest of darktable through `dev->proxy` — `exposure`
publishes its effective exposure that way, and `agx` reads it from a GTK-thread helper
(`src/iop/agx.c`). The caller sits in another module and has no way to take your
`gui_lock`, so the accessor has to make its own access safe.

**Derive, don't cache.** Before asking how to lock the field, ask whether it has to be a
field at all. A value that is a function of `params` and of the image can be recomputed
inside the accessor, on the caller's thread, from data that thread already owns. Then
there is no shared field, no lock, and no window in which the pipe and the GUI disagree.
Having a pipe thread compute the same value into `gui_data` so that the GUI can read it
back buys nothing and costs a data race.

`exposure` shows both halves of the answer, because its two modes are genuinely
different:

- **Manual mode** — the effective exposure is the exposure parameter plus the two
  optional compensations, and both compensations come from the image's EXIF data. All of
  that is available to the accessor, so the accessor derives the value and does not read
  `gui_data` at all. Nothing is shared, so nothing needs locking.
- **Deflicker mode** — the correction is computed from a histogram of the raw file,
  which is nowhere in `params`, so there is nothing to derive from and the value has to
  be published. The scalar that carries it is
  [Pattern A](#pattern-a-critical-section--g_idle_add) done correctly:
  `_process_common_setup()` writes `g->deflicker_computed_exposure` inside a critical
  section on the pipe thread, and the `_show_computed()` idle callback reads it inside
  one on the GTK thread (`src/iop/exposure.c`).

The histogram *behind* that scalar is a second piece of shared state, and it is not
covered. `gui_update()` and `gui_changed()` free `g->deflicker_histogram` and rebuild it
on the GTK thread, while `_process_common_setup()` reads that pointer and its statistics
on a pipe thread — neither side takes `gui_lock`, and in `gui_update()` the free sits
between two critical sections that guard other fields. That is the trap from
[Short Is Not The Same As Correct](#short-is-not-the-same-as-correct), live in the tree:
a heap buffer in `gui_data` that one thread can free while the other is walking it.
Publishing a value safely does not make the state it was computed from safe; each shared
field needs its own answer.

> **The manual-mode bullet above describes `exposure` after pull request #21974**, not
> master as it stands. #21974 removes the cached `effective_exposure` field and makes
> the accessor derive its result. Until it lands, `src/iop/exposure.c` still caches:
> `commit_params()` writes `effective_exposure` from a pipe thread and
> `_exposure_proxy_get_effective_exposure()` reads it from the GTK thread, neither under
> `gui_lock` and the reader without a NULL check — the exact race this section tells you
> to design away. The deflicker bullet and the histogram paragraph describe the tree
> as it stands.

<!-- TODO @kofa : check again after 21974 has been merged -->

So which shape a proxy accessor needs depends on which of the two cases it is. A
derived value makes the accessor a plain function of `params`. A value that genuinely
has to come from the pipe makes it this:

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

The example tests `g` and nothing else, and where a module writes both, the order
matters: `if(!g || !self->dev->gui_attached)`, as `colorequal` and `toneequal` do, never
`self->dev->gui_attached && g`. The reason is that `self->dev` is not guaranteed. One
instance in the tree has `gui_data` without a `dev`: at startup, `_init_module_so()`
builds a throw-away instance with `dev == NULL` and runs `dt_iop_gui_init()` on it so
that its widgets can register their accelerators (`src/develop/imageop.c`). Written the
other way round, the guard would dereference NULL in its first operand, before the `g`
test that decides the question. Inside `process()` and `commit_params()` that cannot
happen — a module only reaches a pipe through `dev->iop`, so it always has a `dev` there
— but a proxy accessor takes whatever instance its caller hands it, so put `g` first and
the case never arises.

That startup instance is also why the `dev->proxy` function pointers outlive every live
instance. It is torn down again straight away, and `exposure`'s `gui_cleanup()` clears
`proxy.exposure.module`, but nothing clears the function pointers it registered
(`src/iop/exposure.c`) — so a non-NULL accessor pointer does not mean there is an
instance behind it. In tree the callers carry that check:
`dt_dev_exposure_get_effective_exposure()` resolves a live, enabled instance itself and
passes that one, and the other two getters go through a helper that requires
`proxy.exposure.module` to be set (`src/develop/develop.c`).

The producing side — usually `commit_params()` or `process()` — must take the same lock.
Publishing a raw pointer into `gui_data` through a proxy is worse still: the reader then
has no lock to take at all, and no way to know the GUI is being torn down.

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
  [The Callback Must Not Outlive the Module or the Image](#the-callback-must-not-outlive-the-module-or-the-image).
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

if(g != NULL                          // GUI exists (not export) — the test that holds
   && self->dev->gui_attached         // conventional; adds nothing
   && dt_pipe_is_full(piece->pipe))   // the pipe this value belongs to
{
  // Schedule GUI update...
}
```

The pipe test is the one to think about rather than copy. `dt_pipe_is_full()` is right
when the value only means something for the full-resolution render, and it keeps the two
preview pipes from queueing an update apiece. When the value is produced elsewhere, test
for that pipe instead: `exposure` computes its deflicker readout on the preview pipe and
publishes it from there (`dt_pipe_is_preview()`, `src/iop/exposure.c`). What is never
right is no pipe test at all — then every pipe running your module queues its own
update.

The middle test is the prevailing idiom rather than a working guard — see
[Using `gui_data` from `commit_params()`](#using-gui_data-from-commit_params)
for what it does and does not establish.

Reading `g` once is enough for the length of the run: the framework holds the pipe
mutexes across module GUI teardown, so a non-NULL `g` cannot be freed while your
`process()` or `commit_params()` is executing. What the guard does not cover is work
you hand to the main loop — see
[The Callback Must Not Outlive the Module or the Image](#the-callback-must-not-outlive-the-module-or-the-image).

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
  // second net only — see "The Callback Must Not Outlive the Module or the Image"
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
// And in change_image(): an image switch keeps the base instance and its gui_data,
// so a source queued for the old image would otherwise run against the new one
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

## The Callback Must Not Outlive the Module or the Image

The framework guarantees one thing here: **no module GUI is torn down while a pipe is
running.** All four teardown sites — leaving the darkroom, switching image, deleting an
instance, and undoing or redoing a module add or delete — hold all three screen-pipe
mutexes across `dt_iop_gui_cleanup_module()`. Deleting an instance and undo/redo take
them through the `dt_dev_pixelpipe_stop_and_lock_all()` /
`dt_dev_pixelpipe_unlock_all()` pair (`src/develop/develop.c`), which also flags the
pipes for shutdown; darkroom exit and the image switch lock the same three mutexes
directly (`src/views/darkroom.c`), the image switch with `trylock` and a re-queue for
as long as a pipe is busy. One consequence for module authors: on those four paths
`gui_cleanup()` runs with all three pipe mutexes held, so it must not call anything that
waits on a pipe. They are not the only callers, though — the startup accelerator probe
tears its throw-away instance down with no pipe lock held at all
(`src/develop/imageop.c`; see
[Publishing `gui_data` Through a Proxy](#publishing-gui_data-through-a-proxy)). So take
the mutexes as a constraint on what `gui_cleanup()` may do, never as a guarantee it can
lean on.

That closes the window against pipe threads. It does not close the one you open
yourself by handing work to the main loop.

`g_idle_add()` hands the main loop a raw `dt_iop_module_t *`. What teardown does to
that pointer depends on the path, and there are three shapes:

- **Leaving the darkroom** — for every module that has a GUI: `gui_cleanup()` runs,
  `gui_lock` is destroyed, `gui_data` is freed, and then the module itself is freed.
  (Hidden modules never get a GUI, so all three teardown shapes skip them.)
- **Deleting an instance, or undo/redo of an add or delete** — the same GUI teardown
  runs, but the module struct survives: it is parked in `dev->alliop` for pipes that
  may still reference it, and freed when the darkroom is left or the image is switched.
- **Switching image** — this one splits (`src/views/darkroom.c`). Each module's *base*
  instance — the one with the lowest `multi_priority` — is **kept**: no `gui_cleanup()`,
  `gui_lock` and `gui_data` stay alive, and the instance is re-used for the new image
  after `dt_iop_reload_defaults()` and, if it implements one, `change_image()`. Its
  extra instances are torn down like a darkroom exit — `gui_cleanup()`, then the struct
  is freed — and the ones the new image's history needs are rebuilt with a fresh
  `gui_init()`.

In the first two shapes your source is still sitting in the main loop and fires against
freed GUI state. In the third, for a base instance, nothing is freed and nothing
cancels the source: it fires on a live module and writes a value computed from the
*previous* image into the new image's widgets.

`dt_iop_gui_cleanup_module()` sets `module->gui_data` to NULL after freeing it, and
teardown and idle dispatch both run on the GTK main thread, so they cannot interleave.
For a **deleted instance** that makes `if(!self->gui_data) return G_SOURCE_REMOVE;` at
the very top of the callback a real guard: the module struct is still there, parked in
`dev->alliop`. On **darkroom exit**, and for an **extra instance on an image switch**,
it is not — the struct itself is freed right after its GUI, so the test reads freed
memory. For a **base instance on an image switch** the check is blind rather than late:
`gui_data` is still allocated, so the test passes and the stale update goes through.
Where the check does work it has to come first, ahead of
`dt_iop_gui_enter_critical_section()`, which would otherwise lock a destroyed mutex.
Treat it as a second net, never as a substitute for cancelling.

Cancel in `gui_cleanup()` instead. If the source data is `self`, that is one line:

```c
void gui_cleanup(dt_iop_module_t *self)
{
  ...
  // g_idle_remove_by_data() drops one source per call, so drain
  while(g_idle_remove_by_data(self)) ;
}
```

`gui_cleanup()` is not the only cancellation point, because it does not run on every
transition that invalidates your payload. A base instance survives an image switch, so
a source queued while the old image was loaded fires against the new one. Drain in
`change_image()` too — that is the callback the framework gives a retained instance for
exactly this kind of reset, and it runs while the three pipe mutexes are still held,
so no pipe thread can queue a source behind the drain (`basicadj` and `retouch` use it
to clear GUI state):

```c
void change_image(dt_iop_module_t *self)
{
  while(g_idle_remove_by_data(self)) ;
  ...   // reset the rest of gui_data for the new image
}
```

If the queued value can simply be recomputed, cancelling is the whole fix: the new
image's first pipe run queues a fresh update.

`exposure` is the in-tree example of Pattern A, and it does cancel — but note that its
`gui_cleanup()` calls `g_idle_remove_by_data(self)` once rather than draining in a
loop. `process()` can queue a source on every qualifying preview run, so one call is
not guaranteed to remove them all. Follow the loop shown above, not `exposure`'s
version of the last line. `src/bauhaus/bauhaus.c` drains in a loop and is the better
model for that one detail.

**Why the loop is not a detail.** Every one of these transitions holds the pipe mutexes
before it touches a module GUI, so nothing can queue a fresh source once
`gui_cleanup()` — or `change_image()` — has started. Draining there is therefore
sufficient, as long as it actually drains. What a survivor does depends on the path,
and that is what makes the NULL check a second net rather than a guard. After an
instance delete the struct is still allocated and `gui_data` has been cleared, so the
check sees NULL and returns; without it the callback dereferences NULL and the process
stops there. After a darkroom exit, or for an extra instance on an image switch, the
struct itself has been freed, so reading `self->gui_data` *is* the use-after-free — the
check comes too late whatever it returns, and what it returns is undefined: the freed
bytes may still hold the NULL that cleanup wrote, or anything the allocator has since
put there. For a base instance on an image switch nothing is freed, the check passes,
and the callback runs to completion against the wrong image. Only the drain covers all
three.

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
g_idle_add(_update_gui, self);  // Fires after gui_cleanup() freed the GUI state, or —
                                // on an image switch, which keeps the base instance —
                                // against the new image. Drain in gui_cleanup() and
                                // in change_image()

// WRONG — No pipe type check at all
if(g != NULL) {  // every pipe that runs the module queues its own update.
  g_idle_add(...);  // Test for the pipe the value belongs to — often, but
}                   // not always, dt_pipe_is_full()

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
