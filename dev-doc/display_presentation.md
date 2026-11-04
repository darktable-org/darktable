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

## Output Description

`dt_display_output_t` (`src/common/colorspaces.h`) is the encoding a
presented pipe renders to: a profile (type, file, intent) and a headroom.
Values are display-referred RGB in that profile, 1.0 at reference white and
up to `headroom` above it.

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
thumbnail of the edited image), by the overlay module, and by the 8-bit
export path.

## Presenter API

`src/gui/presenter.h`. A presenting widget wraps its draw handler:

```c
cairo_t *pcr = dt_presenter_begin(widget, cr);
// draw everything on pcr, in paint order
dt_presenter_end(widget, pcr);
```

Inside, `dt_presenter_image(pcr, x, y, pixels, width, height, encoding, id,
filter)` takes the place of `cairo_set_source_surface()` + `cairo_paint()`
for an opaque image: it uses the context's current transformation and clip,
and paint order is kept, so whatever is drawn afterwards is on top. The
pixels are converted once per `id` into a frame the presenter owns, so the
caller may release the buffer (and its lock) when the call returns. It
returns `FALSE` when the window's backend cannot show a frame in that
encoding; the caller renders it again. The cairo backend shows any
display-encoded frame.

- `dt_presenter_keep()` shows the images of the last full draw again where
  they were (the darkroom while it loads the next image without the loading
  screen). A draw GTK restricted to a damaged area does not replace them.
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

The cairo backend paints each frame, quantized to an RGB24 surface, into the
widget's own context. The window's output is the selected display profile
(display2 for the second window), and every platform uses it.

## Scope

Through the presenter: the darkroom (main image, preview fallback, snapshots
split and side by side, the duplicate manager's preview over the canvas, the
second window and its pinned image) and the slideshow.

Everything else GTK draws stays on GTK's 8-bit surface: lighttable,
filmstrip, culling and full preview, navigation, the duplicate manager's
thumbnails, all thumbnails. They show what the mipmap cache holds (8-bit
sRGB, AdobeRGB or display RGB). Taking them through the presenter needs a
float thumbnail cache. See darktable-org/darktable#20477.

## Common Mistakes

| Mistake | Instead |
|---|---|
| painting a backbuf with cairo in a presenting widget | `dt_presenter_image()` with the buffer's `backbuf_output` and `backbuf_id` |
| reading `darktable.color_profiles` for a presented pipe's display profile | read `pipe->output`, or the pipe's output profile info |
