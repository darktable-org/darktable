# Module Lifecycle: What Happens When the User Interacts

[IOP_Module_API.md](IOP_Module_API.md) documents the callback API: what each function signature
means and what a module must implement. This document is its companion. It describes *when*
those callbacks fire and *what else* the system does around them: pipe change flags, cache
invalidation, history replay, and cross-module data flow.

After reading the API doc you can implement a module. After reading this one you should also
have a mental model of what darktable does when the user loads an image, drags a slider,
toggles a module, resets it, or undoes an edit.

References point to code by **file, function, and the relevant block** (an `if`, `for`, or
`while`), and use short snippets where that is clearer. They avoid line numbers, which drift
as the source changes.

---

## 1. Background concepts

These are the moving parts the later sections rely on. Each one gets a short definition, not
a full reference. [Core_Concepts.md](Core_Concepts.md) introduces the objects they act on:
dev, module instance, pipe, and piece.

### History stack (`dt_dev_history_item_t`)

An ordered list of per-module parameter snapshots on the dev
([`dt_develop_t`](pixelpipe_architecture.md#dt_develop_t)). `dev->history_end` is the active
cursor: only the entries in the range `[0, history_end)` are "live". Note that a history item's
**`enabled` flag is stored separately from `params`**: it is not part of the params buffer.

### Pipe types

The darkroom dev has three pixelpipes. They run the same module chain independently, at
different resolutions:

- **full**: the main darkroom image
- **preview**: the low-resolution image used for the navigation thumbnail and the histogram
- **preview2**: the optional second window

Each is marked dirty and re-run on its own, often in parallel on separate threads. "Mark all
pipes dirty" below means all of the pipes that exist for the dev.

### Pipe change flags

These are set on `pipe->changed`. They tell `dt_dev_pixelpipe_change()` (pixelpipe_hb.c)
which resync strategy to use when the pipeline next runs. The constants are defined in
pixelpipe_hb.h.

| Flag | Meaning | Strategy |
|---|---|---|
| `DT_DEV_PIPE_TOP_CHANGED` | Only the topmost history item changed | `synch_top`: re-commit that one module; the other pieces are not touched |
| `DT_DEV_PIPE_SYNCH` | All items need a param resync, topology unchanged | `synch_all`: commit every piece's defaults, then every active history item in history order |
| `DT_DEV_PIPE_REMOVE` | A module was added or removed; topology must be rebuilt | full `cleanup_nodes` + `create_nodes` + `synch_all` |
| `DT_DEV_PIPE_ZOOMED` | Zoom or pan only | no param sync; only the ROIs change |

`synch_all` re-commits every module, but a module whose hash did not change still produces a
cache hit, so its `process()` is skipped. In other words, the flag controls *committing*, not
*reprocessing*. Reprocessing is decided per module by the cache (see
[section 6](#6-pipeline-execution-detail)).

### Pipe status

`pipe->status` is one of `DT_DEV_PIXELPIPE_DIRTY` (needs reprocessing), `RUNNING` (a thread is
processing it now), `VALID` (finished with a valid result) or `INVALID` (finished with an
unusable result).

### `focus_hash`

Despite its name, `dev->focus_hash` is a flag. `dt_iop_request_focus()` sets it to `TRUE` when
the user moves focus to a different module (opens its panel or clicks into one of its widgets),
and `dt_dev_reload_history_items()` sets it back to `FALSE`. Each new history item stores the
value it had when the item was created.

`_dev_add_history_item_ext()` (develop.c) compares the two. Editing the module that is already
at the top of the history normally updates that item in place, but when `dev->focus_hash`
differs from the item's stored value and the params differ, it pushes a new item instead:

```c
|| ((dev->focus_hash != hist->focus_hash)                 // or if focused out and in
    // but only add item if there is a difference at all for the same module
    && (... memcmp(hist->params, module->params, module->params_size)))
```

A new item takes the `SYNCH` path instead of the cheaper `TOP_CHANGED` path. If you write
deferred or batched parameter changes, keep this in mind, because it changes how much of the
pipe re-commits.

### Pixelpipe cache

The cache is hash-based with lazy invalidation. The key for a module's output is the
**cumulative hash** of every upstream `piece->hash` value (each computed in
`dt_iop_commit_params()` from the op name, instance, params, blend params and mask), combined
with the image id, the color-profile information and the requested ROI. It is *not* a hash of
one module's data on its own. `_dev_pixelpipe_cache_basichash()` in pixelpipe_cache.c walks all
enabled modules up to the requested position.

When the key matches a cached entry, the output is reused and `process()` is skipped. Stale
entries are not actively deleted on a param change; they simply stop being looked up, and the
fixed-size LRU evicts them later. For the full caching model see
[pixelpipe_architecture.md](pixelpipe_architecture.md#pipeline-caching).

### `module->params` vs `module->default_params`

`default_params` is **image-specific**: it is set by `reload_defaults()` (see
[IOP_Module_API.md](IOP_Module_API.md#reload_defaults---per-image-defaults)). `params` is the
**live editing state**. `dt_dev_pop_history_items_ext()` resets every module to its
`default_params` and then replays the history entries on top. As a result, a module with no
history entry runs with its image-specific defaults, not with compile-time constants.

---

## 2. Image loading

A pristine (never edited) image and one with saved history both go through the **same**
`dt_dev_load_image()` -> `dt_dev_read_history_ext()` chain. The "no history" versus "has
history" distinction is handled **inside** `dt_dev_read_history_ext()`, not by different
callers.

### Load sequence (`dt_dev_load_image()`, develop.c)

1. **`_dt_dev_load_raw()`** triggers the raw decode through the mipmap cache (this blocks),
   then reads the decoded image struct into `dev->image_storage`. The large mipmap buffer is
   released right after the decode; `image_storage` keeps the metadata. (The *mipmap cache*
   holds pre-scaled copies of each image at several resolutions, used for fast thumbnail and
   preview generation.)
2. Mark all pipes `DIRTY` and set `loading = TRUE`.
3. **`dt_iop_load_modules()`** builds the IOP module list (`dev->iop`).
4. **`dt_dev_read_history_ext()`** loads defaults and presets, and builds `dev->history`.
   See below.

`dt_dev_load_image()` itself does not load defaults. That happens inside
`dt_dev_read_history_ext()`. Neither function copies the history into `module->params`: the
caller does that afterwards (see [After loading](#after-loading-replay-into-the-modules)).

### Inside `dt_dev_read_history_ext()` (develop.c)

This function applies saved history by **building `dev->history` directly from the
database**: it allocates one history item per stored row and appends it to the list. (This is
a different mechanism from the reset-then-replay path that undo/redo uses later; that one is
described in [section 3f](#3f-undo-redo-and-history-panel-navigation).)

**For every image** (the `if(!no_image)` block, which runs whether or not the image has saved
history):

- Clear the scratch `memory.history` table.
- **`dt_dev_reset_chroma()`** resets part of the shared white balance state, `dev->chroma`
  (see [section 5](#5-pipeline-ordering-asymmetry) for why).
- **`_dt_dev_load_pipeline_defaults()`** calls `dt_iop_reload_defaults()` on every module
  instance already in `dev->iop`, in **reverse** pipe order
  ([section 5](#5-pipeline-ordering-asymmetry) explains the consequences). This sets each
  module's image-specific `default_params`. Because it goes through the wrapper, it also
  copies them into `params` (via `dt_iop_load_default_params()`).
  Some modules also write shared state here: white balance stores the camera's reference
  coefficients in `dev->chroma`.
- **`_dev_add_default_modules()`** prepends the workflow-mandated modules into the in-memory
  history.
- **`_dev_auto_apply_presets()`** applies auto-presets. It is gated on the
  `DT_IMAGE_AUTO_PRESETS_APPLIED` image flag, **not** on `change_timestamp == -1` (that test
  guards only the separate legacy pre-3.0 white balance recovery branch). It is a no-op for an
  image whose presets were already applied, so only a first import actually gains preset
  entries here.
- **`_dev_merge_history()`** merges the in-memory default and preset history into
  `main.history`, so the database read below picks it up.

**Then, for all images,** the same database-read loop runs. For each `main.history` row it
allocates a `hist` item, copies the params (falling back to the module's `default_params`
when the row has none, which is how auto-applied presets are stored), sets `hist->enabled`,
appends to `dev->history`, and increments `history_end`:

```c
if(param_length == 0)
  memcpy(hist->params, hist->module->default_params, hist->module->params_size);
...
dev->history = g_list_append(dev->history, hist);
dev->history_end++;
```

Modules with no database row keep the image-specific `default_params` set earlier by
`_dt_dev_load_pipeline_defaults()`.

A row can name an instance that is not in `dev->iop` yet, for example a second exposure
instance (`multi_priority > 0`). The loop then creates it with `dt_iop_load_module()`
(imageop.c), which calls `init()` but **not** `reload_defaults()`. The new instance has the
`default_params` set by `init()`, not image-specific ones, and a row without params falls
back to them. Whether `reload_defaults()` runs for it later depends on the caller: see
[After loading](#after-loading-replay-into-the-modules).

After the loop, in the `none` workflow, if both white balance (`temperature`) and color
calibration (`channelmixerrgb`) are enabled in the history, the function calls
`temperature->reload_defaults(temperature)` **directly**. This bypasses the wrapper, so
`params` is only partially updated; see the `reload_defaults()` caveat in
[IOP_Module_API.md](IOP_Module_API.md#reload_defaults---per-image-defaults).

The function then re-reads `history_end` from `main.images`, and, when the GUI is attached,
calls `dt_dev_pipe_synch_all()` and `dt_dev_invalidate_all()` so the pipes will commit and
reprocess. The pipes stay `DIRTY`, and the pipeline threads run on the next redraw
([section 6](#6-pipeline-execution-detail)).

### After loading: replay into the modules

`dt_dev_read_history_ext()` fills `dev->history`, but it does not copy the history params into
`module->params`, and it does not update any widget. In darkroom, the caller does this with
`dt_dev_pop_history_items(dev, dev->history_end)`, after the module GUIs exist. That call
resets every module to `default_params`, replays the active history entries into
`module->params`, calls `gui_update()` on every module, and schedules pipe synchronization
(see [section 3f](#3f-undo-redo-and-history-panel-navigation)). Only after this call are
`module->params` and the widgets current for the new image.

The two darkroom paths differ in what runs before that call (both in views/darkroom.c):

- **Entering darkroom** (`enter()`): calls `dt_dev_load_image()`, then, for every visible
  module instance, `dt_iop_gui_init()` followed by `dt_iop_reload_defaults()`, then
  `dt_dev_pop_history_items()`. Instances created from the history get their image-specific
  defaults here.
- **Switching image** (`_dev_load_requested_image()`, queued by `_dev_change_image()`): does
  not call `dt_dev_load_image()`. It keeps each module's base instance and calls
  `dt_iop_reload_defaults()` on it, destroys the other instances, then calls
  `dt_dev_read_history()`. For each instance that the history creates, it calls
  `dt_iop_gui_init()` but **not** `dt_iop_reload_defaults()`; for each base instance it calls
  `change_image()`. Then it calls `dt_dev_pop_history_items()`.

So a module cannot assume that `reload_defaults()`, or any side effect in it, has run for an
instance created from the history.

**Headless callers** call `dt_dev_load_image()`, and not all of them replay. Export in
imageio.c, for example, calls `dt_dev_pop_history_items_ext()` only when it needs a history
end different from the stored one. The pipe does not read `module->params`:
`dt_dev_pixelpipe_synch_all()` commits each module's `default_params` and then the
`hist->params` of the history entries up to `history_end`, so the pipe output is correct
without a replay into the modules.

---

## 3. Editing operations

The edits in 3a-3d end in `_dev_add_history_item_ext()` (develop.c), which chooses between two
branches:

- **update in place** (`TOP_CHANGED`): the module is already at the top of the history, and
  the `focus_hash` test above does not ask for a new item. The item's params and `enabled`
  flag are overwritten and `pipe->changed |= DT_DEV_PIPE_TOP_CHANGED`.
- **new item** (`SYNCH`): anything else, such as a different module or instance. A new history
  entry is pushed and `pipe->changed |= DT_DEV_PIPE_SYNCH`.

Removing an instance (3e) and undo, redo or history navigation (3f) change the history
without this function.

### 3a. Adjust a slider: the common `TOP_CHANGED` path

1. For a widget created with a `_from_params` helper, the bauhaus value-change handler
   (`bauhaus.c`) writes the new value into `self->params` and calls
   `dt_iop_gui_changed(module, widget, &prev)`. A manually connected widget calls
   `dt_dev_add_history_item()` from its own callback instead.
2. `dt_iop_gui_changed()` runs the module's `gui_changed()` (module-specific; it may update
   dependent widgets), then `dt_dev_add_history_item_target()`.
3. `_dev_add_history_item_ext()` chooses one of the two branches above.
4. `dt_dev_invalidate_all()` marks all pipes `DIRTY` and increments `dev->timestamp`.
5. `DT_SIGNAL_DEVELOP_HISTORY_CHANGE` is emitted **only when `need_end_record` is true**, not
   on every slider tick. It is tied to the undo-record start/end pairing, so one logical edit
   emits the signal once.
6. On the pipeline thread, `dt_dev_pixelpipe_change()` dispatches on the flag:
   - `TOP_CHANGED` -> `dt_dev_pixelpipe_synch_top()`: runs `commit_params()` for that one
     module only.
   - `SYNCH` -> `dt_dev_pixelpipe_synch_all()`: commits every piece's defaults, then every
     active history item in history order. Unchanged hashes still hit the cache.
7. The pipeline runs: modules with unchanged hashes reuse their cached output; the changed
   module and everything downstream of it reprocess.

### 3b. Enable / disable a module

1. The toggle button fires its `toggled` signal, which reaches `_gui_off_callback()`
   (imageop.c).
2. The callback sets `module->enabled` and calls `dt_dev_add_history_item()`. The
   `hist->enabled` flag is recorded **separately from params**.
3. If the module is already at the top of the history, its item is updated in place
   (`TOP_CHANGED`); otherwise a new item is pushed (`SYNCH`).

### 3c. Reset to defaults

A click on the reset button in the module header reaches `_gui_reset_clicked()` (imageop.c).
A shortcut reaches `_gui_reset_callback()`, which has the same logic. A plain reset does this:

1. If the module has a drawn mask, `dt_masks_form_remove()` (masks.c) removes it. This
   changes the history before the reset itself: it clears the module's `mask_id` and calls
   `dt_dev_add_history_item()`, then calls `dt_dev_add_masks_history_item()`, which stores
   the new mask list in the history and sets `SYNCH` on all pipes.
   This can change other modules too. The "add existing shape" menu of a drawn mask also
   lists the mask groups of other modules, so the group of module B can contain the group
   of module A. A reset of A removes A's group from B's group. If B's group becomes empty,
   it is removed as well: B's `mask_id` is cleared and B gets its own history item.
2. `dt_iop_reload_defaults(module)` recomputes the image-specific `default_params` and copies
   them into `module->params` (this is the wrapper path, so the copy does happen). The blend
   params are reset to `default_blendop_params`.
3. `dt_iop_gui_reset(module)` calls the module's `gui_reset()`, if it has one, and
   `dt_iop_gui_update(module)` syncs the widgets to the new params.
4. `dt_dev_add_history_item(module->dev, module, TRUE)` adds or updates a **single** history
   entry, with the same `TOP_CHANGED` / `SYNCH` choice as any other edit. It does not rebuild
   the whole stack. Without a drawn mask, this is the only history change of the reset. With
   a drawn mask, step 1 has already set `SYNCH`, so the pipe runs
   `dt_dev_pixelpipe_synch_all()` even if this step updates the item in place.

**Ctrl+reset** first calls `dt_gui_presets_autoapply_for_module()` (gui/presets.c). If an
auto-apply preset matches the image, or the module has a built-in preset for the current
workflow (such as "scene-referred default"), that preset is applied instead of steps 1-4:
`dt_gui_presets_apply_preset()` copies the preset's params, enabled flag and blend params into
the module, updates the GUI, and calls `dt_dev_add_history_item(..., FALSE)`. If no preset
matches, a plain reset is done.

### 3d. Add a module instance (+ button)

1. `dt_iop_gui_duplicate()` (imageop.c) starts the duplication.
2. It records the base module's state: `dt_dev_add_history_item(base->dev, base, FALSE)`.
3. `dt_dev_module_duplicate()` creates the new instance in `dev->iop`.
4. `dt_iop_reload_defaults(new_module)` sets the image-specific defaults for the new instance.
5. `dt_dev_add_history_item(new_module->dev, new_module, TRUE)` adds a new history entry.
6. `dt_dev_pixelpipe_rebuild()` sets `pipe->changed |= DT_DEV_PIPE_REMOVE`, which forces a
   full topology rebuild: the node graph changed, so it is rebuilt from scratch.

### 3e. Remove a module instance (- button)

`dt_dev_module_remove()` (develop.c) does the teardown. Inside its `if(dev->gui_attached)`
block (true in the interactive darkroom) it **prunes the history**. It walks `dev->history`
and, for each entry whose module is the one being removed, frees the item, unlinks it, and
decrements `history_end`:

```c
if(module == hist->module)
{
  dt_dev_free_history_item(hist);
  dev->history = g_list_delete_link(dev->history, elem);
  dev->history_end--;
}
```

It then removes the module from `dev->iop`, and the pipes are marked for a topology rebuild
(`REMOVE`: `cleanup_nodes` + `create_nodes`).

So a removed instance's history entries are **actively deleted**, not merely skipped. In a
headless context, where `gui_attached` is false, this pruning block does not run, but
interactive removal is the case this document covers.

### 3f. Undo, redo, and history-panel navigation

During an active editing session these are the main way `module->params` are rewritten
from saved entries. Both end in `dt_dev_pop_history_items()`, the same call that finishes a
darkroom image load ([After loading](#after-loading-replay-into-the-modules)). It calls
`dt_dev_pop_history_items_ext()` to reset all modules to their `default_params` and replay the
history up to the given position, updates every module's GUI, then calls
`dt_dev_pipe_synch_all()` (or `dt_dev_pixelpipe_rebuild()` when the module order changed) and
`dt_dev_invalidate_all()`.

**Clicking an entry in the history panel** (`libs/history.c`) calls
`dt_dev_pop_history_items(dev, num)` directly, with the clicked position.

**Undo and redo** (`_pop_undo()` in `libs/history.c`) swap in the saved history list and
`history_end`, mark the pipes for a rebuild, write the history to the database, and call
`dt_dev_reload_history_items()` (develop.c), which:

1. clears `dev->focus_hash`,
2. calls `dt_dev_pop_history_items(dev, 0)` to reset all modules to their `default_params`,
3. drops the history items beyond `history_end`, and re-reads the history from the database,
4. calls `dt_dev_pop_history_items(dev, history_end)` to replay up to the target position.

---

## 4. Cross-module interactions

### 4a. White balance -> color calibration (through `dev->chroma`)

White balance (`temperature.c`) and color calibration (`channelmixerrgb.c`) share state in
`dev->chroma`; there is no signal. White balance writes the camera's reference coefficients in
`reload_defaults()` and the coefficients it applies in `commit_params()`; color calibration
reads them in `reload_defaults()`, `commit_params()` and `process()`.

Commit order does not guarantee that white balance commits first: `synch_all` replays the
history in history order, and `synch_top` re-commits only the top item. Color calibration
therefore recomputes its camera illuminant (`DT_ILLUMINANT_CAMERA`) in `process()`, which runs
after all the pipe's commits are done.

### 4b. Color-profile change (colorin / colorout)

This follows the same flow as a slider adjust: `TOP_CHANGED` or `SYNCH`, depending on the
history and `focus_hash`. There is no `REMOVE`, because the topology does not change.
`commit_params()` rebuilds the color transforms; no cross-module signal is needed. Because the
transform changes the pixels, all downstream cache hashes change and everything downstream
reprocesses.

---

## 5. Pipeline ordering asymmetry

Three operations walk the modules in **different** orders, which is a common source of
confusion. (See also the
[ordering-asymmetry note](pixelpipe_architecture.md#pipeline-ordering-asymmetry) in
pixelpipe_architecture.md.)

- **Processing** runs in pipe order: an upstream module's `process()` finishes before a
  downstream one starts.
- **`commit_params()` (through `synch_all`)** first commits every piece's defaults in pipe
  order, then replays the history items in **history order**. History order is the order of
  the user's edits, not pipe order, so a downstream module can commit before an upstream one.
- **`_dt_dev_load_pipeline_defaults()`** iterates in **reverse** pipe order (last module
  first):

  ```c
  for(const GList *modules = g_list_last(dev->iop);
      modules;
      modules = g_list_previous(modules))
  {
    dt_iop_reload_defaults(modules->data);
  }
  ```

  So a downstream module's `reload_defaults()` runs **before** an upstream one's. The source
  documents no rationale for the direction.

### Developer rule

Neither `reload_defaults()` nor `commit_params()` can assume that earlier-in-pipe modules have
already written shared state for the current image or edit. Only `process()` runs in pipe
order.

Shared state that a `reload_defaults()` depends on must therefore be **reset to a neutral
value before the reverse default-load**. The framework does this for white balance: it calls
`dt_dev_reset_chroma()` just before `_dt_dev_load_pipeline_defaults()`. The neutral white
balance coefficients make color calibration compute its default illuminant from the image's own
as-shot coefficients, instead of from values left by the previous image.

---

## 6. Pipeline execution detail

How pixels actually flow once the pipes are marked dirty:

- `dt_dev_process_image_job()` (develop.c) is the entry point. It locks the pipe and records
  `dev->timestamp` as the pipe's input timestamp.
- `dt_dev_pixelpipe_process()` calls `_dev_pixelpipe_process_rec()` (pixelpipe_hb.c), which
  recurses from the last module back toward the input, pulling each module's input on demand.
- For each module, the cache key is the **cumulative hash** described in
  [section 1](#pixelpipe-cache). On a key match, the cached output is reused and `process()` is
  skipped. On a miss, `process()` (or `process_cl()` on an OpenCL device) runs and the result
  is stored.
- `modify_roi_in()` and `modify_roi_out()` run on every module during the ROI-propagation
  phase, before processing, translating between the output-side and input-side regions. Under
  the `ZOOMED` flag only the ROIs change, not the module hashes. The ROI is part of the cache
  key, so a module gets a cache hit only for a region it has already produced, for example
  after zooming back.
- The cache is a fixed-size LRU, and eviction is automatic. There is no explicit invalidation
  on a param change; a changed hash simply misses.
- After all modules finish, `pipe->status` becomes `VALID` and a finished signal (for example
  `DT_SIGNAL_DEVELOP_PREVIEW_PIPE_FINISHED`) triggers the GTK redraw.
- The full and preview pipes run identical logic at different input resolutions and can run in
  parallel on separate threads.

---

## Key symbols

Grouped by file. Look these up by name; the surrounding block is described in the relevant
section above.

**develop.c**

| Symbol | Role |
|---|---|
| `dt_dev_load_image` | Top-level image load |
| `dt_dev_read_history_ext` | Loads defaults/presets and builds `dev->history` from the DB |
| `_dt_dev_load_pipeline_defaults` | Calls `reload_defaults` on the instances already in `dev->iop`, in reverse pipe order |
| `dt_dev_reset_chroma` | Resets shared white balance state before the reverse default-load |
| `dt_dev_add_history_item` / `_dev_add_history_item_ext` | Add/update a history entry; choose `TOP_CHANGED` vs `SYNCH` (the `focus_hash` test lives here) |
| `dt_dev_pop_history_items` / `_ext` | Reset to defaults, then replay history up to a position |
| `dt_dev_reload_history_items` | Undo/redo: reset, re-read the history, replay |
| `dt_dev_module_remove` | Removes a module instance and prunes its history entries |
| `dt_dev_invalidate_all` | Marks all pipes dirty, bumps `dev->timestamp` |
| `_dev_auto_apply_presets` | Auto-presets; gated on `DT_IMAGE_AUTO_PRESETS_APPLIED` |
| `dt_dev_process_image_job` | Pixel-processing entry point |

**imageop.c**

| Symbol | Role |
|---|---|
| `dt_iop_load_module` | Creates a module instance; calls `init()`, not `reload_defaults()` |
| `dt_iop_reload_defaults` | Wrapper: calls the module's `reload_defaults()` then `dt_iop_load_default_params()` |
| `dt_iop_load_default_params` | Copies `default_params` into `params` |
| `dt_iop_commit_params` | Translates params into `piece->data`; sets `piece->hash` |
| `dt_iop_gui_changed` | Entry point for `_from_params` widget changes |
| `dt_iop_request_focus` | Changes the focused module; sets `dev->focus_hash` |
| `_gui_off_callback` | Enable/disable toggle handler |
| `_gui_reset_clicked` | Reset button handler (`_gui_reset_callback` for shortcuts) |
| `dt_iop_gui_duplicate` | Add a module instance |

**pixelpipe_hb.c / .h**

| Symbol | Role |
|---|---|
| `DT_DEV_PIPE_*` flags | Pipe change flags (defined in pixelpipe_hb.h) |
| `dt_dev_pixelpipe_change` | Dispatches on the change flag |
| `dt_dev_pixelpipe_synch_top` | Re-commits the top history item only |
| `dt_dev_pixelpipe_synch_all` | Commits all defaults, then all active history items |
| `_dev_pixelpipe_process_rec` | Recursive per-module processing |

**Other**

| Symbol | File | Role |
|---|---|---|
| `_dev_pixelpipe_cache_basichash` | pixelpipe_cache.c | Builds the cache key from image id, profiles, and all modules up to a position |
| `_pop_undo` | libs/history.c | Undo/redo handler |
| `enter` | views/darkroom.c | Darkroom entry: load image, create GUIs, reload defaults, replay history |
| `_dev_load_requested_image` | views/darkroom.c | Darkroom image switch: keep base instances, read history, replay |

## See also

- [IOP_Module_API.md](IOP_Module_API.md): the callback API reference (signatures, the two
  [`reload_defaults`](IOP_Module_API.md#reload_defaults---per-image-defaults) jobs).
- [pixelpipe_architecture.md](pixelpipe_architecture.md): pipeline architecture, including the
  [ordering-asymmetry note](pixelpipe_architecture.md#pipeline-ordering-asymmetry).
