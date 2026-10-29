# How white balance and color calibration share state

White balance (`temperature.c`) hands its data to color calibration (`channelmixerrgb.c`)
through shared state. No signal passes between them. White balance writes its state into the
dev's `dev->chroma` and into each pipe's `pipe->wb`. Color calibration and several other
modules read it from there. Whether a reader sees current values depends on the order in which
darktable runs the two modules' callbacks.

The two modules' GUIs ask each other through proxies, function pointers in `dev->proxy`. White
balance's GUI asks color calibration whether any instance applies chromatic adaptation. Color
calibration's GUI asks white balance for the values that white balance's current params give,
instead of reading `dev->chroma`. [White balance asks color calibration through a
proxy](#white-balance-asks-color-calibration-through-a-proxy) and [Color calibration asks white
balance through a proxy](#color-calibration-asks-white-balance-through-a-proxy) describe these
calls.

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
from the params that it commits, through `_wb_from_params()`.

### Four places hold white balance values

White balance values are also in two other places: the white balance piece's own data, and the
descriptor of each buffer that passes down the pipe. The table compares all four. For
`dev->chroma` and `pipe->wb`, it names only the main writers and readers. [Who writes and reads
each field](#who-writes-and-reads-each-field) lists all of them.

| Place | Scope | Written by | Holds | Read by |
|---|---|---|---|---|
| `dev->chroma.wb` | all pipes of a dev | `temperature.commit_params()`, which copies `pipe->wb`, and `temperature.reload_defaults()` for `D65coeffs[]` | the values that white balance last committed, in any pipe | color calibration's `reload_defaults()`, and white balance's GUI for `D65coeffs[]`. `_wb_from_params()` gets `D65coeffs[]` from it |
| white balance's `piece->data` (`dt_iop_temperature_data_t`) | the white balance piece of one pipe | `temperature.commit_params()` | the coefficients that white balance multiplies into the image | white balance's `process()` and `process_cl()` |
| `pipe->wb` | one pipe | `temperature.commit_params()`, through `_wb_from_params()` | what white balance applied, the same for every module in the pipe | colorin, highlight reconstruction, and color calibration |
| `pipe->dsc.temperature` | one buffer | white balance's `process()` and `process_cl()`, through `_update_pipe_dsc()`, and colorin, which rewrites `coeffs[]` to `D65coeffs[]` when it completes the late correction | the white balance multiplied into this buffer | highlight reconstruction, demosaic (dual demosaic and capture sharpening), denoise (profiled), and the details mask |

`pipe->dsc` is a `dt_iop_buffer_dsc_t`, the descriptor of the buffer that the pipe passes to the
next module. The pipe cache stores it with the buffer. If white balance is disabled, its
`process()` does not run, and `temperature` keeps the values of the image's input descriptor:
`enabled` is `FALSE` and `coeffs[]` are `0` (`dt_image_init()`).

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
`pipe->wb`. Color calibration's GUI does not read `dev->chroma.wb` either. It calls
`dt_dev_get_gui_wb()`, as described in [Color calibration asks white balance through a
proxy](#color-calibration-asks-white-balance-through-a-proxy). The remaining readers are white
balance's GUI and color calibration's `reload_defaults()`.

---

## Who writes and reads each field

### `dev->chroma`

| Field | Written by | Read by |
|---|---|---|
| `as_shot[]` | `temperature.reload_defaults()` | `temperature` only (its "as shot" preset, GUI, and debug output) |
| `wb.D65coeffs[]` | `temperature.reload_defaults()` | `temperature` (GUI, `commit_params()`, and `_gui_wb_proxy()`, which pass it to `_wb_from_params()`), `channelmixerrgb` (`reload_defaults()`) |
| `wb.coeffs[]` | `temperature.commit_params()`, copied from `pipe->wb` | `channelmixerrgb` (`reload_defaults()`) |
| `wb.late_correction` | `temperature.commit_params()`, copied from `pipe->wb` | `channelmixerrgb` (`reload_defaults()`, through `_dev_is_D65_chroma()`) |
| `temperature` | `temperature.commit_params()` | `channelmixerrgb` (`_set_trouble_messages()`) |
| `adaptation` | `channelmixerrgb` (`_declare_cat_on_pipe()`, only when the module has a GUI; `gui_cleanup()` clears it) | `channelmixerrgb` (`reload_defaults()`, `_set_trouble_messages()`) |

`dt_dev_get_gui_wb()` also copies all of `dev->chroma.wb` when no white balance instance
answers its proxy.

In `channelmixerrgb`, only `reload_defaults()` reads `dev->chroma.wb`, through
`_get_d65_correction_ratios()` and `_dev_is_D65_chroma()`. While the defaults load, white
balance's params do not hold the edit's white balance yet, so `reload_defaults()` cannot ask
the proxy. The GUI functions `gui_changed()`, `_set_trouble_messages()`,
`_preview_pipe_finished_callback()`, `_illuminant_color_draw()`, `_update_approx_cct()`, and
`_auto_set_illuminant()` get their `dt_dev_wb_t` from `dt_dev_get_gui_wb()`.

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

`temperature.commit_params()` writes all three fields through `_wb_from_params()`. White balance
applies nothing when the piece is disabled or the image is true monochrome:

- `coeffs[]` from the params, or `1.0` when white balance applies nothing.
- `late_correction` from the params. It is `FALSE` in camera reference mode
  (`DT_IOP_TEMP_D65`) and when white balance applies nothing.
- `D65coeffs[]`, copied from `dev->chroma.wb`.

`commit_params()` then copies `coeffs[]` and `late_correction` from `pipe->wb` into
`dev->chroma.wb`.

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

## Color calibration asks white balance through a proxy

Color calibration's GUI uses white balance's values in several places. It sets `p->x` and
`p->y` from them in `gui_changed()` when the user selects `DT_ILLUMINANT_CAMERA` or
`DT_ILLUMINANT_FROM_WB`, or switches away from one of them, so the values go into the history.
It also uses them for the illuminant swatch, the approximate CCT, the spot measurement, and the
trouble messages.

`dev->chroma.wb` does not suit these readers. It changes only when a pipe commits, so after a
white balance edit it holds the previous values until the next commit. A pipe thread can also
rewrite it while the GTK thread reads it.

So these functions call `dt_dev_get_gui_wb()` (`develop.c`). It looks up the white balance
instance in `dev->iop` and calls `dev->proxy.gui_wb` for it. White balance's `gui_init()` sets
that pointer to `_gui_wb_proxy()`, which passes the instance's current params,
`self->enabled`, `self->hide_enable_button`, and `dev->chroma.wb.D65coeffs[]` to
`_wb_from_params()`. `commit_params()` uses the same function, so the GUI gets the values that
the next commit writes into `pipe->wb`. The GTK thread owns the params, so the GUI sees an edit
before any pipe commits it.

This proxy has the same three limits as `cat_is_active`. Call it from the GTK main thread
only. Only `darktable.develop` gets the pointer. Nothing clears the pointer, so
`dt_dev_get_gui_wb()` asks only an instance that it finds in `dev->iop`. If no instance
answers, the helper copies `dev->chroma.wb`. For a `NULL` dev, it returns neutral values.

---

## Commit order changes the stored illuminant, not the rendered one

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

In both cases, processing replaces that illuminant. Only two illuminants depend on white
balance, `DT_ILLUMINANT_CAMERA` and `DT_ILLUMINANT_FROM_WB` (`_get_corrected_illuminant_xy()` in
`channelmixerrgb.c`). `channelmixerrgb.process()` and `process_cl()` look up both again from
`pipe->wb` and write the result into `piece->data`. Processing starts only after all of the
pipe's commits are done, so the lookup sees white balance's final `pipe->wb`. The one exception
is a failed `DT_ILLUMINANT_CAMERA` lookup, which writes nothing. [A failed as-shot lookup keeps
the saved illuminant](#a-failed-as-shot-lookup-keeps-the-saved-illuminant) explains why that is
safe.

### A failed lookup from white balance gives D50

Every `DT_ILLUMINANT_FROM_WB` lookup goes through `_illuminant_xy_from_wb_or_D50()`
(`channelmixerrgb.c`). `commit_params()`, `process()`, `process_cl()`, and the GUI call it. If
`find_illuminant_xy_from_wb_coeffs()` fails, the helper returns D50. D50 is the pipe's own
illuminant, so chromatic adaptation then leaves the image unchanged.

The lookup fails when the image has no usable camera matrix, or when one of the first three
values in `pipe->wb.coeffs[]` is zero or is not a normal float (`_wb_coeffs_invalid()` in
`illuminants.h`). White balance allows a zero coefficient. Its params start at `0.0`, and its
sliders take their range from the params.

The fallback does not use the saved `p->x` and `p->y`. In this mode nothing keeps them up to
date. They hold the values from when the user selected the illuminant, or whatever a preset or
a pasted history stored.

Commit order still changes the illuminant that `commit_params()` stores. Take a history whose
last two entries are color calibration in `DT_ILLUMINANT_FROM_WB` mode and white balance with a
zero red coefficient:

- If color calibration commits first, its lookup uses the valid coefficients of white
  balance's previous commit, and it stores that illuminant.
- If white balance commits first, color calibration's lookup fails, and it stores D50.

Processing fails the same lookup in both cases and replaces the stored illuminant with D50, so
the two renders are the same. After `synch_top`, processing replaces the illuminant from before
the user's white balance edit in the same way. The stored illuminant has no other effect.
`commit_params()` also derives `d->p` from it, but only Bradford adaptation uses `d->p`, and
`commit_params()` always selects CAT16 for this illuminant.

### A failed as-shot lookup keeps the saved illuminant

A failed `DT_ILLUMINANT_CAMERA` lookup does not depend on commit order. The lookup scales the
image's own as-shot coefficients by ratios that are positive in the three channels the lookup
checks (`_get_d65_correction_ratios()`). So it fails only because of the image, for example
when the image has no camera matrix. It then fails at commit time too, and the illuminant
comes from `p->x` and `p->y` in every order. A D50 fallback for this illuminant would change
how existing edits render.

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
- GTK code that needs white balance's current values calls `dt_dev_get_gui_wb()`, not
  `dev->chroma.wb`.
- A `reload_defaults()` must not depend on another module's `reload_defaults()` having run
  earlier in the same walk over the modules. The walks do not all go in one direction.
  `_dt_dev_load_pipeline_defaults()` walks the last module first, so modules earlier in the
  pipe have not written shared state for the current image yet. `_pin_image()` and
  `dt_dev_reload_history_items()` walk the first module first.
- If a `reload_defaults()` reads shared state, reset that state to neutral values before the
  walk. `dt_dev_reset_chroma()` does this for `dev->chroma` before the defaults of each image
  load. Color calibration stays correct only because that reset sets `wb.coeffs[]` to `1.0`,
  which keeps the previous image's `D65coeffs[]` and `late_correction` out of the result.

---

## Key symbols

| Symbol | File | Role |
|---|---|---|
| `dt_dev_wb_t` | format.h | Holds `coeffs[]`, `D65coeffs[]`, and `late_correction`; the type of `dev->chroma.wb` and `pipe->wb` |
| `dt_dev_reset_chroma` | develop.c | Clears trouble messages, `adaptation`, and `temperature`; sets `wb.coeffs[]` to `1.0` |
| `dt_dev_init_chroma` | develop.c | Resets, then sets `wb` and `as_shot[]` to neutral; runs when the dev is created and cleaned up |
| `dt_dev_wb_set_neutral` | develop.c | Sets a `dt_dev_wb_t` to neutral; also used when a pipe is created (`pixelpipe_hb.c`) |
| `temperature.reload_defaults` | temperature.c | Writes `dev->chroma.as_shot[]` and `dev->chroma.wb.D65coeffs[]` |
| `temperature.commit_params` | temperature.c | Fills `pipe->wb` through `_wb_from_params()`; copies its `coeffs[]` and `late_correction` into `dev->chroma.wb`; writes `dev->chroma.temperature` |
| `_wb_from_params` | temperature.c | Computes the `dt_dev_wb_t` that white balance applies with the given params; used by `commit_params()` and `_gui_wb_proxy()` |
| `_gui_wb_proxy` | temperature.c | The proxy that white balance's `gui_init()` registers; returns `_wb_from_params()` for the instance's current params |
| `dt_dev_get_gui_wb` | develop.c | GUI: asks the white balance instance in `dev->iop`, through `dev->proxy.gui_wb`; copies `dev->chroma.wb` if no instance answers |
| `_update_preset` | temperature.c | GUI: when the user leaves camera reference, sets `p->late_correction` from `dt_dev_cat_is_active()` |
| `dt_dev_cat_is_active` | develop.c | GUI: asks each color calibration instance in `dev->iop`, through `dev->proxy.cat_is_active`, whether it applies chromatic adaptation |
| `_cat_proxy_is_active` | channelmixerrgb.c | The proxy that color calibration's `gui_init()` registers; returns `_applies_cat()` |
| `channelmixerrgb.reload_defaults` | channelmixerrgb.c | Computes the default illuminant from `img->wb_coeffs` |
| `channelmixerrgb.commit_params` | channelmixerrgb.c | Reads `pipe->wb` to build the chromatic adaptation matrix |
| `_get_corrected_illuminant_xy` | channelmixerrgb.c | Illuminant xy for the params' illuminant; uses the given `dt_dev_wb_t` for `DT_ILLUMINANT_CAMERA` and `DT_ILLUMINANT_FROM_WB` |
| `_declare_cat_on_pipe` | channelmixerrgb.c | Registers the instance that does chromatic adaptation in `dev->chroma.adaptation` (GUI only) |
| `_get_d65_correction_ratios` | channelmixerrgb.c | `D65coeffs[] / coeffs[]` of the given `dt_dev_wb_t`, gated by `changed_chroma` |
| `_dev_is_D65_chroma` | channelmixerrgb.c | True when `late_correction` is set or `coeffs[]` equals `D65coeffs[]` |
| `find_illuminant_xy_from_as_shot_coeffs` | illuminants.h | Default illuminant xy from `img->wb_coeffs` |
| `find_illuminant_xy_from_wb_coeffs` | illuminants.h | Illuminant xy from white balance coefficients (the `DT_ILLUMINANT_FROM_WB` illuminant) |
| `_illuminant_xy_from_wb_or_D50` | channelmixerrgb.c | Illuminant xy from `find_illuminant_xy_from_wb_coeffs()`, or D50 if that fails; used by every `DT_ILLUMINANT_FROM_WB` lookup |

## See also

- [Module_Lifecycle.md](../../Module_Lifecycle.md): the general lifecycle (load, edits, history
  replay, pipe flags) and the rule for forward and reverse module order.
- [pixelpipe_architecture.md](../../pixelpipe_architecture.md): the pipeline architecture and
  an overview of the ordering asymmetry.
