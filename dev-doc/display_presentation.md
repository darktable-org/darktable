# Display Presentation

How a rendered image reaches the screen: what the darkroom pipes render to,
the frames they hand over, and the presenter that shows those frames in a
window.

See also:
- [pixelpipe_architecture.md](pixelpipe_architecture.md) - the pipes, their
  nodes and caching
- [GUI_Threading.md](GUI_Threading.md) - which thread a callback runs on
- [GUI.md](GUI.md) - GUI architecture: UI construction, events and callbacks

---

## The Rules, in Short

- [A presented pipe renders to its output description](#output-description),
  taken from its window when the run starts. Workers read `pipe->output`;
  the display profile preferences can change under a running pipe.
- [The pipe does not quantize](#float-backbuf). The screen backbuf is float,
  and 8-bit values are made where an 8-bit surface is made, with
  `dt_display_to_8bit()`.
- [A presenting widget submits its images with
  `dt_presenter_image()`](#presenter-api) and draws everything else with
  cairo as before.
- [GTK-side colors are in the window's device
  encoding](#output-description): module GUIs convert with
  `dt_ioppr_get_gui_profile_info()`. The pipe's output profile describes the
  image layer, which on a native backend is not what GTK draws in.
- [Platform and GTK3 code stays in the presenter files](#workarounds). The
  pipeline, modules and views only see an output description.

## Output Description

`dt_display_output_t` (`src/common/colorspaces.h`) is what a window wants.
`image` is the encoding its pipes render to: display-referred RGB in a
profile (type, file, intent), 1.0 at reference white and up to `headroom`
above it. `device` is what untagged 8-bit values mean in the window,
everything GTK draws there. `platform_owned` says a backend reported it,
so a new system display profile does not replace it. Where image and device are the same
encoding, as on every cairo window, the values reach the screen as device
values. The mask and channel views are UI tones, authored as device values
and converted from the device to the image encoding by gamma, which
prepares the conversion when it commits; the gamut check and clipping
markers are written as they are, saturated colors that mark in any output.

- Both windows have an output at all times, from `dt_colorspaces_init()` on:
  develop and its pipes exist before the GUI, and headless runs never
  realize a window. It is stored in `darktable.color_profiles`, indexed by
  window and written under `xprofile_lock`, and its `serial` changes
  whenever anything else in it does.
- The display profile always comes from the platform: the X atom or colord
  on X11 and on Wayland without color management, the compositor through
  a backend, `GetICMProfileW()` on Windows, and sRGB on macOS, where GTK
  tags its backing store sRGB. There is no user selection; previewing
  another display, print or sRGB is a soft proof. Unless the platform owns
  it, a window's output is its system display profile (`DISPLAY` or
  `DISPLAY2`) as image and device encoding, with the display intent
  preference (`ui_last/display_intent`, one for both windows) and headroom
  1 (`_describe_system_display()` in `colorspaces.c`). A new system display
  profile and a change of the intent preference
  (`dt_colorspaces_update_display_intent()`) republish it.
- What GTK draws converts to the device encoding: the thumbnail transforms
  follow the main window's, picker swatches convert from what the preview
  pipe produced to its window's, and module GUIs take
  `dt_ioppr_get_gui_profile_info()`, the main window's device profile, or
  sRGB for a profile without matrices. On a cairo window device and image
  are the same encoding, so these conversions are the identity there.
- The presenter takes a window's output over for the platform, and gives
  it back when the window goes away (`dt_colorspaces_set_window_output()`).
- A change is announced by what changed (`dt_display_output_diff()`), the
  same way for the presenter, a new system display profile and the intent
  preference (`_output_changed()` in `colorspaces.c`). Any change moves
  the serial and queues a redraw of the window, where its producers find
  it. A new device encoding or owner also rebuilds the thumbnail
  transforms and raises `DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED` with
  `DISPLAY` or `DISPLAY2`, for what GTK draws there: thumbnails (main
  window only) and module GUI colors. A new image encoding alone, such as
  a new headroom, reruns that window's producers and nothing else.
  `DT_SIGNAL_CONTROL_PROFILE_CHANGED` is not raised for a window's output.
  It announces a new system display profile, the registry's `DISPLAY` or
  `DISPLAY2` entry, whether a window follows it or not, for what names the
  profile by type: a soft proof, the histogram or export profile, the main
  window naming `DISPLAY2`. colorout reprocesses the darkroom on it, so a
  new screen profile on X11 reruns both windows' pipes, as it always did; a
  new headroom never touches the registry.
- Producers pull. Each compares the serial its image was made for with
  its window's where it already decides to render: the darkroom's full,
  preview and second window pipes, a pinned image's too, compare the serial
  their last finished run took (`pipe->output.serial`, published or not, so
  a failed run is not retried on every redraw) in their request
  (`_pipe_outdated()` in `darkroom.c`); snapshots and the duplicate manager's preview through
  `dt_view_get_context_hash()`, which holds the main window's serial; the
  slideshow on every expose, over all its slots. An image made for an
  earlier serial is not taken as current: the slideshow drops it on
  arrival and keeps showing the slot's previous image until the new one
  arrives, and a pipe whose run took an earlier serial is requested again.
- A pipe records the window it renders for where it is created
  (`pipe->window`): full and preview the main window, the second window's
  pipe and every pipe of the pinned image the second one. They take their
  window's output at the start of every run, in
  `dt_dev_process_image_job()`, so workers never read GUI state while
  processing. A new serial resynchronizes the pipe, a new screen profile
  for the system display profile included.
- `dt_dev_image()` sets the output of its own pipe and returns the
  encoding of its buffer. `NULL` means no display encoding: with colorout
  filtered out, as overlay does, the buffer stays in the pipe's working RGB.
- colorout renders to `pipe->output`, with the profile the run took
  (`profile`), not one it looks up again by type. The pipe's `output_headroom`, which
  sigmoid and agx tone map to, honors `output.headroom`. A soft proof sets
  it to the proof profile's headroom, as an export to that profile does,
  above or below the screen's, and the output encoding says so. The
  presenter clips what the screen cannot show: the Wayland transport to the
  range of its image description, cairo the encoded values to 8 bits.
- `pipe->output_encoding` (`dt_color_encoding_t`) is what the pipe's output
  values are: the profile, intent and headroom of the transform colorout
  built, after it replaced a missing or unsupported profile by sRGB, or the
  final working profile when colorout does not run. colorout records its
  transform in `commit_params()`, and nothing (type `DT_COLORSPACE_NONE`)
  when building it fails or no profile describes what it writes; the pipe
  resolves the encoding when a run starts, after synchronization, since
  history replay can commit colorin after colorout. The scopes and the
  global color picker interpret the preview pipe's pixels with it, and it
  labels the backbuf and `dt_dev_image()`'s buffer. `output_profile_info`
  stays the matrix stand-in for code that needs matrices: sRGB for a LUT
  profile.
- An encoding and an output name the registry's profile (`profile`) as
  well as its type. A new screen profile replaces the system display
  profile under the same type and file name, and the replaced one stays
  valid for the session (`system_display_profiles` in
  `dt_colorspaces_t`), so pixels made for the old screen keep converting
  with the profile they were made with: conversions take their profile
  info with `dt_ioppr_add_profile_info_for_content()`. For every other
  type, type and file name are the identity
  (`dt_colorspaces_replaced_in_session()`). A query that yields no usable
  profile (no X atom and none from colord, an empty Windows query, bytes
  lcms rejects) makes the window's system display profile sRGB again, the
  profile it starts with, rather than leaving the previous screen's. A
  new system display profile that changes a window's output changes its
  device encoding, so thumbnails drawn before it arrived, as colord's
  asynchronous answer at startup, are redone.
- A soft proof shows what an export to the proof profile holds, in the
  output: converted to the proof profile with its own intent
  (`softproof_intent`, set next to the proof profile in the darkroom), and
  from there to the output relative colorimetric; clipped where that export clips, and by the presenter to what the
  screen shows. PQ and HLG proofs take the export's own encoding round trip
  in linear light (`dt_hdr_encode()`, `dt_hdr_decode()`), since their ICC
  shapers end at 1.0, where the pipe has reference white.
- Export, thumbnail and AI pipes have no output description (`type` is
  `DT_COLORSPACE_NONE`).

## Float Backbuf

gamma is the terminal module of every pipe and writes float RGBA: the pixel
as colorout encoded it, with alpha 1 (channel 3 carries mask data inside the
pipe), or its mask and channel views as display values. A screen pipe copies
that into `pipe->backbuf`, labels it with `backbuf_output`, its
`output` with `output_encoding` as its image, and gives the frame a
new `backbuf_id` (`dt_dev_new_frame_id()`).

Nothing quantizes inside the pipe. 8-bit values are made where an 8-bit
surface is made, with `dt_display_to_8bit()`: by the presenter, by
`dt_presenter_gtk_surface()` for GTK-side drawing (navigation, the filmstrip
thumbnail of the edited image, the correction cursor, focus peaking), by the
overlay module, and by the 8-bit export path. GTK-side drawing draws the
same frame on every draw, so `dt_presenter_gtk_surface()` converts it once
per frame id and keeps the last few; matrix profiles go through darktable's
matrix and curves, others through lcms.

## Presenter API

`src/gui/presenter.h`. A presenting widget wraps its draw handler:

```c
cairo_t *pcr = dt_presenter_begin(widget, cr);
// draw everything on pcr, in paint order
dt_presenter_end(widget, pcr);
```

Inside, `dt_presenter_image(pcr, x, y, pixels, width, height, encoding, id,
filter)` takes the place of `cairo_set_source_surface()` + `cairo_paint()` for
an opaque image: it uses the context's current transformation and the extents
of its clip, and paint order is kept, so whatever is drawn afterwards is on
top. The pixels are converted once per `id` into a frame the presenter owns,
so the caller may release the buffer (and its lock) when the call returns. A
frame is its `id` and encoding: ids are never reused. It returns `FALSE`, and
shows nothing, when the platform owns the window's output (a native backend,
or cairo on a compositor that manages color) and the frame is in another
profile. The caller does nothing about it: taking the output moves the
window's serial and redraws it, and the producers render again where they
compare it ([output description](#output-description)). Otherwise the cairo
backend shows any display-encoded frame.

- `dt_presenter_keep()` shows the images of the last full draw again where
  they were (the darkroom while it loads the next image without the loading
  screen). A draw GTK restricted to a damaged area does not replace them.
  Kept images in a profile other than the output's are dropped while the
  platform owns it.
- `dt_presenter_clear()` drops a widget's images (darkroom `leave()`);
  unmapping a widget drops them too.
- `dt_view_paint_surface()` is the darkroom's user: background, then the
  preview fallback and the main image through the presenter. Pickers, masks,
  module overlays, guides and focus peaking follow on the same context.

Presenting widgets: the center view and the side-by-side snapshot area
(`gui/gtk.c`, `_draw()`), and the second darkroom window. Images submitted
there: the darkroom main image and preview fallback, snapshots, the
duplicate manager's preview, and the slideshow.

## Backends

A window has one backend, chosen when its first presenting widget draws
(`gui/presenter_layers.c`, `_window_get()`): a native one if the platform has
one and it can set up, otherwise cairo.

### cairo

Paints each frame, quantized to an RGB24 surface, into the widget's own
context. The window's output is its system display profile (display2 for
the second window) as image and device encoding, the same output a window
has before it is realized and after it is gone.
Every platform without a native backend uses it: X11, macOS, Windows, and
Wayland without color management.

A Wayland compositor that manages color but lacks what the native backend
needs also gets cairo. It takes GTK's untagged surface as sRGB, with a
transfer function of its choosing, and converts it to the screen. The
Wayland setup reports that the compositor manages color, and the backend
choice then gives the platform the window's output with sRGB as image and
device encoding (`dt_presenter_window_set_output()`), as a native backend
does with its own.

### Native backends and their layers

A native backend shows a window's images on surfaces of its own above GTK's
surface, from bottom to top:

1. GTK's surface: everything GTK draws, untagged.
2. The image layer: float, in the window's output, alpha 0 off the images.
3. The foreground layer: premultiplied 8-bit, untagged like GTK's surface,
   with everything the presenting widget draws. Where an image is, its
   pixels are cleared, so paint order holds across the two layers.

```
  foreground layer   8-bit, untagged    pickers, masks, guides, overlays
  image layer        float, managed     darkroom image, snapshots
  GTK's surface      8-bit, untagged    panels, thumbnails, everything else
```

Both layers take no input, so GTK below keeps getting all of it. A presenting
widget that draws no image puts its drawing on GTK's surface as cairo would,
and its layers are unmapped (lighttable, map, a loading darkroom).

The backend reports the window's output, its image encoding and, as
device, the profile GTK's own surface is shown in, through
`dt_presenter_window_set_output()`, at setup and whenever it changes. The
pipes of that window then render to that output, and everything GTK draws
there (thumbnail transforms, picker swatches, the navigation thumbnail,
`dt_presenter_gtk_surface()`, and module GUIs through
`dt_ioppr_get_gui_profile_info()`) converts to the device profile, sRGB,
where a cairo window's device profile is its system display profile. A
new headroom changes the image encoding only: that window's images are
rendered again, thumbnails are not. When the window goes away, its output
returns to its system display profile.

Code is split by what it works around:

- `gui/presenter.c`: the API, owned frames, retained images, the cairo
  backend.
- `gui/presenter_layers.c`: the GTK3 part every native backend shares. It
  composes the image layer and the foreground, and computes their size,
  scale and position in the toplevel. A GTK4 port, where widgets draw
  textures with color states, deletes it.
- `gui/presenter_<platform>.c`: transport and timing. Setup and fallback,
  output description and headroom, encoding the composed image for the
  compositor, buffers, and `present()`, `clear()`, `release()` and `free()`
  (`dt_presenter_backend_t` in `gui/presenter_layers.h`). The backend calls
  `dt_presenter_window_present()` where the layers become visible together
  with GTK's surface for that frame, or later, never earlier. `present()` may refuse while the compositor holds
  its buffers; the layers are then composed and presented again next frame.

### Wayland

`gui/presenter_wayland.c`, built with `USE_WAYLAND`. It needs `wl_compositor`
4 and `wp_color_manager_v1` with parametric descriptions, `ext_linear`, BT.2020
primaries and `ABGR16161616F` shm buffers; `set_luminances` is optional
(without it the output is SDR). Without the rest, a compositor with
`wp_color_manager_v1` gets the window on cairo in sRGB (see cairo). The output
is linear BT.2020 with headroom target max / reference luminance of the
surface's preferred description (a compositor hint, not a measurement),
re-read whenever the preference changes. The description is ext_linear,
BT.2020, luminances (0, round(headroom x 203), 203), so a value v is sent as
v x 203 / max, clipped to [0, headroom]. Layers are synchronized subsurfaces
of GDK's surface, placed directly above it and below GTK's own subsurfaces
such as popovers. They are presented at the end of the frame clock's `paint`,
so they apply with GDK's commit in `after-paint`.

A presenting toplevel cannot be shown again after hiding. GDK destroys its
`wl_surface` when a window is hidden, and the backend presents nothing once
its toplevel unmaps. darktable has no such path: the main window is hidden
only at quit, and the second window is destroyed after hiding. With GTK4,
where the image is a texture on GTK's own surface, the limit goes away.

## Workarounds

Each lives in one file and goes away with the limitation it works around.

| Workaround | Where | Works around | Removed when |
|---|---|---|---|
| image and foreground layers above GTK's surface | `presenter_layers.c` | GTK3 cannot give a widget a surface with a color state | GTK4: the image is a texture with a color state |
| `gtk_widget_draw()` replay of GtkOverlay children stacked above a presenting widget into the foreground | `presenter_layers.c`, `_replay_overlays()` | the layers cover GTK's drawing of those widgets (toasts, masks panel, second window buttons); where GTK3 draws popovers into the toplevel (Quartz, Win32), popovers need the same | same |
| composing and presenting in the frame clock's `paint` signal, after GDK's handler | `presenter_wayland.c`, `_paint()` | GTK3 runs all draws in `paint` and GDK commits its surface in `after-paint`; the layers must be ready in between | same |
| GTK's surface taken as sRGB, the window's device encoding | `presenter_wayland.c`, `_set_output()`, and `presenter_layers.c`, `_window_get()` | GTK3 does not tag its surface; an untagged surface is sRGB to the compositor, with a transfer function of its choosing | GTK tags its surfaces |
| no opaque region on the layers | `presenter_wayland.c`, `_layer_init()` | a compositor may stop frame callbacks for a surface it considers occluded, which stops GTK's frame clock | GTK4 |
| surface feedback destroyed when the toplevel unmaps | `presenter_wayland.c`, `_toplevel_unmap()` | GDK destroys its `wl_surface` when a window is hidden, and asking the feedback on it for the preferred description is then a protocol error | GTK4 |
| a draw GTK clipped to a damaged area does not replace the images `dt_presenter_keep()` shows | `presenter.c`, `dt_presenter_begin()` | GTK3 redraws only the damaged part of a widget, so such a draw records clipped images; it stays in `presenter.c` because the cairo backend needs it too | GTK4: a widget's snapshot is always whole |

## Scope

Through the presenter: the darkroom (main image, preview fallback, snapshots
split and side by side, the duplicate manager's preview over the canvas, the
second window and its pinned image) and the slideshow.

Everything else GTK draws stays on GTK's 8-bit surface: lighttable,
filmstrip, culling and full preview, navigation, the duplicate manager's
thumbnails, all thumbnails. With a native backend they are shown as the
compositor shows untagged content, without wide gamut or HDR. GTK3 cannot
tag its surface, and the protocol leaves the transfer function of untagged
surfaces to the compositor: where it decodes them with gamma 2.2, as KWin
does, their shadows are darker than the image's. In GTK4 each thumbnail can
be a texture with a color state, and GTK tags its surfaces. They stay on
GTK's surface because thumbnails carry GTK widgets on top (stars, labels,
badges, buttons), which on the layers would mean replaying most of the
lighttable on every scroll, and because the mipmap cache is 8-bit sRGB or
AdobeRGB, so wide gamut and HDR need a float thumbnail cache anyway. See
darktable-org/darktable#20477.

With `cache_color_managed` off the mipmap cache holds display-encoded
thumbnails, encoded for the main window's system display profile, and a compositor that
manages color shows them as sRGB like the rest of GTK's surface. They
follow the window's output once thumbnails go through the presenter.

A macOS or Windows backend also has to replay popovers over the canvas into
the foreground: there GTK3 draws them into the toplevel's surface, while on
Wayland they are subsurfaces above the layers.

## Common Mistakes

| Mistake | Instead |
|---|---|
| painting a backbuf with cairo in a presenting widget | `dt_presenter_image()` with the buffer's `backbuf_output` and `backbuf_id` |
| reading `darktable.color_profiles` for a presented pipe's display profile, or `pipe->output` for what its pixels are | `pipe->output_encoding` or the buffer's label; `pipe->output` is the request |
| converting a module GUI's colors with the pipe's output profile | `dt_ioppr_get_gui_profile_info()` |
| converting a backbuf for GTK-side drawing on every draw | `dt_presenter_gtk_surface()` with the buffer's `backbuf_id` |
| checking for a platform in the pipeline, a module or a view | put it in `presenter_layers.c` or the platform's transport, with a row in [Workarounds](#workarounds) |
