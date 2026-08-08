# Module Lifecycle: What Happens When the User Interacts

[IOP_Module_API.md](IOP_Module_API.md) documents the callback API: what each function signature
means and what a module must implement. This document is its companion. It describes *when* those callbacks
fire and *what else* the system does around them — pipe change flags, cache invalidation,
history replay, and cross-module data flow.

After reading the API doc you can implement a module. After reading this one you should also
have a mental model of what darktable does when the user loads an image, drags a slider,
toggles a module, resets it, or undoes an edit.

References point to code by **file, function, and the relevant block** (an `if`, `for`, or
`while`), and use short snippets where that is clearer. They avoid line numbers, which drift
as the source changes.

---

## 1. Background concepts

These are the moving parts the later sections rely on. Each one gets a short definition, not
a full reference.

### History stack (`dt_dev_history_item_t`)

An ordered list of per-module parameter snapshots on
[`dt_develop_t`](pixelpipe_architecture.md#dt_develop_t) (the main darkroom session state).
`dev->history_end` is
the active cursor: only the entries in the range `[0, history_end)` are "live". Note that a
history item's **`enabled` flag is stored separately from `params`** — it is not part of the
params buffer. Toggling a module on or off and editing its parameters are therefore two
different kinds of history change.

### Pipe types

Three pixelpipes run the same module chain independently, at different resolutions:

- **full** — the main darkroom image
- **preview** — the low-resolution thumbnail (used for the histogram and navigation)
- **preview2** — an optional second-window preview

Each is marked dirty and re-run on its own, often in parallel on separate threads. "Mark all
pipes dirty" below means all of the pipes that exist for the current session.

### Pipe change flags

These are set on `pipe->changed`. They tell `dt_dev_pixelpipe_change()` (pixelpipe_hb.c)
which resync strategy to use when the pipeline next runs. The constants are defined in
pixelpipe_hb.h.

| Flag | Meaning | Strategy |
|---|---|---|
| `DT_DEV_PIPE_TOP_CHANGED` | Only the topmost history item's params changed | `synch_top`: re-commit that one module; upstream piece hashes stay untouched |
| `DT_DEV_PIPE_SYNCH` | All items need a param resync, topology unchanged | `synch_all`: re-commit every history item in order |
| `DT_DEV_PIPE_REMOVE` | A module was added or removed; topology must be rebuilt | full `cleanup_nodes` + `create_nodes` + `synch_all` |
| `DT_DEV_PIPE_ZOOMED` | Zoom or pan only | skip the param sync; only the `modify_roi_in`/`modify_roi_out` chain re-runs |

`synch_all` re-commits every module, but a module whose hash did not change still produces a
cache hit, so its `process()` is skipped. In other words, the flag controls *committing*, not
*reprocessing*. Reprocessing is decided per module by the cache (see
[section 6](#6-pipeline-execution-detail)).

### Pipe status flags

`DIRTY` (needs reprocessing), `RUNNING` (a thread is processing it now), `VALID` (output is
current), `INVALID` (unusable, for example during teardown).

### `focus_hash`

A module is **focused** when the user opens its panel or clicks into one of its widgets; the
darkroom tracks a single focused module at a time. `dt_iop_request_focus()` records that module
and updates `dev->focus_hash`, a hash of the focused module's widget. Inside
`_dev_add_history_item_ext()` (develop.c), the relevant test is roughly:

```c
if(dev->focus_hash != hist->focus_hash) { /* force a new history entry */ }
```

If the user moved focus since the last commit, a **new** history entry is forced even when
the same module is edited again. The practical effect is that the edit then takes the `SYNCH`
path instead of the cheaper `TOP_CHANGED` path. If you write deferred or batched parameter
changes, keep this in mind, because it changes how much of the pipe re-commits.

### Pixelpipe cache

The cache is hash-based with lazy invalidation. The key for a module's output is the
**cumulative hash** of every upstream `piece->hash` value (each computed in
`dt_iop_commit_params()` from the op name, instance, params, and blend_params), combined with
the relevant color-profile information. It is *not* a hash of one module's data on its own.
The hashing happens in the basic-hash helper in pixelpipe_cache.c, which seeds the hash from
the image id and pipe profiles and then walks all modules up to the requested position.

When the key matches a cached entry, the output is reused and `process()` is skipped. Stale
entries are not actively deleted on a param change; they simply stop being looked up, and the
fixed-size LRU evicts them later. For the full caching model see
[pixelpipe_architecture.md](pixelpipe_architecture.md#pipeline-caching).

### `dev->chroma` (shared WB/CAT state)

A struct on `dt_develop_t` used by white balance (`temperature.c`, the producer) and color
calibration (`channelmixerrgb.c`, the consumer) to communicate. There is **no signal**;
synchronization is implicit, through the order in which `commit_params()` runs (see
[section 4](#4-cross-module-interactions) and [section 5](#5-pipeline-ordering-asymmetry)).
Temperature writes the camera WB reference in `reload_defaults()` and the live coefficients in
`commit_params()`; channelmixerrgb reads them. The full field-by-field producer/consumer map and
the reverse-load reasoning live in their own doc — see
[wb_and_colorcalibration](iop/wb_and_colorcalibration/README.md).

### `module->params` vs `module->default_params`

`default_params` is **image-specific**: it is set by `reload_defaults()` (see
[IOP_Module_API.md](IOP_Module_API.md#reload_defaults---per-image-defaults)). `params` is the
**live editing state**.
`dt_dev_pop_history_items_ext()` resets every module to its `default_params` and then replays
the history entries on top. As a result, a module with no history entry runs with its
image-specific defaults, not with compile-time constants.

---

## 2. Image loading

A pristine (never edited) image and one with saved history both go through the **same**
`dt_dev_load_image()` → `dt_dev_read_history_ext()` chain. The "no history" versus "has
history" distinction is handled **inside** `dt_dev_read_history_ext()`, not by different
callers.

### Load sequence (`dt_dev_load_image()`, develop.c)

1. **`_dt_dev_load_raw()`** triggers the rawspeed decode through the mipmap cache (this
   blocks), then reads the decoded image struct into `dev->image_storage`. The large mipmap
   buffer is released right after the decode; `image_storage` keeps the metadata. (The *mipmap
   cache* holds pre-scaled copies of each image at several resolutions, used for fast thumbnail
   and preview generation — see [mipmap](https://en.wikipedia.org/wiki/Mipmap).)
2. Mark all pipes `DIRTY` and set `loading = TRUE`.
3. **`dt_iop_load_modules()`** builds the IOP module list (`dev->iop`).
4. **`dt_dev_read_history_ext()`** does everything else — defaults, presets, and history
   replay. See below.

`dt_dev_load_image()` itself does not touch `dev->chroma` and does not load defaults. All of
that happens inside `dt_dev_read_history_ext()`.

### Inside `dt_dev_read_history_ext()` (develop.c)

This function applies saved history by **building `dev->history` directly from the database** —
it allocates one history item per stored row and appends it to the list. (This is a different
mechanism from the reset-then-replay path that undo/redo uses later; that one is described in
[section 3f](#3f-undo-redo-and-history-panel-navigation).)

**For every image** — this is the `if(!no_image)` block, and it runs whether or not the image
has saved history:

- Clear the scratch `memory.history` table.
- **`dt_dev_reset_chroma()`** clears part of the shared WB state
  ([section 5](#5-pipeline-ordering-asymmetry) lists exactly which fields): `temperature` and
  `adaptation` become `NULL`, and `wb_coeffs[]` is reset to 1.0.
- **`_dt_dev_load_pipeline_defaults()`** calls `dt_iop_reload_defaults()` on every module in
  **reverse** pipe order ([section 5](#5-pipeline-ordering-asymmetry) explains the direction).
  This sets each module's image-specific
  `default_params`. Because it goes through the wrapper, it also copies them into `params`
  (via `dt_iop_load_default_params()`). Temperature additionally populates
  `dev->chroma.as_shot[]` and `D65coeffs[]` as a side effect.
- **`_dev_add_default_modules()`** prepends the workflow-mandated modules into the in-memory
  history.
- **`_dev_auto_apply_presets()`** applies auto-presets. It is gated on the
  `DT_IMAGE_AUTO_PRESETS_APPLIED` image flag, **not** on `change_timestamp == -1` (that test
  guards only the separate legacy pre-3.0 WB recovery branch). It is a no-op for an image
  whose presets were already applied, so only a first import actually gains preset entries
  here.
- **`_dev_merge_history()`** merges the in-memory default and preset history into
  `main.history`, so the database read below picks it up.

**Then, for all images,** the same database-read loop runs. For each `main.history` row it
allocates a `hist` item, copies the params (falling back to the module's `default_params`
when the row has none), sets `hist->enabled`, appends to `dev->history`, and increments
`history_end`:

```c
memcpy(hist->params, hist->module->default_params, hist->module->params_size);
hist->enabled = TRUE;
...
dev->history = g_list_append(dev->history, hist);
dev->history_end++;
```

Modules with no database row keep the image-specific `default_params` set earlier by
`_dt_dev_load_pipeline_defaults()`. The `enabled` flag is carried per entry, separately from
params.

After the loop, if both `temperature` and `channelmixerrgb` are present, the function calls
`temperature->reload_defaults(temperature)` **directly**, to resync the WB/CAT shared state:

```c
if(temperature && channelmixerrgb)
  temperature->reload_defaults(temperature);
```

This direct call is a deliberate wrapper bypass: it skips the framework's wholesale
`default_params → params` copy (`dt_iop_load_default_params()`). It still refreshes
`default_params` and the `dev->chroma` reference data, and temperature's own body also writes
a few `self->params` fields directly (`preset`, `late_correction`). So `params` is partially
updated, not fully resynced — see the `reload_defaults` caveat in
[IOP_Module_API.md](IOP_Module_API.md#reload_defaults---per-image-defaults).

The function then re-reads `history_end` from `main.images`, and — when the GUI is attached —
calls `dt_dev_pipe_synch_all()` and `dt_dev_invalidate_all()` so the pipes will commit and
reprocess. The pipes stay `DIRTY`, and the pipeline threads run on the next redraw
([section 6](#6-pipeline-execution-detail)).

---

## 3. Editing operations

### 3a. Adjust a slider — the common `TOP_CHANGED` path

1. The bauhaus slider fires its value-change handler (`bauhaus.c`), which calls
   `dt_iop_gui_changed(module, widget, &prev)` directly.
2. The module's `gui_changed()` runs (module-specific; it may update dependent widgets).
3. `dt_dev_add_history_item_target()` reaches `_dev_add_history_item_ext()` (develop.c), which
   chooses one of two branches:
   - **`TOP_CHANGED`**: the same module is already at the top of the history,
     `dev->focus_hash == hist->focus_hash`, and only the params differ. The params are
     updated in place and `pipe->changed |= DT_DEV_PIPE_TOP_CHANGED`.
   - **`SYNCH`**: anything else (a different module, or focus changed since the last commit).
     A new history entry is pushed and `pipe->changed |= DT_DEV_PIPE_SYNCH`.
4. `dt_dev_invalidate_all()` marks all pipes `DIRTY` and increments `dev->timestamp`.
5. `DT_SIGNAL_DEVELOP_HISTORY_CHANGE` is emitted **only when `need_end_record` is true**, not
   on every slider tick. It is tied to the undo-record start/end pairing, so one logical edit
   emits the signal once.
6. On the pipeline thread, `dt_dev_pixelpipe_change()` dispatches on the flag:
   - `TOP_CHANGED` → `dt_dev_pixelpipe_synch_top()`: runs `commit_params()` for that one
     module only; upstream piece hashes are not recomputed.
   - `SYNCH` → `dt_dev_pixelpipe_synch_all()`: runs `commit_params()` for every history item
     in order. Upstream modules re-commit, but an unchanged hash still hits the cache.
7. The pipeline runs: modules with unchanged hashes reuse their cached output; the changed
   module and everything downstream of it reprocess.

### 3b. Enable / disable a module

1. The toggle button fires its `toggled` signal, which reaches `_gui_off_callback()`
   (imageop.c).
2. The callback sets `module->enabled` and calls `dt_dev_add_history_item()`. The
   `hist->enabled` flag is recorded **separately from params**.
3. `pipe->changed |= DT_DEV_PIPE_SYNCH`, because an enable/disable propagates downstream.
4. A full `synch_all` runs on the next pipeline pass.

### 3c. Reset to defaults

1. The reset button fires `button-press-event`, which reaches `_gui_reset_callback()`
   (imageop.c).
2. `dt_iop_reload_defaults(module)` recomputes the image-specific `default_params` and copies
   them into `module->params` (this is the wrapper path, so the copy does happen).
3. `dt_iop_gui_update(module)` syncs the widgets to the new params.
4. `dt_dev_add_history_item(module->dev, module, TRUE)` adds or updates a **single** history
   entry. It does not rebuild the whole stack.
5. `pipe->changed |= DT_DEV_PIPE_SYNCH`.

### 3d. Add a module instance (+ button)

1. `dt_iop_gui_duplicate()` (imageop.c) starts the duplication.
2. It records the base module's state: `dt_dev_add_history_item(base->dev, base, FALSE)`.
3. `dt_dev_module_duplicate()` creates the new instance in `dev->iop`.
4. `dt_iop_reload_defaults(new_module)` sets the image-specific defaults for the new instance.
5. `dt_dev_add_history_item(new_module->dev, new_module, TRUE)` adds a new history entry.
6. `dt_dev_pixelpipe_rebuild()` sets `pipe->changed |= DT_DEV_PIPE_REMOVE`, which forces a
   full topology rebuild — the node graph changed, so it is rebuilt from scratch.

### 3e. Remove a module instance (− button)

`dt_dev_module_remove()` (develop.c) does the teardown. Inside its `if(dev->gui_attached)`
block — true in the interactive darkroom — it **prunes the history**. It walks `dev->history`
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

It then removes the module from `dev->iop`. A `REMOVE` flag triggers a full topology rebuild
(`cleanup_nodes` + `create_nodes`).

So a removed instance's history entries are **actively deleted**, not merely skipped. In a
headless context, where `gui_attached` is false, this pruning block does not run — but
interactive removal is the case this document covers.

### 3f. Undo, redo, and history-panel navigation

This is a **distinct path** from the initial image load. During an active editing session it
is the main way `module->params` are rewritten from saved entries.

1. The user clicks a history entry, or presses Ctrl+Z / Ctrl+Y.
2. `dt_dev_reload_history_items()` (develop.c) sets `dev->history_end` to the target position.
3. `dt_dev_pop_history_items(dev, 0)` resets all modules to their `default_params`.
4. The history is re-read (from memory or the database).
5. `dt_dev_pop_history_items(dev, history_end)` replays up to the target position.
6. `dt_dev_invalidate_all()` marks the pipes dirty, which leads to a full pipeline rerun.

The two `pop_history_items` calls live **here**, in `dt_dev_reload_history_items()`, not in
`dt_dev_read_history_ext()`. Apart from the initial image load, this is the only path that
rewrites `module->params` from saved history. The `SYNCH` flag ensures every module
re-commits after the replay.

---

## 4. Cross-module interactions

### 4a. Temperature (WB) → ChannelMixerRGB (through `dev->chroma`)

There is no signal; synchronization is implicit and ordered by iop_order. Because temperature is
upstream of channelmixerrgb, a full `synch_all` runs `commit_params()` in pipe order:
`temperature.commit_params()` writes `dev->chroma.wb_coeffs[]` first and
`channelmixerrgb.commit_params()` reads it afterward to build its chromatic-adaptation matrix.
(`synch_top` re-commits only the topmost history item, so it is not evidence that an upstream
producer has just run.) The full producer/consumer field map, the once-per-load `reload_defaults`
side effects, and why reverse-order default loading stays correct are covered in
[wb_and_colorcalibration](iop/wb_and_colorcalibration/README.md).

### 4b. Color-profile change (colorin / colorout)

This follows the same flow as a slider adjust: `TOP_CHANGED` or `SYNCH`, depending on
`focus_hash` and on whether it is an in-place update. There is no `REMOVE`, because the
topology does not change. `commit_params()` rebuilds the color transforms; no cross-module
signal is needed. Because the transform changes the pixels, all downstream cache hashes change
and everything downstream reprocesses.

---

## 5. Pipeline ordering asymmetry

Two operations walk the module list in **opposite** directions, which is a common source of
confusion. (See also the
[ordering-asymmetry note](pixelpipe_architecture.md#pipeline-ordering-asymmetry) in
pixelpipe_architecture.md.)

- **`commit_params()` (through `synch_all`)** iterates the history in history-list order, which
  is effectively pipe order: an upstream module commits before a downstream one, so any state it
  writes into shared structures is in place when the downstream module reads it.
- **`_dt_dev_load_pipeline_defaults()`** iterates in **reverse** pipe order (last module first):

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

A module's `reload_defaults()` **cannot** assume that earlier-in-pipe modules have already
populated shared state (such as `dev->chroma`), because under reverse iteration they have not run
yet. Shared state that a `reload_defaults()` depends on must therefore be **reset to a neutral
value before the reverse default-load** — which the framework does via `dt_dev_reset_chroma()`
just before `_dt_dev_load_pipeline_defaults()`.

The worked example — how white balance and color calibration share `dev->chroma`, and why this
reset keeps color calibration's defaults image-local despite the reverse order — is in
[wb_and_colorcalibration](iop/wb_and_colorcalibration/README.md).

---

## 6. Pipeline execution detail

How pixels actually flow once the pipes are marked dirty:

- `dt_dev_process_image_job()` (develop.c) is the entry point. It locks the pipe and captures
  `dev->timestamp`.
- `dt_dev_pixelpipe_process()` calls `_dev_pixelpipe_process_rec()` (pixelpipe_hb.c), which
  recurses from the last module back toward the input, pulling each module's input on demand.
- For each module, the cache key is the **cumulative hash** of all upstream `piece->hash`
  values (each set in
  [`dt_iop_commit_params()`](IOP_Module_API.md#commit_params---transform-ui-parameters-into-processing-data)
  from op, instance, params, and blend_params),
  plus the color-profile information. On a key match, the cached output is reused and
  `process()` is skipped. On a miss, `process()` (or `process_cl()` on the GPU) runs and the
  result is stored.
- `modify_roi_in()` and `modify_roi_out()` run on every module during the ROI-propagation
  phase, before processing, translating between the output-side and input-side regions. Under
  the `ZOOMED` flag only the ROIs change — the module hashes do not — so cache hits are
  possible for any module whose cached output region still covers the requested region.
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
| `_dt_dev_load_pipeline_defaults` | Calls `reload_defaults` on all modules, in reverse pipe order |
| `dt_dev_reset_chroma` | Resets shared WB state to neutral before reverse default-load |
| `dt_dev_add_history_item` / `_dev_add_history_item_ext` | Add/update a history entry; choose `TOP_CHANGED` vs `SYNCH` (the `focus_hash` test lives here) |
| `dt_dev_pop_history_items` / `_ext` | Reset to defaults, then replay history (used by undo/redo) |
| `dt_dev_reload_history_items` | Undo/redo and history-panel navigation |
| `dt_dev_module_remove` | Removes a module instance and prunes its history entries |
| `dt_dev_invalidate_all` | Marks all pipes dirty, bumps `dev->timestamp` |
| `_dev_auto_apply_presets` | Auto-presets; gated on `DT_IMAGE_AUTO_PRESETS_APPLIED` |
| `dt_dev_process_image_job` | Pixel-processing entry point |

**imageop.c**

| Symbol | Role |
|---|---|
| `dt_iop_reload_defaults` | Wrapper: calls the module's `reload_defaults()` then `dt_iop_load_default_params()` |
| `dt_iop_load_default_params` | Copies `default_params` into `params` |
| `dt_iop_commit_params` | Translates params into `piece->data`; sets `piece->hash` |
| `dt_iop_gui_changed` | Slider/widget change entry point |
| `_gui_off_callback` | Enable/disable toggle handler |
| `_gui_reset_callback` | Reset-to-defaults handler |
| `dt_iop_gui_duplicate` | Add a module instance |

**pixelpipe_hb.c / .h**

| Symbol | Role |
|---|---|
| `DT_DEV_PIPE_*` flags | Pipe change flags (defined in pixelpipe_hb.h) |
| `dt_dev_pixelpipe_change` | Dispatches on the change flag |
| `dt_dev_pixelpipe_synch_top` | Re-commits the top module only |
| `dt_dev_pixelpipe_synch_all` | Re-commits all history items in order |
| `_dev_pixelpipe_process_rec` | Recursive per-module processing |

**Other**

| Symbol | File | Role |
|---|---|---|
| Basic-hash helper (cumulative cache hash) | pixelpipe_cache.c | Builds the cache key from image id, profiles, and all modules up to a position |

For the white-balance ↔ color-calibration symbols (`temperature.*` / `channelmixerrgb.*` and the
`dev->chroma` helpers), see
[wb_and_colorcalibration](iop/wb_and_colorcalibration/README.md#key-symbols).

## See also

- [IOP_Module_API.md](IOP_Module_API.md) — the callback API reference (signatures, the two
  [`reload_defaults`](IOP_Module_API.md#reload_defaults---per-image-defaults) jobs).
- [pixelpipe_architecture.md](pixelpipe_architecture.md) — pipeline architecture, including the
  [ordering-asymmetry note](pixelpipe_architecture.md#pipeline-ordering-asymmetry).
- [wb_and_colorcalibration](iop/wb_and_colorcalibration/README.md) — the white-balance ↔
  color-calibration interaction through `dev->chroma` (worked example of the ordering principle).
