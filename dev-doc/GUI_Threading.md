# Thread Safety — Sharing `gui_data` Between Threads

How an IOP module shares `gui_data` between the GTK main thread and the pixelpipe
worker threads, and between two pipe worker threads: which callback runs where, when the
GUI critical section is required, how to send an update to the GUI without outliving the
module, and which parts of this the framework already does for you.

See also:
- [GUI.md](GUI.md) — GUI architecture: UI construction, events and callbacks, reparenting
- [IOP_Module_API.md](IOP_Module_API.md) — Module API reference, `commit_params()`, lifecycle
- [pixelpipe_architecture.md](pixelpipe_architecture.md) — Pixelpipe data flow, caching, ROI, ordering

---

## The Problem

`process()` is not a GTK callback and has no guaranteed thread affinity. GTK is not thread-safe. You **cannot** call GTK functions directly from `process()`.

The same split applies to data: `gui_data` is not owned by the GTK thread alone. Three
directions need care:

- the pipe writing values for the GUI to display,
- the GUI writing values the pipe will read,
- and [one pipe writing values another pipe reads](#passing-values-between-pipes-through-gui_data),
  because `gui_data` is where that channel lives too.

The rules, in short:

- [Know which thread you are on](#which-thread-am-i-on). `commit_params()` and
  `process()` are pipe callbacks, not GUI ones.
- [Take the GUI critical section](#using-gui_data-from-commit_params) for every field
  more than one thread touches — and only when a GUI exists.
- [Hold it as long as the value must stay valid](#hold-the-lock-as-long-as-the-value-must-stay-valid) —
  not just for the load.
- Never call GTK from a pipe worker thread. Hand the update to the main loop
  ([Pattern A](#pattern-a-critical-section--g_idle_add)), and
  [cancel it before the module or the image goes away](#the-callback-must-not-outlive-the-module-or-the-image).
- [Check whether the framework has already done it](#the-framework-service-for-per-pixel-readouts)
  before hand-rolling buffer, hash and lock plumbing for a cursor readout.

The sections, in order:

**Where your code runs**
- [Which Thread Am I On?](#which-thread-am-i-on)

**Locking rules**
- [Using `gui_data` from `commit_params()`](#using-gui_data-from-commit_params)
- [Writing `gui_data` from a Widget Callback](#writing-gui_data-from-a-widget-callback)
- [Publishing `gui_data` Through a Proxy](#publishing-gui_data-through-a-proxy)
- [Passing Values Between Pipes Through `gui_data`](#passing-values-between-pipes-through-gui_data)
- [The Lock Is Not Recursive](#the-lock-is-not-recursive)
- [Hold the Lock as Long as the Value Must Stay Valid](#hold-the-lock-as-long-as-the-value-must-stay-valid)

**What the framework already does**
- [The Framework Service for Per-Pixel Readouts](#the-framework-service-for-per-pixel-readouts)

**Sending updates to the GUI**
- [Guards Before Sending GUI Updates](#guards-before-sending-gui-updates)
- [Pattern A: Critical Section + `g_idle_add`](#pattern-a-critical-section--g_idle_add)
- [Pattern B: Message Passing](#pattern-b-message-passing)
- [The Callback Must Not Outlive the Module or the Image](#the-callback-must-not-outlive-the-module-or-the-image)

**Then**
- [Common Mistakes](#common-mistakes)

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

Each column covers the static helpers called from it too. A `process()` that hands
`gui_data` to a helper does not make the access GTK-thread-safe, and that is where
mistakes hide: the offending line is two or three frames down, in a function whose name
says nothing about which thread reaches it. When you audit a field, follow the call
chain, not just the callback name.

This would be wrong:

```c
static void _rebuild_cache(dt_iop_module_t *self, ...)
{
  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  ...
  g->readout = value;                            // no critical section
  gtk_widget_set_tooltip_text(g->area, text);    // GTK, on whatever thread got here
  gtk_widget_queue_draw(g->area);
}

void process(...)
{
  if(_cache_is_stale(...)) _rebuild_cache(self, ...);   // a pipe worker thread
  ...
}
```

A mutex of your own does not rescue that. Holding one across `_rebuild_cache()`
serializes your pipe worker threads against each other, which is worth doing for the
cache; the GTK main loop never takes that mutex and is not ordered by it, so the two
widget calls are exactly as unsynchronized as they were. Only getting them onto the main
loop fixes them — [Pattern A](#pattern-a-critical-section--g_idle_add), or one of the
thread-safe redraw helpers below.

### Thread-Safe Redraw Helpers

These marshal the redraw onto the GTK main context internally and are safe to call from
any thread:

- `dt_control_queue_redraw_widget(widget)` — redraw a specific widget
- `dt_control_queue_redraw_center()` — redraw the center view

### The Picture for Your Own Instance

The right column has no fixed thread, but for the instance that owns your `gui_data`
the picture is narrower:

- **Your darkroom instance** — `process()` runs on one of the three screen pipes' worker
  threads (full, preview, preview2), never on the GTK main thread. `commit_params()`
  usually does too, as part of synchronizing a pipe, but it has a route onto the GTK
  thread as well: rebuilding a pipe's nodes calls every module's `init_pipe()`, and
  `basecurve`'s calls `commit_params()` straight back (`src/iop/basecurve.c`) — while
  the image switch rebuilds the screen pipes from an idle callback
  (`src/views/darkroom.c`). So not even this instance gives `commit_params()` an
  affinity you can rely on.
- **Every instance in another develop context** — export, thumbnail generation,
  snapshots, the duplicate manager, the tethering view's histogram of the last captured
  image, and the `overlay` module rendering its second image, among others — belongs to
  a separate `dt_develop_t` with its own module instances and no GUI
  (`gui_data == NULL`). Most of those develop contexts are built for one render and
  thrown away, but not all: the pinned second-window preview keeps its own develop
  context, history, modules and pipes until it is unpinned (`src/develop/develop.c`).
  What they have in common is the absent `gui_data`, not a short life. Several of them
  run the pipe synchronously from a GTK draw handler, so there the very same callbacks
  *do* run on the GTK main thread.

Hence the two-sided rule: `commit_params()` must never touch GTK, and must never wait
for the GTK main loop to run anything. Contending for `gui_lock` is not that — it is a
short mutex both sides release quickly, and taking it is the whole point of the next
section. A *synchronous round-trip* through the main loop is the thing that deadlocks,
because the GTK thread may itself be waiting on a pipe.

### Exceptions: Pipeline Callbacks That GTK-Thread Code Also Calls

> Four entries in the right column are also called directly from GTK-thread code, so they
> run on either thread and need the same care for a different reason:
> `distort_transform()` and `distort_backtransform()` (mask handling and darkroom zoom
> both call them), `blend_colorspace()` (the blend GUI calls it) and
> `default_colorspace()` (the color picker calls it while building a picker button,
> `src/gui/color_picker_proxy.c`). `distort_mask()` does *not* have that property despite
> the similar name — it is invoked only from pipeline code.
>
> Those two colorspace callbacks are called with `NULL` for both `pipe` and `piece` from
> the GTK side, so each has to tolerate that. `default_colorspace()` is also reached
> indirectly: if you leave `blend_colorspace()` at its default,
> `default_blend_colorspace()` forwards its two arguments straight through.

## Using `gui_data` from `commit_params()`

`commit_params()` is the one that surprises people. It reads like module setup code
that belongs to the GUI, but it is called while a pipe is being synchronized. The
three screen pipes each run on their own worker thread, yet synchronization holds
`dev->history_mutex` across the commit, so `commit_params()` does not run concurrently
with itself for one module instance. It can still overlap with `process()` on another
pipe and with any GTK-thread callback — that is what the lock is for.

Two preconditions, both mandatory:

- **The GUI may not exist.** `gui_data` is `NULL` in export and batch mode, and the GUI
  critical section — `dt_iop_gui_enter_critical_section()`, i.e. the module's `gui_lock`
  — is only initialized by `dt_iop_gui_init()`. Entering it without a GUI locks a mutex
  that was never set up.
- **Processing must not depend on the GUI cache.** The same `commit_params()` runs
  during export, where there is nothing to read. Whatever the cache saves for the
  darkroom, the non-GUI branch has to compute independently.

The shape:

```c
void commit_params(dt_iop_module_t *self, dt_iop_params_t *params, ...)
{
  dt_iop_mymodule_params_t *p = (dt_iop_mymodule_params_t *)params;
  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  ...
  if(self->dev->gui_attached && g)
  {
    // darkroom: refresh and reuse the GUI-side cache
    dt_iop_gui_enter_critical_section(self);
    if(g->smoothing != p->smoothing) g->interpolation_valid = FALSE;
    g->smoothing = p->smoothing;
    dt_iop_gui_leave_critical_section(self);

    _rebuild_lut(self, p);   // takes the lock itself, so call it outside the section
    ...
  }
  else
  {
    // export or headless: solve from scratch, with no cache to lean on
    _solve_from_scratch(p, ...);
    ...
  }
}
```

Two things to copy and one to watch. Copy the guard and the critical section around the
fields both sides touch. Copy the `else` branch too — the export path has to reach the
same processing result without the cache, and a module that quietly depends on the cache
exports differently from how it previewed. What to watch is the helper: pass it `p`, the
argument this commit was handed, and not `self->params`. Those are the same object in
the normal case and different on the pipe's defaults sync, which passes `default_params`
(see
[IOP_Module_API.md](IOP_Module_API.md#commit_params---transform-parameters-into-processing-data)).
And keep it outside the critical section
if it takes the lock itself — see
[The Lock Is Not Recursive](#the-lock-is-not-recursive).

`src/iop/toneequal.c`'s `commit_params()` is the in-tree instance of this split.

`g != NULL` is the whole guard, because a dev whose modules were loaded without a GUI
leaves `gui_data` NULL, and on the normal lifecycle the lock is ready by then:
`dt_iop_gui_init()` initializes `gui_lock` immediately before it calls `gui_init()`.

Treat that as the lifecycle, not as an invariant you can lean on.

> **In tree today:** `dt_iop_gui_init()` is the wrapper that creates the lock, and it is
> the wrapper that guarantees the pairing. A framework path that calls a module's
> `gui_init()` directly instead leaves `gui_data` allocated and `gui_lock` as it found
> it — and at least one history route in tree does exactly that today. Nothing in a
> module can detect it, and nothing you write in `commit_params()` fixes it. It is noted
> here only so that "`gui_data != NULL` means the lock is ready" is read as what normally
> happens rather than as something the framework promises.

**`g` is the test that holds; `self->dev->gui_attached` is the in-tree idiom.** Write
both, as the examples in this document do: it is what the tree writes, at about forty
places under `src/iop`, and it records the intent. But read only `g` as a test.

`gui_attached` adds nothing to `g` in current code. It is fixed while the `dt_develop_t`
is being built and never toggled afterwards. Most non-GUI contexts pass FALSE straight
to `dt_dev_init()` — export, thumbnailing, style application, mask objects. Exactly one
passes TRUE and overwrites it: the throw-away develop context `dt_dev_image()` renders
into, which clears the field before it loads a single module (`src/develop/develop.c`).
`darktable.develop` is given TRUE before darktable decides whether to start a GUI at all
(`src/common/darktable.c`) — so it is TRUE under `darktable-cli` too, and on its own it
is not even a test for "a GUI exists".

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

  // g->cached_xyz is also written by commit_params() on a pipe worker thread
  dt_iop_gui_enter_critical_section(self);
  if(g->cached_xyz != p->xyz) g->cache_valid = FALSE;
  g->cached_xyz = p->xyz;
  dt_iop_gui_leave_critical_section(self);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
```

If you are unsure whether a field crosses threads, search for every read and write of
it and check which callback each one sits in, using the table above.

**Asking for a reprocess is not synchronization.** A widget callback that writes a
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
Having a pipe worker thread compute the same value into `gui_data` so that the GUI can
read it back buys nothing and costs a data race.

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
  section on the pipe worker thread, and the `_show_computed()` idle callback reads it
  inside one on the GTK thread (`src/iop/exposure.c`).

The histogram *behind* that scalar is a second piece of shared state, and it is not
covered. Publishing a value safely does not make the state it was computed from safe;
each shared field needs its own answer.

> **In tree today:** `gui_update()` and `gui_changed()` free `g->deflicker_histogram` and
> rebuild it on the GTK thread, while `_process_common_setup()` reads that pointer and
> its statistics on a pipe worker thread — neither side takes `gui_lock`, and in
> `gui_update()` the free sits between two critical sections that guard other fields.
> That is the trap from
> [Hold the Lock as Long as the Value Must Stay Valid](#hold-the-lock-as-long-as-the-value-must-stay-valid),
> live in the tree: a heap buffer in `gui_data` that one thread can free while the other
> is walking it.

> **In tree today, until #21974 lands:** the manual-mode bullet above describes
> `exposure` *after* pull request #21974, not master as it stands. #21974 removes the
> cached `effective_exposure` field and makes the accessor derive its result. Until it
> lands, `src/iop/exposure.c` still caches:
> `commit_params()` writes `effective_exposure` from a pipe worker thread and
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

The example tests `g` and stops there, and stopping there is the point. A proxy accessor
takes whatever instance its caller hands it, and `self->dev` is not guaranteed: on the
throw-away instance darktable builds at startup so that widgets can register their
accelerators, `gui_data` is allocated and `dev` is NULL (see
[GUI.md](GUI.md#gui_init-overview)).

Reordering the guard does **not** help, and it is worth being explicit about why,
because the opposite is easy to assume. On that instance `gui_data` is allocated, so `g`
is non-NULL: `if(!g || !self->dev->gui_attached)` gets straight past `!g` and
dereferences the NULL `dev`, exactly as `if(self->dev->gui_attached && g)` would.
Short-circuit evaluation only protects the operand it skips, and here it skips neither.
What protects you is not reaching for `self->dev` at all — as the example above does —
or testing it on its own if you need something from it:

```c
  if(!g || !self->dev) return 0.0f;
```

Inside `process()` and `commit_params()` none of this arises, for the reason given in
[Using `gui_data` from `commit_params()`](#using-gui_data-from-commit_params): a module
reaches a pipe only through `dev->iop`, so it always has a `dev` there and either order
is safe. That is why the `commit_params()` example earlier in this document can write
`self->dev->gui_attached && g` without qualification.

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

## Passing Values Between Pipes Through `gui_data`

`gui_data` is also the channel one pipe uses to hand a value to another. A module that
needs a property of the *whole* image usually cannot get it on the full pipe, which
normally sees only the region of interest at the current zoom. The preview pipe does see
the whole image, at reduced size, so the module computes the value there, publishes it
into `gui_data`, and lets the full pipe pick it up.

The framework primitive for the handover is `dt_dev_sync_pixelpipe_hash()`
(`src/develop/develop.c`). The publisher stores the value together with the cumulative
hash of its own upstream pipe state. The consumer hands the primitive that hash cell and
its `gui_lock`, and the primitive waits until the stored hash matches what the
consumer's own upstream state hashes to.

Read that as evidence, not as proof. The primitive compares a hash it snapshotted
against one it computes afterwards, and returns as soon as the two agree; it does not
snapshot the payload, and it does not stop the publisher running again and replacing
payload and hash before the consumer gets round to reading them. The hash it compares is
also a hash of selected upstream `piece` values, not a full render identity — the cache
key is a different and larger thing. So a match makes a leftover from the previous
history state unlikely; it does not make it impossible.

A handful of modules use it, each passing `&self->gui_lock` as the lock argument so
that the primitive can read the publisher's hash safely while it waits.

Not every hash in `gui_data` means this is happening. A module may keep one purely to
memoize its own work, comparing it only against a hash it wrote itself on the same pipe,
and never waiting for anybody. A hash beside a payload is a freshness marker; it is a
handover only when one pipe waits on a hash another pipe wrote.

The consuming side:

```c
// on the consuming pipe
if(g && dt_pipe_is_full(piece->pipe))
{
  dt_iop_gui_enter_critical_section(self);
  const dt_hash_t hash = g->hash;      // has anything been published yet?
  dt_iop_gui_leave_critical_section(self);

  if(hash != DT_INVALID_HASH
     && !dt_dev_sync_pixelpipe_hash(self->dev, piece->pipe, self->iop_order,
                                    DT_DEV_TRANSFORM_DIR_BACK_INCL,
                                    &self->gui_lock, &g->hash))
    dt_control_log(_("inconsistent output"));

  dt_iop_gui_enter_critical_section(self);
  d->value = g->published_value;       // the payload, whatever it is
  dt_iop_gui_leave_critical_section(self);
}
```

and the publishing side, usually in the same function:

```c
// on the producing pipe
if(g && dt_pipe_is_preview(piece->pipe))
{
  const dt_hash_t hash = dt_dev_hash_plus(self->dev, piece->pipe, self->iop_order,
                                          DT_DEV_TRANSFORM_DIR_BACK_INCL);
  dt_iop_gui_enter_critical_section(self);
  g->published_value = d->value;   // both go in under one lock, so no reader can
  g->hash = hash;                  // catch the pair half-updated
  dt_iop_gui_leave_critical_section(self);
}
```

`src/iop/levels.c` carries both halves in one helper, `commit_params_late()`, called
from `process()` and `process_cl()`.

Four things about that shape do not follow from the rest of this document.

**`g != NULL` is not testing for a GUI here.** Nothing is being displayed; the test is
there because the channel itself lives in `gui_data` and so exists only when a GUI does.
The rule from [Using `gui_data` from `commit_params()`](#using-gui_data-from-commit_params)
applies unchanged — processing must not depend on the cache — and these modules obey it.
The in-tree consumers each recompute from scratch when they are on the producing pipe,
or when the published value is still at its uninitialized sentinel. Note what that
fallback does *not* cover: it triggers on a value that was never published, not on one
that arrived late. After a timed-out wait the consumer uses whatever is in `gui_data`.
Between that and the limits above, a module here has to be able to live with a stale
value as well as with none.

**`process()` may block here, and that is the supported idiom.** The wait is bounded by
the `pixelpipe_synchronization_timeout` preference — or by
`darktable.opencl->opencl_synchronization_timeout` on a GPU pipe — and it gives up early
once the pipe is flagged for shutdown. Setting that preference to zero or less switches
the wait off, and the primitive then reports success without checking anything. After a
real timeout it still reports success if the history stack has changed underneath, since
a reprocess is already on its way; it fails only when neither holds, and that failure is
the `inconsistent output` in the snippet above. This is not the wait ruled out in
[Which Thread Am I On?](#which-thread-am-i-on). What deadlocks is a synchronous
round-trip through the GTK main loop, because the GTK thread may itself be waiting on a
pipe. Waiting on another pipe is a different thing, and the framework supplies the
primitive for it.

**Do not hold `gui_lock` across the call.** The primitive takes the lock you hand it on
every polling iteration, so calling it from inside a critical section self-deadlocks on
the [non-recursive mutex](#the-lock-is-not-recursive). Snapshot what you need, release,
then call — as the consuming snippet above does. Underneath, the probe also takes
`dev->history_mutex`, because hashing the upstream state walks the pipe. The primitive
releases your `gui_lock` before it does that, so the two are never nested; keep it that
way on your side.

**A scalar hands over cleanly; an allocation does not.** When the payload is a plain
number, the span in which it must stay valid ends at the load and the short critical
section above is exactly right. When it is a heap object, copying the pointer out under
the lock buys nothing: only the pointer load is protected, and the publisher's next run
may have freed the object before you are done with it.

The hash handshake does not close that window. It establishes that the publisher has
*finished* a run matching your upstream state; it does not stop the next slider move
from starting another one that frees and replaces the object while you are still reading
it. A widget callback that resets the published state can free it too. This is
[Hold the Lock as Long as the Value Must Stay Valid](#hold-the-lock-as-long-as-the-value-must-stay-valid)
wearing an inter-pipe costume, so pick one of the three answers given there before you
publish an allocation.

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
locks. Framework helpers count — `dt_dev_sync_pixelpipe_hash()` in particular, which
takes the lock you hand it on every polling iteration. Keep critical sections short and
free of function calls where you can — that avoids the problem instead of reasoning about
it.

## Hold the Lock as Long as the Value Must Stay Valid

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

That rule covers a buffer being *replaced*. It does not cover one being **refilled in
place** while the flag beside it still says the old contents are good. Clear the flag
inside a critical section before the fill starts, and commit the new contents' hash and
the flag inside another one once the data is there; between the two the reader sees "not
valid" and stays away:

```c
  dt_iop_gui_enter_critical_section(self);
  g->buffer_valid = FALSE;              // reader now knows to stay away
  dt_iop_gui_leave_critical_section(self);

  _fill_buffer(g->buffer, ...);         // long, and deliberately outside the lock

  dt_iop_gui_enter_critical_section(self);
  g->hash = new_hash;                   // hash and flag committed together
  g->buffer_valid = TRUE;
  dt_iop_gui_leave_critical_section(self);
```

Without the first section there is a window in which a valid-flagged buffer holds half
of one image and half of another — a different failure from the size mismatch above, and
one that no amount of locking around the *read* will catch.

`src/iop/toneequal.c` is worth reading for the producer half of this, with one
difference. It clears the flag and fills outside the lock as above, but it commits the
hash and raises the flag in two separate critical sections rather than one. That is
still safe, and only because of the order: a reader that lands in the gap sees a fresh
hash with the flag still down and waits, never the reverse. Committing both in one
section, as shown here, is the version that does not need that argument made for it.

The flag only pays off if the reader honors it for as long as it uses the buffer.
Testing it under the lock, releasing, and *then* reading puts you back in the first
trap in this section: the writer can clear the flag and start refilling in between. Test
and use inside one critical section, or copy out what you need while you hold it.

## The Framework Service for Per-Pixel Readouts

Everything above is the hand-rolled version. For the most common instance of it — a
per-pixel value produced on the preview pipe and read on the GTK thread under the mouse
cursor — the framework already has it: `src/develop/preview_data.h`. Its file comment
says what it is for, and names the modules that used to duplicate it.

The service owns the buffer, the hash and the locking:

- `dt_preview_data_alloc()` in `gui_init()` and `dt_preview_data_free()` in
  `gui_cleanup()` are the bookends. These are the two entry points that do not take the
  lock — `_free()` leans on the same teardown guarantee `gui_cleanup()` does, scoped as
  in
  [The Callback Must Not Outlive the Module or the Image](#the-callback-must-not-outlive-the-module-or-the-image).
- `dt_preview_data_store()` is the write side: it sizes the buffer, fills it through a
  callback of yours and commits the hash.
- `dt_preview_data_resize()` and `dt_preview_data_set_hash()` are the two-step form of
  that, for a fill too expensive to hold the lock across.
- `dt_preview_data_get()` reads one component of one pixel, from the GTK thread, while
  the pipe may be writing.
- `dt_preview_data_is_fresh()` compares the stored hash against the module's piece in
  the current preview pipe and answers yes or no. `dt_preview_data_get_hash()` does not
  compare anything — it hands back the stored hash so you can do the comparison
  yourself. `dt_preview_data_invalidate()` marks the data stale without dropping the
  buffer.

The other seven take your module's `gui_lock` internally for the fields they own, so for
the service's own buffer and hash you do not have to take it yourself. What is awkward to
build by hand is the guarantee the header attaches to `dt_preview_data_store()`: resize,
fill and hash commit happen inside a *single* critical section, so the GUI can never
observe a resized but not-yet-filled buffer.

Note the limit of that. `gui_lock` serializes the service's fields against your module's
other users of the same lock. It does not stabilize anything outside them.

> **In tree today:** `dt_preview_data_is_fresh()` does reach outside them, walking the
> live preview pipe's node list to find your piece. Pipe topology is rebuilt under the
> pipe and history mutexes, not under `gui_lock`, so that walk needs the caller to be
> somewhere the topology is stable, and the service's own locking does not supply that.
> The same function also tests the buffer pointer in an early return, before it takes the
> lock at all — against a field the two write entry points free and replace while holding
> it.

The two-step form gives up that single-section guarantee, and hands you the piece you
need to replace it: if `dt_preview_data_resize()` has to resize, it calls a callback of
yours while still holding the lock, so you can drop your own validity flag atomically
with the resize — the refill-in-place discipline from
[Hold the Lock as Long as the Value Must Stay Valid](#hold-the-lock-as-long-as-the-value-must-stay-valid),
with the framework opening the critical section for you. In `src/iop/toneequal.c` that
callback is four lines long and clears one flag.

`toneequal` and `colorequal` use the service. What stays yours is what the header says
is module-specific: computing the value, drawing it, and mapping the cursor position to
a buffer pixel — that last one depends on which geometry modules sit after yours in the
pipe, so the service cannot do it for you.

This replaces the buffer, hash and lock bookkeeping for that one case. It does not
replace `gui_lock` for your module's other shared fields, and it is not a general
`gui_data` mutex — everything else in this document still applies to them.

## Guards Before Sending GUI Updates

Three tests belong here before you schedule a GUI update from `process()`; only the first
is unconditional.

```c
dt_iop_mymodule_gui_data_t *g = self->gui_data;

if(g != NULL                          // GUI exists (not export) — the test that holds
   && self->dev->gui_attached         // the in-tree idiom, not a second test
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

The middle test is the in-tree idiom, not a working guard — see
[Using `gui_data` from `commit_params()`](#using-gui_data-from-commit_params).

Reading `g` once is enough for the length of the run: on the four darkroom teardown
paths the framework holds the screen-pipe mutexes across module GUI teardown, so a
non-NULL `g` cannot be freed while your `process()` or `commit_params()` is executing.
What the guard does not cover is work you hand to the main loop, and the scope of that
teardown guarantee — both are in
[The Callback Must Not Outlive the Module or the Image](#the-callback-must-not-outlive-the-module-or-the-image).

## Pattern A: Critical Section + `g_idle_add`

Store computed values in `gui_data` under mutex, then schedule a GTK-thread callback.
This is the pattern to reach for first; Pattern B below is for the payload that cannot
live in `gui_data`.

`g` is `self->gui_data`, tested as in
[Guards Before Sending GUI Updates](#guards-before-sending-gui-updates) — that guard
block is required context for both patterns.

```c
// In process():
if(g != NULL && self->dev->gui_attached      // see "Guards Before Sending GUI Updates"
   && dt_pipe_is_full(piece->pipe))          // — and pick the pipe test to match
{                                            //   where your value is produced
  dt_iop_gui_enter_critical_section(self);
  g->computed_exposure = exposure;
  dt_iop_gui_leave_critical_section(self);
  g_idle_add(_show_computed, self);
}

// Callback (GTK main thread):
static gboolean _show_computed(gpointer user_data)
{
  dt_iop_module_t *self = user_data;
  dt_iop_mymodule_gui_data_t *g = self->gui_data;
  // fallback check only — see "The Callback Must Not Outlive the Module or the Image"
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
harder, and the bookkeeping that fixes it is more than the critical section this pattern
set out to avoid. **Prefer Pattern A unless the payload genuinely cannot live in
`gui_data`**; the reasoning is at the end of
[The Callback Must Not Outlive the Module or the Image](#the-callback-must-not-outlive-the-module-or-the-image).

The snippet below shows the dispatch and the callback only. It is **not** complete: the
source-id bookkeeping that makes it cancellable is described in that section, and has to
be added.

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
  // fallback check only — see "The Callback Must Not Outlive the Module or the Image"
  if(!g)
  {
    g_free(msg);
    return G_SOURCE_REMOVE;
  }

  memcpy(g->display_values, msg->values, sizeof(g->display_values));
  gtk_widget_queue_draw(g->area);   // reach the widget through g, not through msg->self

  g_free(msg);  // Callback owns the message
  return G_SOURCE_REMOVE;
}

// At end of process():
if(g != NULL && self->dev->gui_attached      // see "Guards Before Sending GUI Updates"
   && dt_pipe_is_full(piece->pipe))
{
  mymodule_gui_msg_t *msg = g_malloc(sizeof(*msg));
  msg->self = self;
  memcpy(msg->values, local_values, sizeof(msg->values));
  // keyed on msg, so it cannot be cancelled by data — see the next section
  g_idle_add(_update_gui, msg);
}
```

Two details in that callback are not decoration. The NULL check earns more here than it
does in Pattern A: a source keyed on the message cannot be cancelled with
`g_idle_remove_by_data()`, so until you add the source-id bookkeeping described in the
next section it is the only thing standing between a deleted instance — the one teardown
shape that keeps the module struct and frees only its GUI — and a NULL dereference. On
the shapes that free the struct as well it is already too late, which is the whole of
why it is a fallback check and not a guard.

And every field the callback touches is reached through `g`, so that one check covers
the whole body — including the widget. Taking the widget from `msg->self->widget`
instead would leave a dereference the check does not guard:
`dt_iop_gui_cleanup_module()` destroys that widget and sets the field to NULL, right
beside where it frees `gui_data`, with the in-source note that it does so because
asynchronous work can still be carrying the module (`src/develop/imageop.c`). Keep your
widget pointers in `gui_data` and the guard stays honest.

## The Callback Must Not Outlive the Module or the Image

The framework guarantees one thing here: **on all four darkroom teardown paths, no module
GUI is torn down while a pipe is running.** Those four sites — leaving the darkroom,
switching image, deleting an instance, and undoing or redoing a module add or delete —
hold all three screen-pipe mutexes across `dt_iop_gui_cleanup_module()`. Deleting an
instance and undo/redo take them through the `dt_dev_pixelpipe_stop_and_lock_all()` /
`dt_dev_pixelpipe_unlock_all()` pair (`src/develop/develop.c`), which also flags the
pipes for shutdown; darkroom exit and the image switch lock the same three mutexes
directly (`src/views/darkroom.c`), the image switch with `trylock` and a re-queue for
as long as a pipe is busy. One consequence for module authors: on those four paths
`gui_cleanup()` runs with all three screen-pipe mutexes held, so it must not call
anything that waits on a pipe. Those four are not the only callers, though — the startup accelerator
probe tears its throw-away instance down with no pipe lock held at all
(`src/develop/imageop.c`; on that startup instance `dev` is NULL as well, see
[GUI.md](GUI.md#gui_init-overview)). So take the mutexes as a constraint on what
`gui_cleanup()` may do, never as a guarantee it can lean on.

That closes the window against pipe worker threads. It does not close the one you open
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

The `if(!self->gui_data) return G_SOURCE_REMOVE;` at the top of both pattern callbacks
does not cover all three; what it does on each is worked through
[below](#what-the-gui_data-check-actually-does). Cancel the source instead.

Cancel in `gui_cleanup()`. If the source data is `self`, that is one line:

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
exactly this kind of reset, and it runs while the three screen-pipe mutexes are still
held, so no pipe worker thread can queue a source behind the drain. Clearing GUI state is what
the modules implementing it use it for; they are listed in
[IOP_Module_API.md](IOP_Module_API.md#change_image---reset-gui-state-for-the-new-image):

```c
void change_image(dt_iop_module_t *self)
{
  while(g_idle_remove_by_data(self)) ;
  ...   // reset the rest of gui_data for the new image
}
```

If the queued value can simply be recomputed, cancelling is the whole fix: the new
image's first pipe run queues a fresh update.

The loop is easy to drop, because a single call looks like it cancels. This would be
wrong:

```c
void gui_cleanup(dt_iop_module_t *self)
{
  ...
  g_idle_remove_by_data(self);   // removes ONE source, and returns whether it did
}
```

`process()` can queue a source on every qualifying run, so several can be outstanding
when the GUI is torn down, and one call is not guaranteed to remove them all.
`src/bauhaus/bauhaus.c` drains in a `while` loop and is the model for that line.

**Why the loop is not a detail.** Every one of these transitions holds the screen-pipe
mutexes before it touches a module GUI, so nothing can queue a fresh source once
`gui_cleanup()` — or `change_image()` — has started. Draining there is therefore
sufficient, as long as it actually drains.

If the source data is a heap message (Pattern B), `g_idle_remove_by_data()` cannot find
it — the source is keyed on the message, not on the module. You then have to track the
source ids yourself: keep the id in `gui_data`, and queue with `g_idle_add_full()`
passing `g_free` as the `GDestroyNotify`. Drop the `g_free(msg)` from the callback if
you do — the notification runs after a normal dispatch as well as on cancellation, so
keeping both frees the message twice. A second update also has to supersede the first
rather than overwrite its id unremoved. That is more bookkeeping than the critical
section Pattern B set out to avoid, so prefer Pattern A unless the payload genuinely
cannot live in `gui_data`.

### What the `gui_data` Check Actually Does

`dt_iop_gui_cleanup_module()` sets `module->gui_data` to NULL after freeing it, and
teardown and idle dispatch both run on the GTK main thread, so they cannot interleave.
That is enough to make `if(!self->gui_data) return G_SOURCE_REMOVE;` meaningful on one
of the three shapes and not on the other two:

- For a **deleted instance** it is a real guard. The module struct is still there,
  parked in `dev->alliop`, and `gui_data` has been cleared, so the check sees NULL and
  returns; without it the callback dereferences NULL and the process stops there.
- On **darkroom exit**, and for an **extra instance on an image switch**, the struct
  itself is freed right after its GUI, so reading `self->gui_data` *is* the
  use-after-free. The check comes too late whatever it returns — and what it returns is
  undefined: the freed bytes may still hold the NULL that cleanup wrote, or anything the
  allocator has since put there.
- For a **base instance on an image switch** the check is blind rather than late.
  Nothing is freed, `gui_data` is still allocated, so the test passes and the callback
  runs to completion against the wrong image.

Where the check does work it has to come first, ahead of
`dt_iop_gui_enter_critical_section()`, which would otherwise lock a destroyed mutex.
Treat it as a fallback check, never as a substitute for cancelling: only the drain covers
all three shapes.

## Common Mistakes

Each row is one WRONG line and the section that explains it.

| Mistake | WRONG | Explained in |
| --- | --- | --- |
| GTK call from `process()`, directly or through a helper | `gtk_label_set_text(g->label, "value");` | [Which Thread Am I On?](#which-thread-am-i-on) |
| Assuming `commit_params()` runs on the GTK thread | `void commit_params(...) { gtk_widget_queue_draw(g->area); }` | [Which Thread Am I On?](#which-thread-am-i-on) |
| No critical section when the pipe writes `gui_data` | `g->computed_value = result;` in `process()` | [Using `gui_data` from `commit_params()`](#using-gui_data-from-commit_params) |
| Entering the critical section without checking that the GUI exists | `dt_iop_gui_enter_critical_section(self);` in `commit_params()`, unguarded | [Using `gui_data` from `commit_params()`](#using-gui_data-from-commit_params) |
| No critical section in a widget callback either, when the pipe reads the field | `g->cache_valid = FALSE;` in a slider callback | [Writing `gui_data` from a Widget Callback](#writing-gui_data-from-a-widget-callback) |
| Treating a reprocess request as a barrier | `dt_dev_reprocess_center(self->dev, self->iop_order);` after writing a shared field | [Writing `gui_data` from a Widget Callback](#writing-gui_data-from-a-widget-callback) |
| Calling a locking helper from inside a critical section | `_update_cache(self);` between enter and leave | [The Lock Is Not Recursive](#the-lock-is-not-recursive) |
| Locking the pointer load and not the pointee | `my_cache_t *c = g->cache;` under the lock, `use_cache(c);` after it | [Hold the Lock as Long as the Value Must Stay Valid](#hold-the-lock-as-long-as-the-value-must-stay-valid) |
| No pipe test at all, so every pipe queues its own update | `if(g != NULL) g_idle_add(...);` | [Guards Before Sending GUI Updates](#guards-before-sending-gui-updates) |
| Forgetting to free the Pattern B message — or freeing it twice | `return G_SOURCE_REMOVE;` with no `g_free(data)`, or one alongside a `g_free` `GDestroyNotify` | [The Callback Must Not Outlive the Module or the Image](#the-callback-must-not-outlive-the-module-or-the-image) |
| Queued callback with no cancellation | `g_idle_add(_update_gui, self);` with no drain in `gui_cleanup()` or `change_image()` | [The Callback Must Not Outlive the Module or the Image](#the-callback-must-not-outlive-the-module-or-the-image) |
