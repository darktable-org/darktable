# Display Presentation

How a rendered image reaches the screen: what the darkroom pipes render to,
the frames they publish, and the presenter that shows those frames in a
window.

See also:
- [pixelpipe_architecture.md](pixelpipe_architecture.md) - the pipes, their
  nodes and caching
- [GUI_Threading.md](GUI_Threading.md) - which thread a callback runs on
- [GUI.md](GUI.md) - GUI architecture: UI construction, events and callbacks

---

## The Rules, in Short

- [A screen pipe renders to its output description](#output-description).
  The pipe copies the description from its window when a run starts.
  Workers read `pipe->output` and never the preferences or GUI state.
- [A screen pipe publishes frames](#frames). A frame is an immutable,
  refcounted copy of the pipe's output, in the format its window asks for.
  Each other format or encoding of a frame is a rendition of it.
- [A presenting widget submits its frames with
  `dt_presenter_image()`](#presenter-api). It draws everything else with
  cairo.
- [GTK-side colors are in the device encoding of the
  window](#gtk-side-colors). Module GUIs convert with
  `dt_ioppr_get_gui_profile_info()`, not with the pipe's output profile.
- [Platform and GTK3 code stays in the presenter files](#workarounds). The
  pipeline, the modules and the views only see an output description.

## Terms

| Term | Meaning |
|---|---|
| window | the main window (`DISPLAY`) or the second darkroom window (`DISPLAY2`) |
| system display profile | the profile the platform publishes for the monitor of a window, or the display profile file |
| output | the output description of a window, a `dt_display_output_t` |
| image encoding | what the screen pipes of a window render to |
| device encoding | what untagged 8-bit values mean in a window: everything GTK draws |
| screen pipe | a pipe that has an output: the darkroom's full, preview and second window pipes, and the pipes of `dt_dev_image()` |
| frame | the image a screen pipe publishes |
| rendition | a frame in another format or encoding |
| presenting widget | a widget that shows frames through the presenter |
| backend | how a window shows frames: cairo or layered |
| cairo window, native window | a window with the cairo backend, with the layered backend |
| platform, transport | the platform-specific parts of a native window: the platform watches the window, the transport shows the layers |
| producer | code that requests the frames for a window |
| soft proof, proof profile | the darkroom's soft proof and its profile; an ink proof is a soft proof to a profile that is not RGB, such as CMYK |

## Output Description

`dt_display_output_t` (`src/common/colorspaces.h`) says what a window
wants.

| Field | Meaning |
|---|---|
| `image` | the image encoding: display-referred RGB in a profile, with 1.0 at reference white and values up to `headroom` above it |
| `device` | the device encoding |
| `format` | the format of the frames: RGB24 where cairo paints them, float where a native backend composes them |
| `serial` | a counter that changes when one of the other fields changes |

An encoding (`dt_color_encoding_t`) has a profile type, a file name, the
registry's `cmsHPROFILE`, an intent and a headroom. Type
`DT_COLORSPACE_NONE` means that no profile describes the values.

The outputs are in `darktable.color_profiles`, one for each window, written
under `xprofile_lock`. Read them with `dt_colorspaces_get_display_output()`
and `dt_colorspaces_get_display_serial()`. Both windows have an output from
`dt_colorspaces_init()` on, because the develop and its pipes exist before
the GUI, and a headless run never realizes a window.

### Who sets the output

- By default the output is the system display profile of the window
  (`_describe_system_display()`). Image and device are the same encoding:
  profile `DISPLAY` or `DISPLAY2`, relative colorimetric, headroom 1,
  format RGB24. A new system display profile publishes a new output.
- The presenter takes the output over for a native window and gives it
  back when the window is no longer native
  (`dt_colorspaces_set_window_output()`). The transport then declares the
  image and device encodings. While the presenter owns the output
  (`platform_owned`), a new system display profile is stored but not
  published.

### The system display profile

| Platform | Source |
|---|---|
| X11 | the X atom or colord |
| Wayland | colord, when darktable is built with GDK X11 support |
| Windows | the default profile of the monitor, from WCS |
| macOS | none: GTK tags its backing store as sRGB |

If the platform gives no usable profile, the system display profile is
sRGB. Examples are macOS, Windows with Advanced Color, and bytes that
littlecms rejects.

The preference `ui_last/display_profile_file` names an ICC file that
replaces the published profile for both windows
(`dt_colorspaces_set_display_profile()`). If the file cannot be read or is
not an RGB profile, darktable ignores it and shows one message for each
file. A native window ignores the file.

There is no other user selection. To preview another display, a print or
sRGB, use a soft proof. A cairo window always renders to its system display
profile: no code asks whether the platform converts the pixels again
([Known limitations](#known-limitations)).

### The registry entries DISPLAY and DISPLAY2

The registry entries "system display profile" (`DISPLAY`) and "system
display profile (second window)" (`DISPLAY2`) hold the device profile of
their window (`_entry_follow_device()`):

- on a cairo window, the system display profile;
- on a native window, the device profile the transport declares, which is
  sRGB on Wayland.

Code that names an entry by type therefore renders for what the window
shows. Examples are thumbnails with `cache_color_managed` off and the style
preview.

No menu offers the two entries. Where the platform converts, darktable
cannot tell what reaches the screen, so no result can claim to be in the
display's profile. At startup, `_drop_unoffered_choices()` replaces a
stored histogram or soft proof profile that no menu offers by sRGB.

A replaced profile stays valid for the session (`system_display_profiles`
in `dt_colorspaces_t`). Pixels made for the old screen then still convert
with the profile they were made with. For this reason an encoding names the
`cmsHPROFILE` as well as the type, and conversions take their profile info
with `dt_ioppr_add_profile_info_for_content()`. Two encodings have the same
content when their profiles are the same and their types have the same HDR
transfer function (`dt_color_encoding_same_content()`).

### What a change triggers

`_publish()` and `_output_changed()` in `colorspaces.c` announce a change
by what changed (`dt_display_output_diff()`). The rules are the same for
the presenter and for a new system display profile.

| Change | Effect |
|---|---|
| any field | the serial moves, and a redraw of the window is queued |
| image encoding or format only | nothing more: the producers of the window render again |
| device encoding (profile, intent or headroom) | the registry entry follows; the thumbnail transforms are rebuilt (main window); `DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED` with `DISPLAY` or `DISPLAY2` |
| device profile | also `DT_SIGNAL_CONTROL_PROFILE_CHANGED` |
| new system display profile on a native window | none: the output and the entry do not change |

A new [headroom target](#headroom-target) is a change of the image encoding
only.

### Producers compare the serial

Producers pull. Each producer compares the serial its image was made for
with the serial of its window, at the point where it decides to render.

| Producer | Compares | On a mismatch |
|---|---|---|
| full, preview and second window pipes, and the pinned image | `pipe->output.serial` of the last finished run, in `_pipe_outdated()` (`darkroom.c`) | requests the pipe again |
| snapshots, the duplicate manager's preview | `dt_view_get_context_hash()`, which includes the serial of the main window | render again |
| slideshow | the serial of each slot's frame, on every expose | queues a job; the slot shows its previous image until the new one arrives |

The pipe keeps the serial of a failed run too, so that a failed run is not
requested again on every redraw. The slideshow drops an image that arrives
for an earlier serial.

### What a pipe does with the output

- A pipe records its window when it is created (`pipe->window`). The full
  and preview pipes have the main window. The second window's pipe and
  every pipe of the pinned image have the second window.
- At the start of every run, `dt_dev_process_image_job()` copies the output
  of the window to `pipe->output`. A new serial also synchronizes the pipe
  again.
- colorout renders to `pipe->output.image`. It uses the profile the
  encoding names and does not look the type up again.
- `pipe->output_headroom` is the peak that sigmoid and agx tone map to. It
  follows `output.image.headroom`. With "follow output peak" on, the
  darkroom on a native window therefore depends on the headroom of the
  screen and not on the PQ peak of an export.
- `pipe->output_encoding` says what the output values of the pipe are. See
  below.
- `dt_dev_image()` takes the output of its sink and returns the frame its
  pipe published. An image encoding of type `DT_COLORSPACE_NONE` asks for
  no display encoding: the overlay filters colorout out, and its frame
  stays in the working RGB profile.
- Export, thumbnail and AI pipes have no output (`type` is
  `DT_COLORSPACE_NONE`).

`pipe->output` is the request. `pipe->output_encoding`
(`dt_color_encoding_t`) is the result:

- when colorout runs, the profile, intent and headroom of the transform it
  built, after it replaced a missing or unsupported profile by sRGB;
- when colorout does not run, or cannot build its transform on a screen
  pipe, the final working profile;
- type `DT_COLORSPACE_NONE` when no profile describes the values.

The pipe resolves the encoding at the start of every run, after
synchronization, because a history replay can commit colorin after
colorout (`_resolve_output_encoding()` in `pixelpipe_hb.c`). The scopes and
the global color picker interpret the pixels of the preview pipe with it,
and each frame carries it. `output_profile_info` is only the matrix
stand-in for code that needs matrices: it is sRGB for a profile without
matrices.

### GTK-side colors

Everything GTK draws converts to the device encoding:

- module GUIs take `dt_ioppr_get_gui_profile_info()`, the device profile
  of the main window, or sRGB for a profile without matrices;
- frames drawn by GTK take their device rendition ([Frames](#frames));
- the thumbnail transforms follow the main window;
- picker swatches convert from the encoding of the preview pipe to the
  device encoding of its window.

On a cairo window, image and device are the same encoding, so these
conversions are the identity. The exception is a frame tagged with an RGB
proof profile.

The mask and channel views, the clipping indicators and the gamut check
mark are UI tones. They are authored as device values and converted to the
`output_encoding` of the pipe (`dt_ioppr_device_to_output()`). colorout
renders the gamut check mark, device (0, 1, 1), once per commit in the
same way as a pixel. A proof narrower than the device therefore clips it.

## Soft Proof

A soft proof is an export to the proof profile. In proof mode colorout
renders the full and the preview pipe as an export to that profile. It does
not change the second window, the thumbnail pipe or exports.

- The proof has its own intent and black point compensation
  (`softproof_intent`, `softproof_bpc`), set next to the proof profile in
  the darkroom.
- The output encoding is the proof profile, and the frame is tagged with
  it. The presenter converts the frame in the same way as each other frame
  and clips what the screen cannot show.
- `output_headroom` is the headroom of the proof profile, as in an export
  to that profile. It can be above or below the headroom of the screen.
- The proof clips where an integer export clips. A float export does not
  clip. The gamut check marks what that clip changes.
- The proof of the preview pipe has no gamut marks, because the scopes, the
  color pickers, navigation and the filmstrip thumbnail read it.
- In proof mode the scopes and pickers show values in the output encoding
  of the preview, whatever the histogram setting
  (`dt_ioppr_get_scope_profile_info()`).

A proof equals a darktable export with the same settings where the export
honors them. An export differs in two points:

- An export asks for no compensation. littlecms still applies compensation
  for the perceptual and saturation intents into a v4 profile.
- The matrix path of an export ignores the intent, unless "always use
  LittleCMS 2" is on. The matrix path cannot serve a proof with the
  absolute colorimetric intent, or with compensation and a black that is
  not zero. Such a proof matches the export made with that preference on.

An ink proof converts in two steps, on the CPU. First colorout converts to
ink values with the intent and compensation of the proof, and clips them.
Then it converts the ink values to the output of the window, relative
colorimetric. A frame holds RGB, so the output encoding has the profile of
the window. The scopes then show the values in that profile.

## Headroom Target

The image headroom of a native window follows a target that the presenter
core settles. It does not follow the raw value from the platform. Each
published change of the image headroom reruns the pipes of the window from
the start, because `output_headroom` is in the hash of every cache line.
Platforms send many values during one brightness change: KWin sends one
description per frame of its animation.

The platform reports with `dt_presenter_window_report_headroom()`
(`dt_presenter_headroom_t` in `gui/presenter_target.h`):

| Field | Meaning |
|---|---|
| `value` | the peak of the screen over reference white now, or NAN when the platform has no value |
| `ceiling` | the most the screen can show in the window, or 0 when the platform does not say |
| `generation` | changes when earlier reports say nothing about this one: another screen or mode, a display that wakes up |

A report never reaches the pipes. The policy is one pure function on a
clock that the caller passes in (`gui/presenter_target.c`, tested by
`test_presenter_target`):

| Rule | Value |
|---|---|
| level | log2 of the value, clipped to the ceiling, rounded down to 1/8 EV, at least 0 EV |
| a higher level counts | when the value is half a step above it |
| a lower level counts | always, because the screen then shows less than the pipes render for |
| quiet time | 1 s since the reported value last changed |
| maximum latency | 3 s after the first report outside the band of the target |

- The core takes a new level at the earlier of the two times.
- If the value returns into the band of the target before that, the core
  drops the waiting level. Nothing is published and no pipe reruns.
- A report without a value keeps the target and drops the waiting level.
- A new generation, and the first report of a window, forget the band. The
  target is 1.0 until the first value arrives, and the core takes that
  value at once. The ceiling never replaces a value.

A settled change goes to the backend's `target()`. The backend prepares to
show frames rendered for the new target. Then it publishes the output of
the window with the headroom it can show, through
`dt_presenter_window_set_output()`. On Wayland, see [Wayland](#wayland).

- Until the producers of the window deliver, its frames keep the tone
  mapping they were rendered with. The transport clips them to the range
  of the image description in use.
- A cairo window has headroom 1 and publishes no target. The target
  continues to settle from the reports, so a window that becomes native
  again starts at the headroom of the screen.
- When the core frees the platform, the settle timer stops and the target
  is 1.0 again.

The constants are hypotheses. To measure them, run with
`-d control -d verbose`, which logs every report with its time and every
settled target. The intervals between the reports of one gesture give the
quiet time. The longest ramp of the platform gives the maximum latency.
The noise of a screen that nobody touches gives the hysteresis.

## Frames

`src/develop/frame.h`. gamma is the last module of every pipe and writes
float RGBA: the pixel as colorout encoded it, with alpha 1, or the mask and
channel views as display values. A screen pipe publishes that buffer as a
frame, `pipe->frame`, written under `backbuf_mutex`. Take a reference with
`dt_dev_pixelpipe_frame()`.

A frame is a copy, because the cache line it comes from is not pinned. It
never changes after it is made. It holds:

- the pixels, in the format of the pipe's output: float RGBA, or RGB24
  packed on the worker with `dt_display_to_8bit()` into cairo's BGRX;
- its encoding, the `output_encoding` of the pipe: what colorout produced,
  not what the pipe was asked for (`dt_frame_encoding()`);
- the serial of the output it was made for (`dt_frame_made_for()`), and an
  id, a 32-bit count of the frames made;
- its geometry (`dt_frame_geometry()`): size, scale and zoom position.

The format belongs to the frame, not to the pipe cache. gamma and every
cache line stay float, no hash includes the format, and only the publish
step packs. A new format is a new image (`dt_display_output_diff()`), so
the producers of the window render again.

Exports, thumbnails, AI restore and pixel masks do not run screen pipes.
Their pipes keep `pipe->backbuf`, the output of the last module.

### Renditions

`dt_frame_surface()` (RGB24) and `dt_frame_float()` return a rendition.

- A rendition is made from the frame's own pixels when it is first asked
  for, never from another rendition. It is kept with the frame.
- Any thread can ask.
- In the frame's own format and profile, the rendition is the frame's
  pixels, whatever the headroom.
- A conversion is built from the profiles that the two encodings name.
  Those profiles live for the session, so a frame can outlive the develop
  that made it.
- A conversion uses the intent of the encoding it converts to.
- Matrix profiles go through darktable's matrix and curves, others through
  littlecms. An RGB24 rendition is clipped after the matrix.

GTK-side drawing takes a reference on the frame and draws
`dt_frame_device_surface()`, the RGB24 rendition in the device encoding of
the window. Its users are navigation, the filmstrip thumbnail of the edited
image, the correction cursor and focus peaking.

The worker makes the device rendition when it publishes a frame, so that
the GUI thread does not convert:

- always for the preview pipe;
- for every darkroom screen pipe while focus peaking is on.

On a cairo window the device rendition is the frame's own pixels, unless
the frame is in another profile, such as an RGB proof.

A draw converts on the GUI thread where the worker did not. Examples are
snapshot frames under focus peaking, and frames whose window changed its
device encoding while their pipe ran.

## Presenter API

`src/gui/presenter.h`. A presenting widget wraps its draw handler:

```c
cairo_t *pcr = dt_presenter_begin(widget, cr);
// draw everything on pcr, in paint order
dt_presenter_end(widget, pcr);
```

Inside, `dt_presenter_image(pcr, x, y, frame, filter)` replaces
`cairo_set_source_surface()` and `cairo_paint()` for an opaque image.

- It uses the current transformation and clip of the context. The clip can
  have any shape.
- Paint order holds: what the widget draws afterwards is on top.
- The presenter holds a reference on the frame while it shows or keeps it.
  The caller can drop its own reference when the call returns.
- The window shows the frame in the image encoding of its output. cairo
  paints the RGB24 rendition, and a native backend composes the float
  rendition.
- A frame in another profile or format, made before the output changed, is
  shown converted and never refused. It stays until its producer renders
  it again ([Producers compare the
  serial](#producers-compare-the-serial)).

A widget can keep what it showed, to show it again while the next image
loads:

- `dt_presenter_retain()` marks the draw in progress. If the draw is full,
  everything it shows becomes the kept presentation of the widget: its
  images and all that is drawn around and above them.
- `dt_presenter_keep()` paints the kept presentation where it was, over
  the area the widget had then. It replaces what the draw has drawn there
  so far, and the rest of the draw goes on top. It returns FALSE when
  there is no kept presentation.
- `dt_presenter_clear()` drops the images and the kept presentation of a
  widget (darkroom `leave()`). Unmapping the widget does the same.

A draw is full when its clip is one rectangle over the whole widget. A draw
that GTK clipped to a damaged area does not replace or drop the kept
presentation. A full draw that does not retain itself and does not show the
kept presentation drops it.

The darkroom retains its full image draws while the loading screen
preference (`darkroom/ui/loading_screen`) is off. It shows the kept
presentation, under the loading toast, while the next image loads.

`dt_view_paint_surface()` is the darkroom's user: it draws the background,
then the preview fallback and the main image through the presenter.
Pickers, masks, module overlays, guides and focus peaking follow on the
same context.

The presenting widgets are the center view and the side-by-side snapshot
area (`gui/gtk.c`, `_draw()`), and the second darkroom window. The images
submitted there are the darkroom main image and its preview fallback,
snapshots, the duplicate manager's preview, and the slideshow.

## Backends

A window has the layered backend while its platform color-manages the
tagged content of the window and the transport can be set up. Otherwise it
has the cairo backend. With the preference `ui_last/display_force_cairo`
set (preferences > miscellaneous), every window is a cairo window. That
preference is an escape hatch for compositor or driver bugs.

The platform (`dt_presenter_platform_t` in `gui/presenter_backend.h`)
watches the window whatever its backend. Its verdict can change while the
window exists. For example, a Wayland output switches its color mode, or
the window moves to a monitor in another mode. The platform reports a new
verdict with `dt_presenter_window_choose()`.

One function decides the backend and the platform: `_choose()` in
`gui/presenter.c`. It runs at once when the first presenting widget of the
window draws. Later requests run it from an idle, one per window, that
`dt_presenter_window_choose()` queues. The old backend stays callable
until that idle runs. In order, a choice:

1. reads the force preference and remembers it;
2. makes the platform, if there is none and the window is not forced and
   not failed. If that setup fails, the window is failed;
3. wants native only if the window is not forced, not failed, has a
   platform, and the verdict of the platform says so;
4. replaces the backend only if there is none yet or it is of the other
   kind. The backend data of every presenting widget is dropped, kept
   presentations included, and the old backend is freed. If the layered
   backend cannot be set up, the window is failed and gets the cairo
   backend. A cairo backend gives the output back to the system display
   profile. A layered backend's transport reports the output during its
   setup;
5. frees the platform if the window is forced or failed, and resets the
   [headroom target](#headroom-target);
6. draws the window again if it replaced an existing backend.

After every choice, the platform exists only while the window can be
native. A forced window makes no color-management requests.

A failed window does not try native again on later verdicts. It tries again
only when the force preference changes. Closing the preferences dialog
(`DT_SIGNAL_PREFERENCES_CHANGE`) compares the preference with the value
the window follows.

With `-d control`, the core writes one line for each backend change and
each failed native attempt: `native`, or `cairo:` with the cause. Where no
transport exists, as on X11, macOS and Windows today, a window has no
platform and is always a cairo window.

The core, `gui/presenter.c`, has no branch on the backend. It keeps the
widget records, the references on the frames of each widget's last draw,
the test for a clipped draw, and the matrix of each image. A backend
(`dt_presenter_backend_t` in `gui/presenter_backend.h`) begins and ends a
draw, shows an image, paints the kept presentation, clears a widget, and
prepares for a new headroom target with `target()`.

### cairo

`gui/presenter_cairo.c`. The widget draws on GTK's own context, and the
backend paints the RGB24 pixels of each frame there. Frames for a cairo
window are made in RGB24. A frame in the encoding of the window is
therefore not converted and not copied on the GUI thread. A frame made for
an earlier output is converted there once.

The kept presentation is what GTK's target holds at the end of the retained
draw. On X11 the backend holds a reference on that target. On the other
platforms it copies the widget's part into a surface of its own.

### Layered

`gui/presenter_layers.c`. The layered backend shows the images of a window
on surfaces of its own, above GTK's surface:

```
  foreground layer   8-bit, untagged    pickers, masks, guides, overlays
  image layer        16-bit, managed    darkroom image, snapshots
  GTK's surface      8-bit, untagged    panels, thumbnails, everything else
```

- The image layer is premultiplied FP16 or UNORM16, in the image encoding
  of the window. Its alpha is 0 where there is no image.
- The foreground layer is premultiplied 8-bit and untagged. It holds
  everything the presenting widget draws.
- The layers are above GTK's surface and not below it, because
  DirectComposition on Windows always composes above the GDI content of a
  window.
- The two layers take no input, so GTK receives all of it.
- A presenting widget that draws no image draws on GTK's surface, and its
  layers are unmapped. Examples are lighttable, map, and a darkroom that
  loads its first image.

#### The two-layer rule

The two layers together show what cairo would paint on GTK's surface, in
paint order. Let `F` be the foreground layer, `N` the image layer, `G`
GTK's surface, `R` the result cairo would paint, and `L` the share of the
images in `R`. The backend keeps `R = F + L` and `N = L / (1 - alpha_F)`.
Then `F over (N over G)` is `R + (1 - alpha_R) G`.

- A paint that is not an image changes `F` only.
- An image paint of coverage `a`, over a foreground of alpha `b`, cuts the
  foreground: `F <- (1 - a) F`.
- The same paint moves the image layer towards the image `I`:
  `N <- N + w (I - N)` with `w = a / (a + (1 - a)(1 - b))`, on all four
  channels, with alpha 1 for the image.

`w` is in [0, 1], so the alpha of `N` stays in [0, 1] and its color stays
in the clip range of the encoding. The rule makes no assumption about the
backdrop, which can be translucent or missing.

The coverage `a` is the alpha cairo itself paints for the image, through
the caller's clip and with the image's matrix and filter. The backend makes
it when an image is placed, one byte per device pixel over the box of the
image. `test_presenter_compose` compares it with cairo's paint of the whole
widget; the comment there gives the tolerances.

The image layer is composed only when what it shows changes. It is
composed straight into the transport's buffer. The matrix of the encoding
applies before the one quantization, so overlapping images round once.

The kept presentation is the foreground of the retained draw, with its
images cut out, and the images of that draw. A presentation kept at
another device scale is dropped. While a widget retains its draws, they
alternate between two foreground surfaces, so keeping needs no copy.

#### Replay

What GTK3 draws above a presenting widget is in GTK's surface, under the
layers. The backend therefore draws it again into the foreground when it
presents (`_replay_above()`):

- the children that each GtkOverlay between the widget and the toplevel
  stacks above the widget, such as toasts;
- the popovers of the toplevel, unless the transport shows them above its
  layers (`popovers_above`), as on Wayland.

A popover is not part of a presenting widget, so the backend watches each
popover it replays. When a popover is shown, hidden, moved or animated, or
its content changes, the presenting widgets of the window draw again.

#### Platform and transport

`gui/presenter_<platform>.c` holds the platform and the transport
(`dt_presenter_transport_t` in `gui/presenter_layers.h`).

- The platform says whether the window is native, and reports the headroom
  of the screen.
- The transport reports the output of the window with
  `dt_presenter_window_set_output()`, at setup and at every change. The
  output has the image encoding and, as device encoding, the profile that
  GTK's own surface is shown in.
- A present is one transaction. `reserve()` gives the storage of the two
  layers and the encoding of the image layer. The layered backend writes
  the layers, and `commit()` shows them.
- The encoding of the image layer (`dt_presenter_encoding_t`) is a clip
  range for straight color in the output of the window, then a 3x3 linear
  map. A transport can thus encode in other primaries or with another
  white level.
- The transport can refuse a reservation while the compositor holds its
  buffers. Nothing of that present is shown, and the widget draws again
  on the next frame.
- The transport decides when to call `dt_presenter_window_present()`: at
  the point where the layers become visible together with GTK's surface
  for that frame, or later, and never earlier.

That point is verified on Wayland only. On macOS and Windows a future
backend must find and verify it.

#### Costs and limits

For each presenting widget, a native window needs more memory than a cairo
window:

- two foreground surfaces of 4 bytes per device pixel, or one while
  nothing is retained;
- the buffers of the transport: on Wayland two for each layer, of 8 and 4
  bytes per device pixel;
- one byte per device pixel for the coverage of each placed image, and one
  byte per pixel of the largest frame shown.

Each present copies the foreground into the transport's buffer and draws
the replay into it. Composing the image layer takes about one sample per
device pixel of each image's box.

Two properties differ from a single cairo surface:

- At pixels of fractional coverage, the compositor blends the layers in
  its own blending space and not in cairo's encoded values. GTK4's
  offloaded subsurfaces and Chromium's underlays have the same property.
- The rounding of the 8-bit foreground can change `N` by one code value
  under later paints.

### Wayland

`gui/presenter_wayland.c`, built with `USE_WAYLAND`. A native window needs:

- `wl_compositor` version 4;
- `wp_color_manager_v1` with parametric descriptions, `ext_linear` and
  BT.2020 primaries;
- `ABGR16161616F` or `ABGR16161616` shm buffers. FP16 is preferred for its
  precision near black;
- `set_luminances`, optional: without it the output is SDR.

Without these the window is a cairo window.

The platform reports the headroom from the preferred description of the
surface: target max luminance (else max luminance) over reference
luminance. This value is a hint, not a measurement.

- Without a reference luminance there is no value (NAN).
- Without `set_luminances` the ceiling is 1.
- A preferred description that differs in more than its luminances starts
  a new generation. KWin's HDR brightness moves only the reference
  luminance.

The transport describes its image layer for the
[headroom target](#headroom-target), never for the raw preference:
`ext_linear`, BT.2020, luminances (0, max, 203) with
max = ceil(headroom x 203).

- A value v is sent as v x 203 / max, clipped to [0, max / 203].
- The output of the window is linear BT.2020 with headroom max / 203, the
  range the transport clips to.
- A new description is published when it is ready. It applies with the
  next commit of the image layer, and the window draws again.
- If a new description fails, the description in use stays.
- If the first description fails, the window is failed and stays on cairo.

The layers are synchronized subsurfaces of GDK's surface. They are directly
above it and below GTK's own subsurfaces such as popovers, which therefore
need no replay. The transport presents at the end of the frame clock's
`paint`, so the layers apply with GDK's commit in `after-paint`.

With `DT_PRESENTER_REPLAY_POPOVERS` set in the environment, the transport
places the layers above the popovers. The popover replay of the layered
backend can then be tested on Wayland.

mutter's default color mode is left to cairo
(`_mutter_default_color_mode()`). In that mode mutter passes untagged
pixels to the panel unchanged, and applies the profile assigned in
Settings > Color only as a gamma ramp (mutter#4597). The window is then a
cairo window on its system display profile, which colord gives, as on X11.
The platform recognizes the mode by three conditions:

- the registry announces `gtk_shell1`;
- `XDG_CURRENT_DESKTOP` lists `GNOME`;
- the preferred description of the surface is plain SDR sRGB.

The test runs on every preferred description, so a window follows a switch
of the color mode or a move to another monitor.

## Known Limitations

- **XWayland.** darktable on XWayland is an X11 client and cannot see the
  description of the compositor. It renders to the X atom or to colord's
  profile. If the compositor also converts untagged pixels to that
  profile, as with xiccd, the image is converted twice. mutter outside its
  default color mode, before mutter !5177, reads untagged pixels as sRGB.
- **Profile reassigned in the system settings.** While the window stays on
  its monitor, darktable finds a new profile only at the next configure
  event that queries again: a move of the main window, or any configure on
  Wayland or of the second window. A persistent `CdWindow` per window that
  listens to its changed signal, and a listener on the X atom, would
  remove this limit.
- **Display profile file.** The file is applied as it is. On macOS, and on
  Windows with automatic color management or HDR, the system converts the
  image again from sRGB, so the colors are converted twice.
- **sway with `color_profile`.** sway describes its output as sRGB and
  converts everything to the profile. A native window leaves that
  conversion to sway, so its image is limited to sRGB. A cairo window on
  that output converts a second time when a tool such as xiccd also
  publishes the profile.
- **Second window in proof mode.** While the second window pans or zooms,
  it can show the frame of the main preview as a fallback. That frame is
  proofed and the second window's own frame is not.
- **Hidden toplevel.** A presenting toplevel on Wayland cannot be shown
  again after it is hidden, because GDK destroys its `wl_surface`.
  darktable has no such path: the main window is hidden only at quit, and
  the second window is destroyed after it is hidden.

## Workarounds

Each workaround is in one file and goes away with the limitation it works
around. GTK4 removes the layers only on Wayland. It has no output color
states on Windows and macOS, so a native backend there keeps its layers
under GTK4.

| Workaround | Where | Works around | Removed when |
|---|---|---|---|
| image and foreground layers above GTK's surface | `presenter_layers.c` | GTK3 cannot give a widget a surface with a color state | GTK4: the image is a texture with a color state |
| replay into the foreground of what GTK stacks above a presenting widget: GtkOverlay children, and popovers where the layers cover them | `presenter_layers.c`, `_replay_above()` | the layers cover GTK's drawing of those widgets | same |
| compose and present in the frame clock's `paint` signal, after GDK's handler | `presenter_wayland.c`, `_paint()` | GTK3 runs all draws in `paint`, and GDK commits its surface in `after-paint` | same |
| GTK's surface taken as sRGB, the device encoding of the window | `presenter_wayland.c`, `_set_output()` | GTK3 does not tag its surface. The compositor reads an untagged surface as sRGB, with a transfer function of its choice | GTK tags its surfaces |
| mutter's default color mode left to cairo with the system display profile | `presenter_wayland.c`, `_mutter_default_color_mode()` | that mode composites as sRGB and does not apply the profile of the screen (mutter#4597) | mutter describes its real output (mutter !5177). The test then no longer matches |
| no opaque region on the layers | `presenter_wayland.c`, `_layer_init()` | a compositor can stop frame callbacks for a surface it considers occluded, which stops GTK's frame clock | GTK4 |
| surface feedback destroyed when the toplevel unmaps | `presenter_wayland.c`, `_toplevel_unmap()` | GDK destroys its `wl_surface` when a window is hidden. A request on the feedback is then a protocol error | GTK4 |
| a clipped draw does not replace or drop the kept presentation | `presenter.c`, `dt_presenter_begin()` | GTK3 redraws only the damaged part of a widget, so such a draw records clipped images | GTK4: the snapshot of a widget is always whole |
| the kept presentation of a cairo window is a reference on GTK's target on X11, and a copy elsewhere | `presenter_cairo.c`, `dt_presenter_cairo_new()` | GTK3 keeps no rendering of a widget. The next frame overwrites the target on Wayland and Quartz, and the target cannot be held on Win32 (#20831) | GTK4: the render nodes of a widget are kept between frames |

## Scope

The presenter shows the darkroom and the slideshow. In the darkroom that is
the main image, the preview fallback, snapshots, the duplicate manager's
preview, and the second window with its pinned image.

Everything else stays on GTK's 8-bit surface: lighttable, filmstrip,
culling and full preview, navigation, and all thumbnails. On a native
window the compositor shows them as untagged content, without wide gamut
or HDR. The protocol leaves the transfer function of untagged surfaces to
the compositor. Where the compositor decodes them with gamma 2.2, as KWin
does, their shadows are darker than those of the image.

Thumbnails stay on GTK's surface for two reasons:

- Thumbnails have GTK widgets on top (stars, labels, badges, buttons). On
  the layers, most of the lighttable would be replayed on every scroll.
- The mipmap cache is 8-bit sRGB or AdobeRGB. Wide gamut and HDR
  thumbnails need a float thumbnail cache first.

In GTK4 each thumbnail can be a texture with a color state. See
darktable-org/darktable#20477.

With `cache_color_managed` off, the mipmap cache holds thumbnails encoded
for the `DISPLAY` registry entry, the device encoding of the main window.
Thumbnails already in the cache keep the profile they were encoded for
when that entry changes; the description of the option warns of it.

## Common Mistakes

| Mistake | Instead |
|---|---|
| painting a frame with cairo in a presenting widget | `dt_presenter_image()` with the frame |
| reading `pipe->backbuf` of a screen pipe | `dt_dev_pixelpipe_frame()`; only the other pipes keep a backbuf |
| keeping the pixels or renditions of a frame after dropping its reference | hold a reference (`dt_frame_ref()`) for as long as you can draw it |
| reading `darktable.color_profiles` or `pipe->output` to learn what a pipe's pixels are | `pipe->output_encoding` or the encoding of the frame; `pipe->output` is the request |
| converting the colors of a module GUI with the pipe's output profile | `dt_ioppr_get_gui_profile_info()` |
| converting a frame for GTK-side drawing yourself | `dt_frame_device_surface()`, made once and kept with the frame |
| converting with the profile infos of a develop outside that develop | build them from the profile of the encoding, as `frame.c` does: a temporary develop frees its list |
| testing for a platform in the pipeline, a module or a view | put the code in `presenter_layers.c` or in the transport of the platform, with a row in [Workarounds](#workarounds) |
