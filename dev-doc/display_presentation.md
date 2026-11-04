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
- [Module GUIs convert colors with the GUI
  profile](#native-backends-and-their-layers),
  `dt_ioppr_get_gui_profile_info()`. The pipe's output profile describes the
  image layer, which on a native backend is not what GTK draws in.
- [Platform and GTK3 code stays in the presenter files](#workarounds). The
  pipeline, modules and views only see an output description.

## Output Description

`dt_display_output_t` (`src/common/colorspaces.h`) is the encoding a
presented pipe renders to: a profile (type, file, intent), a headroom, and
whether it is `managed`. Values are display-referred RGB in that profile,
1.0 at reference white and up to `headroom` above it. A managed output is
shown by a presenter backend that interprets the values in the profile; an
unmanaged one, the selected display profile, sends them to the screen as
device values. The mask and channel views are authored as sRGB device
values and converted for a managed output (`dt_display_output_diagnostic()`);
the gamut check and clipping markers are written as they are, saturated
colors that mark in any output.

- The full, preview and second-window pipes take their output from their
  window at the start of every run, `dt_colorspaces_get_display_output()` in
  `dt_dev_process_image_job()`, so workers never read GUI state while
  processing. A changed output resynchronizes the pipe.
- `dt_dev_image()` sets the output of its own pipe. `NULL` means no display
  encoding: with colorout filtered out, as overlay does, the buffer stays in
  the pipe's working RGB.
- colorout renders to `pipe->output`; the scopes and the global color picker
  read it to interpret the preview pipe's pixels. The pipe's
  `output_headroom`, which sigmoid and agx tone map to, honors
  `output.headroom`. A soft proof caps it at the proof profile's headroom.
- Export, thumbnail and AI pipes have no output description (`type` is
  `DT_COLORSPACE_NONE`).

## Float Backbuf

gamma is the terminal module of every pipe and writes float RGBA: the pixel
as colorout encoded it, with alpha 1 (channel 3 carries mask data inside the
pipe), or its mask and channel views as display values. A screen pipe copies
that into `pipe->backbuf`, records the output it was rendered in as
`backbuf_output`, and gives the frame a new `backbuf_id`
(`dt_dev_new_frame_id()`).

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
shows nothing, when the window declared an output (a native backend, or cairo
on a compositor that manages color) and the frame is in another profile. The
caller does nothing about it: the declaration announces a change of display
profile, colorout's handler reprocesses the darkroom pipes
(`dt_dev_reprocess_all()`), and the center view's next draw requests the
second window's pipe too; snapshots are redone on
`DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED`. Without a declared output the cairo
backend shows any display-encoded frame.

- `dt_presenter_keep()` shows the images of the last full draw again where
  they were (the darkroom while it loads the next image without the loading
  screen). A draw GTK restricted to a damaged area does not replace them.
  Kept images in a profile other than the declared output's are dropped.
- `dt_presenter_clear()` drops a widget's images (darkroom `leave()`);
  unmapping a widget drops them too.
- `dt_view_paint_surface()` is the darkroom's user: background, then the
  preview fallback and the main image through the presenter. Pickers, masks,
  module overlays, guides and focus peaking follow on the same context.

The headroom is not compared. After it changes, the duplicate manager's
preview, the slideshow and a pinned image in the second window keep the tone
mapping of the old headroom until they are rendered again for another
reason.

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
context. The window's output is the selected display profile (display2 for
the second window), unmanaged. Every platform without a native backend uses
it: X11, macOS, Windows, and Wayland without color management.

A Wayland compositor that manages color but lacks what the native backend
needs also gets cairo. It takes GTK's untagged surface as sRGB, with a
transfer function of its choosing, and converts it to the screen, so the
Wayland setup declares the window's output and GTK's surface sRGB
(`dt_presenter_window_set_output()`), and the display profile controls are
insensitive as with a native backend. The declaration is given back when the
window goes away.

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

The backend declares the window's output and the profile GTK's own surface
is shown in (`dt_colorspaces_set_backend_output()`). The pipes of that window
then render to that output, and everything GTK draws there (thumbnail
transforms, picker swatches, the navigation thumbnail,
`dt_presenter_gtk_surface()`, and module GUIs through
`dt_ioppr_get_gui_profile_info()`) converts to that profile instead of the
selected display profile. The change is announced like a user's change of
display profile, so converted thumbnails are redone. The saved selection is
never touched; the display profile controls are insensitive while the
presenter owns the output (`dt_presenter_display_control()`).

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
| GTK's surface declared sRGB | `presenter_wayland.c`, `_set_output()` and `dt_presenter_wayland_new()` | GTK3 does not tag its surface; an untagged surface is sRGB to the compositor, with a transfer function of its choosing | GTK tags its surfaces |
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
thumbnails, encoded for the selected display profile, and a compositor that
manages color shows them as sRGB like the rest of GTK's surface. They
follow the window's output once thumbnails go through the presenter.

A macOS or Windows backend also has to replay popovers over the canvas into
the foreground: there GTK3 draws them into the toplevel's surface, while on
Wayland they are subsurfaces above the layers.

## Common Mistakes

| Mistake | Instead |
|---|---|
| painting a backbuf with cairo in a presenting widget | `dt_presenter_image()` with the buffer's `backbuf_output` and `backbuf_id` |
| reading `darktable.color_profiles` for a presented pipe's display profile | read `pipe->output`, or the pipe's output profile info |
| converting a module GUI's colors with the pipe's output profile | `dt_ioppr_get_gui_profile_info()` |
| converting a backbuf for GTK-side drawing on every draw | `dt_presenter_gtk_surface()` with the buffer's `backbuf_id` |
| checking for a platform in the pipeline, a module or a view | put it in `presenter_layers.c` or the platform's transport, with a row in [Workarounds](#workarounds) |
