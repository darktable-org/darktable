# How white balance and color calibration share state

White balance (`temperature.c`) hands its data to color calibration (`channelmixerrgb.c`)
through shared state. No signal passes between them. White balance writes its state into the
dev's `dev->chroma` and into each pipe's `pipe->wb`. Color calibration and several other
modules read it from there. Whether a reader sees current values depends on the order in which
darktable runs the two modules' callbacks.

In the other direction, white balance's GUI asks color calibration, through a proxy, whether
any instance applies chromatic adaptation. [White balance asks color calibration through a
proxy](#white-balance-asks-color-calibration-through-a-proxy) describes this call.

This page describes that state and the ordering it relies on. For the general mechanics (pipe
change flags, `synch_all` and `synch_top`, the reverse-order default load), see
[Module_Lifecycle.md](../../Module_Lifecycle.md).

---

## Where the state lives

`dev->chroma` is a `dt_dev_chroma_t` (`develop.h`) on the dev
([`dt_develop_t`](../../pixelpipe_architecture.md#dt_develop_t)). It holds:

- `temperature`, a pointer to the white balance instance, and `adaptation`, a pointer to the
  color calibration instance that does chromatic adaptation.
- `as_shot[]`, the camera's as-shot coefficients.
- `wb`, a `dt_dev_wb_t` (`format.h`). Its `coeffs[]` are the coefficients that white balance
  applies, `D65coeffs[]` is the camera's D65 reference, and `late_correction` says whether a
  later module completes the correction to D65.

Each pipe has its own `dt_dev_wb_t`, `pipe->wb`. White balance's `commit_params()` fills it
from its own piece, through `_publish_pipe_wb()`.

### Why each pipe has its own copy

All pipes of a dev (main view, preview, and second window) share `dev->chroma`, and they run in
parallel on separate threads. White balance's `commit_params()` rewrites `dev->chroma` once per
pipe, while other pipes may still be processing.

Late correction is split across the pipe. White balance multiplies the data by `coeffs[]` near
the start, and colorin later multiplies it by `D65coeffs[] / coeffs[]`. If colorin read
`dev->chroma`, it could get coefficients from a different commit than the one that white
balance applied to its input. The pixelpipe cache keys that mixed result on the pipe's own
parameters, so the wrong image stays in the cache.

So code that runs in a pipe thread reads `pipe->wb`, never `dev->chroma.wb`. The one exception
is white balance's `commit_params()`, which copies `dev->chroma.wb.D65coeffs[]` into
`pipe->wb`. GUI code and `reload_defaults()` read `dev->chroma`.

---

## Who writes and reads each field

### `dev->chroma`

| Field | Written by | Read by |
|---|---|---|
| `as_shot[]` | `temperature.reload_defaults()` | `temperature` only (its "as shot" preset, GUI, and debug output) |
| `wb.D65coeffs[]` | `temperature.reload_defaults()` | `temperature` (GUI), `channelmixerrgb`; `_publish_pipe_wb()` copies it into `pipe->wb` |
| `wb.coeffs[]` | `temperature.commit_params()` | `channelmixerrgb` |
| `wb.late_correction` | `temperature.commit_params()`, `_update_preset()` (GUI) | `channelmixerrgb`, through `_dev_is_D65_chroma()` |
| `temperature` | `temperature.commit_params()` | `channelmixerrgb` (`_set_trouble_messages()`) |
| `adaptation` | `channelmixerrgb` (`_declare_cat_on_pipe()`, only when the module has a GUI; `gui_cleanup()` clears it) | `channelmixerrgb` (`reload_defaults()`, `_set_trouble_messages()`) |

In `channelmixerrgb`, these functions read `dev->chroma.wb`: `reload_defaults()`,
`_set_trouble_messages()`, and the GUI functions `gui_changed()`,
`_preview_pipe_finished_callback()`, `_illuminant_color_draw()`, and `_update_approx_cct()`.
Most of them read it through `_get_d65_correction_ratios()` and `_dev_is_D65_chroma()`.

Two functions also reset fields. `dt_dev_reset_chroma()` sets `temperature` and `adaptation` to
`NULL` and `wb.coeffs[]` to `1.0`. It runs before the defaults of each image are loaded, and
from `channelmixerrgb.cleanup_pipe()`. `dt_dev_init_chroma()` also sets `wb` and `as_shot[]` to
neutral, when the dev is created and when it is cleaned up.

Notes:

- `temperature.reload_defaults()` writes `as_shot[]` and `wb.D65coeffs[]` on every image load,
  even when white balance is disabled or absent from the history. So color calibration always
  has the camera's reference. The exception is a call without a valid image, from the presets
  code, which returns before it writes them. For images without a camera matrix, `as_shot[]`
  stays `1.0`.
- In the `none` workflow, when both modules are enabled in the history,
  `dt_dev_read_history_ext()` calls `temperature->reload_defaults(temperature)` a second time,
  directly. This call skips the framework wrapper, so it updates only some fields of
  `self->params`.
  [IOP_Module_API.md](../../IOP_Module_API.md#reload_defaults---per-image-defaults) and
  [Module_Lifecycle.md](../../Module_Lifecycle.md) describe the consequences.
- `channelmixerrgb` does not read `as_shot[]`. Color calibration takes its default illuminant
  from `dev->image_storage.wb_coeffs`, as described in [Why the reverse-order default load is
  safe](#why-the-reverse-order-default-load-is-safe).

### `pipe->wb`

`temperature.commit_params()` writes all three fields through `_publish_pipe_wb()`:

- `coeffs[]` from its piece, or `1.0` when the piece is disabled.
- `late_correction` from its piece, or `FALSE` when the piece is disabled.
- `D65coeffs[]`, copied from `dev->chroma.wb`.

`dt_dev_wb_set_neutral()` sets `pipe->wb` to neutral when the pipe is created.

These functions read `pipe->wb`, all in the pipe thread:

| Module | Functions | Use |
|---|---|---|
| color calibration | `commit_params()`, `process()`, `process_cl()` | the `DT_ILLUMINANT_CAMERA` and `DT_ILLUMINANT_FROM_WB` illuminants |
| colorin | `process()`, `process_cl()` | the late correction to D65 |
| highlights, clip mode | `process_clip()`, `process_cl()` | clip thresholds under late correction |
| opposed highlight reconstruction | `_process_opposed()`, `process_opposed_cl()` | the late correction |
| segmentation-based highlight reconstruction | `_process_segmentation()` | the late correction |

---

## White balance asks color calibration through a proxy

When the user switches white balance from the camera reference mode to another mode,
`_update_preset()` (`temperature.c`) sets `p->late_correction` to the result of
`dt_dev_cat_is_active()` (`develop.c`). The answer becomes part of white balance's params, and
so of the history.

`dt_dev_cat_is_active()` calls `dev->proxy.cat_is_active` for each `channelmixerrgb` instance
in `dev->iop` that is in the pipe (`iop_order != INT_MAX`), and returns `TRUE` if one of them
answers `TRUE`. Color calibration's `gui_init()` sets that pointer to `_cat_proxy_is_active()`.
That function reads the instance's current params and returns `TRUE` when the instance is
enabled, its illuminant is not `DT_ILLUMINANT_PIPE`, its adaptation is not
`DT_ADAPTATION_RGB`, and the image is not monochrome (`_applies_cat()`).

The proxy has three limits:

- Call it from the GTK main thread only (`develop.h`). It reads params that the GUI changes.
- Only `darktable.develop` gets the pointer. For any other dev, such as the one that
  `_pin_image()` creates, and in `darktable-cli`, `dt_dev_cat_is_active()` returns `FALSE`.
- Nothing clears the pointer. The instance that `_init_module_so()` (`imageop.c`) creates and
  destroys at startup also sets it, so a set pointer does not mean that a live instance exists.
  This is why `dt_dev_cat_is_active()` asks only the instances in `dev->iop`.

---

## Commit order changes the result only when the illuminant lookup fails

`synch_all` commits in two passes:

1. The defaults of every piece, in **pipe order**.
2. The active history items, in **history order** (`_dev_pixelpipe_synch()`).

During the history pass, `temperature.commit_params()` writes `pipe->wb` and the `dev->chroma`
fields listed above. `channelmixerrgb.commit_params()` reads `pipe->wb` to build its chromatic
adaptation matrix. History order is not pipe order. If the history ends in `[..., color
calibration, white balance]`, color calibration commits before white balance's last commit. It
reads the `pipe->wb` left by white balance's previous commit on the same pipe.

`synch_top` re-commits only the top history item. If the user edits white balance while it is
the top item, `_dev_add_history_item_ext()` (`develop.c`) sets `DT_DEV_PIPE_TOP_CHANGED`, and
only white balance re-commits. Color calibration's `piece->data` keeps the illuminant that its
last commit or its last processing run computed.

In both cases, processing corrects the illuminant if it can. Only two illuminants depend on
white balance, `DT_ILLUMINANT_CAMERA` and `DT_ILLUMINANT_FROM_WB` (`illuminant_to_xy()` in
`illuminants.h`). `channelmixerrgb.process()` and `process_cl()` look up both again from
`pipe->wb` and write the result into `piece->data`. Processing starts only after all of the
pipe's commits are done, so the lookup sees white balance's final `pipe->wb`.

### A failed lookup keeps an illuminant that depends on commit order

If the lookup in `process()` or `process_cl()` fails, the illuminant in `piece->data` stays as
it is. That value can come from an earlier white balance setting, so commit order then changes
the rendered image.

`DT_ILLUMINANT_FROM_WB` can reach this case. `find_illuminant_xy_from_wb_coeffs()` fails when
one of the first three values in `pipe->wb.coeffs[]` is zero or is not a normal float
(`_wb_coeffs_invalid()` in `illuminants.h`). White balance allows a zero coefficient. Its
params start at `0.0`, and its sliders take their range from the params. Take a history whose
last two entries are color calibration in `DT_ILLUMINANT_FROM_WB` mode and white balance with a
zero red coefficient:

- If color calibration commits first, its lookup uses the valid coefficients of white
  balance's previous commit, and it stores that illuminant.
- If white balance commits first, color calibration's lookup fails, and it stores the
  illuminant from its saved `p->x` and `p->y`.

Processing fails the same lookup in both cases and keeps the stored illuminant, so the two
renders differ. After `synch_top`, the illuminant that stays is the one from before the user's
white balance edit.

`DT_ILLUMINANT_CAMERA` does not depend on commit order in this way. Its lookup scales the
image's own as-shot coefficients by ratios that are positive in the three channels the lookup
checks (`_get_d65_correction_ratios()`). So it fails only because of the image, for example
when the image has no camera matrix. It then fails at commit time too, and the illuminant
comes from `p->x` and `p->y` in every order.

---

## Why the reverse-order default load is safe

`_dt_dev_load_pipeline_defaults()` walks the modules in **reverse** pipe order, so
`channelmixerrgb.reload_defaults()` runs **before** `temperature.reload_defaults()`. Color
calibration reads `dev->chroma` before white balance has refreshed it for the new image. A
reset before the load keeps the result correct.

`dt_dev_read_history_ext()` calls `dt_dev_reset_chroma()` before the reverse default load. That
function clears the modules' trouble messages, sets `adaptation` and `temperature` to `NULL`,
and sets `wb.coeffs[]` to `1.0`:

```c
chr->adaptation = NULL;
chr->temperature = NULL;
for_four_channels(c)
  chr->wb.coeffs[c] = 1.0f;
```

It keeps the previous image's `wb.D65coeffs[]`, `as_shot[]`, and `wb.late_correction`. Only
`dt_dev_init_chroma()` sets those to neutral.

`channelmixerrgb.reload_defaults()` then does two things:

1. It calls `_get_d65_correction_ratios()`, which returns identity ratios (`1.0`) unless all of
   these are true: the image has a camera matrix, `_dev_is_D65_chroma()` is false,
   `D65coeffs[]` are valid, and `changed_chroma` is true:

   ```c
   const gboolean changed_chroma =
     wb->coeffs[0] > 1.0f || wb->coeffs[1] > 1.0f || wb->coeffs[2] > 1.0f;
   ```

   Right after the reset, `wb.coeffs[]` is exactly `1.0`, so `changed_chroma` is false and the
   ratios stay at identity. The previous image's `D65coeffs[]` and `late_correction` cannot
   change the result. If `late_correction` is set, `_dev_is_D65_chroma()` returns true, which
   also gives identity ratios.
2. With identity ratios, `find_illuminant_xy_from_as_shot_coeffs()` computes the default
   illuminant from `img->wb_coeffs` (`dev->image_storage.wb_coeffs`). These are the current
   image's as-shot coefficients, read from the raw file.

So the reset of `wb.coeffs[]` is what ties color calibration's default illuminant to the
current image. The default depends only on the image's own raw coefficients, never on what
`dev->chroma` held for the previous image.

---

## Keep the first commit of each image a `synch_all`

`synch_top` re-commits only the top history item. It needs an earlier full commit to have
written `pipe->wb`. Two facts make this hold today:

- The first commit on every pipe is a `synch_all`. In the darkroom, `dt_dev_read_history_ext()`
  calls `dt_dev_pipe_synch_all()` at load, and `dt_dev_pixelpipe_change()` skips `synch_top`
  when the `SYNCH` flag is set (`... && !sync_all`). Export calls
  `dt_dev_pixelpipe_synch_all()` directly.
- The first pass of `synch_all` commits every piece in pipe order. White balance therefore
  writes `pipe->wb` before any later module commits or processes.

`pipe->wb` is neutral only when the pipe is created (`dt_dev_wb_set_neutral()` in
`_dev_pixelpipe_init_cached()`). The darkroom keeps its pipes when it switches images. Until
white balance's first commit, `pipe->wb` still holds the previous image's values, and the first
`synch_all` overwrites them before anything reads them.

Do not add a path that runs `synch_top` before the first `synch_all` of an image. Such a path
would let later modules read the previous image's `pipe->wb`.

---

## Developer rules

- Code that runs in a pipe thread reads `pipe->wb`, not `dev->chroma.wb`.
- A `reload_defaults()` cannot assume that modules earlier in the pipe have already written
  shared state for the current image. In the reverse default load, they have not run yet. Color
  calibration stays correct only because the framework resets `wb.coeffs[]` to neutral before
  that load. The reset keeps the previous image's `D65coeffs[]` and `late_correction` out of
  the result.
- Reset shared state before a reverse walk over the modules. `dt_dev_reset_chroma()` does this
  for `dev->chroma`.

---

## Key symbols

| Symbol | File | Role |
|---|---|---|
| `dt_dev_wb_t` | format.h | Holds `coeffs[]`, `D65coeffs[]`, and `late_correction`; the type of `dev->chroma.wb` and `pipe->wb` |
| `dt_dev_reset_chroma` | develop.c | Clears trouble messages, `adaptation`, and `temperature`; sets `wb.coeffs[]` to `1.0` |
| `dt_dev_init_chroma` | develop.c | Resets, then sets `wb` and `as_shot[]` to neutral; runs when the dev is created and cleaned up |
| `dt_dev_wb_set_neutral` | develop.c | Sets a `dt_dev_wb_t` to neutral; also used when a pipe is created (`pixelpipe_hb.c`) |
| `temperature.reload_defaults` | temperature.c | Writes `dev->chroma.as_shot[]` and `dev->chroma.wb.D65coeffs[]` |
| `temperature.commit_params` | temperature.c | Writes `dev->chroma.wb.coeffs[]`, `dev->chroma.wb.late_correction`, and `dev->chroma.temperature`; fills `pipe->wb` through `_publish_pipe_wb()` |
| `_publish_pipe_wb` | temperature.c | Fills `pipe->wb` from the white balance piece; copies `D65coeffs[]` from `dev->chroma.wb` |
| `_update_preset` | temperature.c | GUI: writes `dev->chroma.wb.late_correction` when the user changes the white balance mode; when the user leaves camera reference, sets `p->late_correction` from `dt_dev_cat_is_active()` |
| `dt_dev_cat_is_active` | develop.c | GUI: asks each color calibration instance in `dev->iop`, through `dev->proxy.cat_is_active`, whether it applies chromatic adaptation |
| `_cat_proxy_is_active` | channelmixerrgb.c | The proxy that color calibration's `gui_init()` registers; returns `_applies_cat()` |
| `channelmixerrgb.reload_defaults` | channelmixerrgb.c | Computes the default illuminant from `img->wb_coeffs` |
| `channelmixerrgb.commit_params` | channelmixerrgb.c | Reads `pipe->wb` to build the chromatic adaptation matrix |
| `_declare_cat_on_pipe` | channelmixerrgb.c | Registers the instance that does chromatic adaptation in `dev->chroma.adaptation` (GUI only) |
| `_get_d65_correction_ratios` | channelmixerrgb.c | `D65coeffs[] / coeffs[]` of the given `dt_dev_wb_t`, gated by `changed_chroma` |
| `_dev_is_D65_chroma` | channelmixerrgb.c | True when `late_correction` is set or `coeffs[]` equals `D65coeffs[]` |
| `find_illuminant_xy_from_as_shot_coeffs` | illuminants.h | Default illuminant xy from `img->wb_coeffs` |
| `find_illuminant_xy_from_wb_coeffs` | illuminants.h | Illuminant xy from white balance coefficients (the `DT_ILLUMINANT_FROM_WB` illuminant) |

## See also

- [Module_Lifecycle.md](../../Module_Lifecycle.md): the general lifecycle (load, edits, history
  replay, pipe flags) and the rule for forward and reverse module order.
- [pixelpipe_architecture.md](../../pixelpipe_architecture.md): the pipeline architecture and
  an overview of the ordering asymmetry.
