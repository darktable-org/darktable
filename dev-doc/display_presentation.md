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
- [A screen pipe publishes frames](#frames): immutable, refcounted copies
  of its output, in the format its window asks for. The pipe does not
  quantize; the publish step packs RGB24 for cairo windows, with
  `dt_display_to_8bit()`. Any other format or encoding of a frame is a
  rendition of it.
- [A presenting widget submits its frames with
  `dt_presenter_image()`](#presenter-api) and draws everything else with
  cairo as before. GTK-side drawing takes the frame's rendition in the
  window's device encoding.
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
  on X11 and on Wayland without the native backend, the compositor through
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
  pipe produced to its window's, frames drawn by GTK take their device
  rendition ([frames](#frames)), and module GUIs take
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
- `dt_dev_image()` takes its sink's output description, which its pipe
  renders to, and returns the frame the pipe published. An image encoding
  of type `DT_COLORSPACE_NONE` is no display encoding: with colorout
  filtered out, as overlay does, the frame stays in the pipe's working RGB,
  float for the high quality overlay and RGB24 for the legacy one.
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
  is each frame's encoding. `output_profile_info`
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

## Frames

`src/develop/frame.h`. gamma is the terminal module of every pipe and
writes float RGBA: the pixel as colorout encoded it, with alpha 1 (channel 3
carries mask data inside the pipe), or its mask and channel views as display
values. A screen pipe publishes that as a frame, `pipe->frame`, written
under `backbuf_mutex`; `dt_dev_pixelpipe_frame()` takes a reference. A frame
is a copy, as master's backbuf was, since the cache line it comes from is not
pinned, and it never changes once it is made. It holds:

- the pixels, in the format of the pipe's output (`format` in
  `dt_display_output_t`): RGB24, packed on the worker with
  `dt_display_to_8bit()` into cairo's BGRX with X 255, or float RGBA;
- its encoding, the pipe's `output_encoding`: what colorout produced, not
  what the pipe was asked for (`dt_frame_encoding()`);
- the serial of the output it was made for (`dt_frame_made_for()`), and an
  id, a 32-bit count of the frames made, so unique for the session in
  practice;
- its geometry (`dt_frame_geometry()`): size, scale and zoom position, as
  master's `backbuf_scale` and `backbuf_zoom_pos` were. The pipe keeps
  `backbuf_width` and `backbuf_height`, which many read.

Exports, thumbnails, AI restore and pixel masks do not run screen pipes:
their pipes keep `pipe->backbuf`, the last module's output.

A window's output names the format its frames are made in: RGB24 where cairo
paints them, float where a native backend composes them. A new format is a
new image (`dt_display_output_diff()`), so the window's producers render
again. The format belongs to the frame, not to the pipe cache: gamma and
every cache line stay float, no hash includes it, and only the publish step
packs.

Any other format or encoding of a frame is a rendition: `dt_frame_surface()`
(RGB24) and `dt_frame_float()`. A rendition is made from the frame's own
pixels when it is first asked for, never from another rendition, and is kept
with the frame; any thread may ask. In the frame's own format and profile it
is the frame's pixels, whatever the headroom. Conversions are built from the
profiles the encodings name, which live for the session, so a frame outlives
the develop that made it, such as `dt_dev_image()`'s temporary one; matrix
profiles go through darktable's matrix and curves, the matrix on unclipped
values and an RGB24 rendition clipped after it, others through lcms.

GTK-side drawing (navigation, the filmstrip thumbnail of the edited image,
the correction cursor, focus peaking) takes a reference on the frame and
draws `dt_frame_device_surface()`, its RGB24 rendition in the window's device
encoding. On a cairo window that is the frame's own pixels: the GUI thread
does what master did with its 8-bit backbuf. Elsewhere the worker makes,
when it publishes a frame, the device renditions GTK usually asks for: the
preview's, for navigation and the filmstrip, whether they are shown or not,
and, while focus peaking is on, those of the full and second window pipes.
A draw still converts where the worker did not: snapshot and duplicate
frames under focus peaking, frames published before focus peaking was
turned on, and frames whose window's device encoding changed while their
pipe ran.

## Presenter API

`src/gui/presenter.h`. A presenting widget wraps its draw handler:

```c
cairo_t *pcr = dt_presenter_begin(widget, cr);
// draw everything on pcr, in paint order
dt_presenter_end(widget, pcr);
```

Inside, `dt_presenter_image(pcr, x, y, frame, filter)` takes the place of
`cairo_set_source_surface()` + `cairo_paint()` for an opaque image: it uses
the context's current transformation and the extents of its clip, and paint
order is kept, so whatever is drawn afterwards is on top. The presenter
holds a reference on the frame for as long as it shows or keeps it, so the
caller may drop its own when the call returns. The window shows the frame
in its output's image encoding: cairo paints the frame's RGB24 rendition, a
native backend composes its float one. A frame made for that output is its
own pixels; one in another profile or format, made before the output
changed, is shown converted, never refused, as GTK4's renderers and Chromium
convert at draw. It stays until its producer renders it again: an output
change moves the window's serial and redraws it, and the producers compare
the serial there ([output description](#output-description)). A frame
rendered for another headroom keeps its tone mapping until then.

- `dt_presenter_retain()` keeps the draw in progress: everything it shows,
  its images and all that is drawn around and above them, is the widget's
  kept presentation from then on. `dt_presenter_keep()` paints the kept
  presentation where it was, over the widget's area as it was then,
  replacing what the draw has drawn so far there, and the rest of the draw
  goes on top; it returns FALSE when there is none.
  The darkroom retains its full image draws while the loading screen
  preference (`darkroom/ui/loading_screen`) is off, as master cached its
  drawing then, and shows them while the next image loads, over its
  background, which shows where the view has grown since: the color
  assessment surround, focus peaking and guides of the image it switches
  away from stay, under the loading toast. A draw GTK restricted to a
  damaged area neither replaces nor drops the kept presentation; a draw is
  full when its clip is one rectangle over the whole widget, and a full
  draw that neither retains itself nor shows the kept one drops it. On the
  layered backend kept images are shown in the window's output as it is
  now, converted as any other; the cairo backend keeps the pixels it
  painted.
- `dt_presenter_clear()` drops a widget's images and kept presentation
  (darkroom `leave()`); unmapping a widget does too.
- `dt_view_paint_surface()` is the darkroom's user: background, then the
  preview fallback and the main image through the presenter. Pickers, masks,
  module overlays, guides and focus peaking follow on the same context.

Presenting widgets: the center view and the side-by-side snapshot area
(`gui/gtk.c`, `_draw()`), and the second darkroom window. Images submitted
there: the darkroom main image and preview fallback, snapshots, the
duplicate manager's preview, and the slideshow.

## Backends

A window has one backend for as long as it exists, chosen when its first
presenting widget draws (`gui/presenter.c`, `_window_get()`): layered if the
platform has a transport and it can set up, otherwise cairo. The core,
`gui/presenter.c`, keeps the widget records, the references on the frames
of each widget's last draw, the test for a draw GTK clipped to a damaged
area, and the matrix of each image; it has no branch on the backend. A
backend (`dt_presenter_backend_t` in `gui/presenter_backend.h`) begins a
widget's draw and returns the context to draw on, shows an image, paints
the kept presentation, ends the draw and clears the widget. Each keeps
the presentation its own way, with the same meaning: what the widget's
retained draw showed.

### cairo

`gui/presenter_cairo.c`. The widget draws on GTK's own context, and each
frame's RGB24 pixels are painted there: frames for a cairo window are made
in RGB24, so a frame in the window's encoding is neither converted nor
copied on the GUI thread; one made for an earlier output is converted
there once. The window's output is its system display profile (display2 for
the second window) as image and device encoding, the same output a window
has before it is realized and after it is gone.
Every window without a native backend uses it: X11, macOS, Windows, and
Wayland where the compositor cannot take the native backend or its output
is plain SDR sRGB ([Wayland](#wayland)).

The kept presentation is what GTK's target holds at the end of the
retained draw. On X11, GDK draws each frame into a surface of its own, and
a reference on it keeps it, as master kept the darkroom's: it holds what
GTK drew above the widget in that frame as well, a toast among it, and the
widgets around it, so keeping paints only the widget's area of then. On
Wayland and Quartz the target is the window's buffer, which the next frame
overwrites, and on Windows holding it stops GTK from invalidating the
widget (#20831), so there the widget's part is copied into a surface of
the widget's size, 4 bytes per device pixel, at the end of every retained
draw.

### Layered

The layered backend, the native one, shows a window's images on surfaces of
its own above GTK's surface, from bottom to top:

1. GTK's surface: everything GTK draws, untagged.
2. The image layer: premultiplied half float, in the window's output,
   alpha 0 off the images.
3. The foreground layer: premultiplied 8-bit, untagged like GTK's surface,
   with everything the presenting widget draws. Where an image is, its
   pixels are cut out by the image's coverage, so paint order holds across
   the two layers.

```
  foreground layer   8-bit, untagged    pickers, masks, guides, overlays
  image layer        float, managed     darkroom image, snapshots
  GTK's surface      8-bit, untagged    panels, thumbnails, everything else
```

Both layers take no input, so GTK below keeps getting all of it. A presenting
widget that draws no image puts its drawing on GTK's surface as cairo would,
and its layers are unmapped (lighttable, map, a darkroom loading its first
image).

The kept presentation is the retained draw's foreground, with its images
cut out and all it drew above them, and that draw's images. Keeping it
paints that foreground over what the draw has drawn so far and places its
images again, below anything the draw places later. A presentation kept
at another device scale is dropped when it would be shown: its cuts and
coverages are of that scale, and the clips they came from are gone. While a widget's
draws are retained, they alternate between two foreground surfaces, so
keeping costs one more foreground-size surface per retaining widget, 4
bytes per device pixel, and no copy. What is replayed above the widget at
present (GtkOverlay children such as toasts) is not part of its
presentation: on a kept foreground the replay saves what it covers and
puts it back after the transport has taken the layers.

The two layers show what cairo would paint on GTK's surface, in paint
order. With the foreground `F` above the image layer `N`, keeping
`R = F + L` for the painted result `R` and the images' share `L`, the
layer shows `N = L / (1 - alpha_F)`, and `F over (N over G)` is
`R + (1 - alpha_R) G` over GTK's surface `G`:

- a paint that is not an image changes `F` only;
- an image paint of coverage `a`, over a foreground of alpha `b` before
  it, cuts the foreground, `F <- (1 - a) F`, and moves the layer towards
  the image, `N <- N + w (I - N)` with `w = a / (a + (1 - a)(1 - b))`, on
  all four channels with the image's alpha 1. `w` comes from the `a` and
  `b` the cut applied and is in [0, 1], so `N`'s alpha stays in [0, 1]
  and its color in the clip range of the encoding. Over an opaque backdrop
  `N` is opaque wherever an image covers anything; a translucent or
  missing backdrop takes the same rule, nothing assumes it away.

The coverage is cairo's: the alpha cairo paints with a source of the
frame's size and alpha 1 and the image's matrix and filter through the
image's clip, including the filter's reach past the frame's edge. It is
made when an image is placed, one byte per device pixel over the image's
box. First cairo paints alpha 1 through the image's own context into a
group of alpha content (`cairo_push_group_with_content()`), which takes
the caller's clip as it is, any shape, nested, with fractional edges
(the preview fallback and the main image share one clip with edges on
half pixels), and is read rather than painted back, which would apply
the clip twice: the clip's coverage. That is the coverage wherever the
frame covers whole; in the 64-pixel tiles near the frame's edge cairo
paints the frame unclipped and the clip's coverage multiplies it, as a
paint through a clip does. Images placed alike in this draw, the last one
and the kept one share a coverage: same frame size, matrix, filter and
scale, and the same clip coverage, compared as runs of equal bytes.
`test_presenter_compose` compares the coverage with cairo's paint of the
whole widget: equal where the frame covers whole; in the edge tiles pixman
steps sample positions in fixed point from where each composite starts,
so a tile differs from the whole paint by a few code values at fractional
pixels (at most 5 in 40 seeds of the test, which allows 8) or, with a
nearest filter, by a pixel whose sample lies on the frame's edge. Through
a clip that is not a rectangle, cairo itself antialiases the clip's edge
differently with the compositing route it takes, and the test allows 32
code values there (at most 14 measured). No caller clips images to
anything but rectangles. When the image is placed, the foreground's alpha
is kept at each pixel of fractional coverage and the foreground is cut. Images are sampled from each frame's
float pixels, nearest or bilinear, padded at the edges (cairo's
`EXTEND_NONE` without its alpha, which the coverage carries), in double
precision positions but not bit-identical to pixman's.

The image layer is composed only when what it shows changes: frames,
placements, the foreground's alpha under fractional coverage, size,
scale, and the encoding the transport gives. It is composed straight into
the transport's buffer, with the images accumulated in float per row; the
encoding's matrix applies before the one quantization, so overlapping
images round once. A texel that is not finite counts as its limit: an
infinite one where it weighs clips to the range, and where its bilinear
weight is 0 it does not count; NaN where it weighs gives the range's
bottom. Limits of
the two surfaces: at pixels of fractional coverage the compositor blends
the layers in its own blending space, not in cairo's encoded values, as
GTK4's offloaded subsurfaces and Chromium's underlays do; the 8-bit
foreground's rounding perturbs `N`'s invariance under later paints by up
to a code value.

What GTK3 draws above a presenting widget is in GTK's surface, under the
layers, so the backend replays it into the foreground when it presents:
the children that each GtkOverlay on the way to the toplevel stacks above
the widget, bottom to top, and the toplevel's popovers unless the
transport shows them above its layers (`popovers_above`), as on Wayland.
GtkWindow draws its popovers above its content, in the order
`gtk_container_forall()` lists them. A popover is no part of any
presenting widget, so the backend watches each one it replays: when one
is shown, hidden, moved or animated, or its content changes, the
window's presenting widgets are drawn again.

The transport reports the window's output, its image encoding and, as
device, the profile GTK's own surface is shown in, through
`dt_presenter_window_set_output()`, at setup and whenever it changes. The
pipes of that window then render to that output, and everything GTK draws
there (thumbnail transforms, picker swatches, frames GTK draws through
`dt_frame_device_surface()`, and module GUIs through
`dt_ioppr_get_gui_profile_info()`) converts to the device profile, sRGB,
where a cairo window's device profile is its system display profile. A
new headroom changes the image encoding only: that window's images are
rendered again, thumbnails are not. When the window goes away, its output
returns to its system display profile.

Code is split by what it works around:

- `gui/presenter_layers.c`: the layered backend, the GTK3 part every
  native platform shares. It composes the image layer and the foreground,
  keeps the presentation, and computes the layers' size, scale and
  position in the toplevel. A GTK4 port, where widgets draw textures with
  color states, deletes it.
- `gui/presenter_<platform>.c`: transport and timing. Setup and fallback,
  output description and headroom, buffers, and `reserve()`, `commit()`,
  `clear()`, `release()` and `free()` (`dt_presenter_transport_t` in
  `gui/presenter_layers.h`). A present is one transaction: `reserve()`
  gives the storage of both layers (`dt_presenter_reservation_t`), each
  with its row stride, and the image's encoding
  (`dt_presenter_encoding_t`): a clip range for straight color in the
  window's output, then a 3x3 linear map, stored as signed half floats,
  which covers a transport that encodes in other primaries or with
  another white level, and the layered backend writes them and commits.
  The transport may refuse a reservation while the compositor holds its
  buffers; a reservation without an image buffer is refused too when the
  image changed. Nothing of the present is then shown, and the widget
  draws again next frame. The transport decides when to call
  `dt_presenter_window_present()`: where the layers become visible
  together with GTK's surface for that frame, or later, never earlier.

The costs over a cairo window, per presenting widget: two foreground
surfaces of 4 bytes per device pixel (one while nothing is retained), the
transport's buffers (on Wayland two of each layer, 8 and 4 bytes per
device pixel), a coverage of one byte per device pixel over each placed
image's box, and an alpha-1 source of one byte per pixel of the largest
frame shown. Per present the foreground is copied into the transport's
buffer and the replay drawn into it; the image layer is composed only
when it changes, at about one sample per device pixel of each image's
box.

### Wayland

`gui/presenter_wayland.c`, built with `USE_WAYLAND`. It needs `wl_compositor`
4 and `wp_color_manager_v1` with parametric descriptions, `ext_linear`, BT.2020
primaries and `ABGR16161616F` shm buffers; `set_luminances` is optional
(without it the output is SDR). Without the rest the window is on cairo.

The native backend also gives up when the surface's preferred description
is plain SDR sRGB: sRGB primaries and D65 white within 1e-3, transfer
function srgb, gamma22 or compound_power_2_4, no ICC file, and a target
max luminance at most the reference, or no luminances
(`_passes_untagged_through()`). A compositor that describes its output
that way passes untagged pixels through, so the window is a plain cairo
window on its system display profile, as on X11. On GNOME's default color
mode that profile, from colord, is the only description of the panel
([Workarounds](#workarounds)); elsewhere there is none, and sRGB gives what
the native backend would. Limits:

- The choice is made once per window, with its backend, for the monitor it
  is on then. A new color mode, or moving the window to a monitor described
  otherwise, takes effect when the window is opened again, for the main
  window after a restart.
- sway with a `color_profile` set only in sway describes its output as sRGB
  while converting everything to that profile. A tool that also publishes
  the profile to colord then makes darktable convert to it a second time.

The output is linear BT.2020 with headroom target max / reference luminance of the
surface's preferred description (a compositor hint, not a measurement),
re-read whenever the preference changes. The description is ext_linear,
BT.2020, luminances (0, round(headroom x 203), 203), so a value v is sent as
v x 203 / max, clipped to [0, headroom]: the reservation's clip range
[0, headroom] and matrix diag(1 / headroom). A new description is set on the
image layer when it is ready, and applies with that layer's next commit,
which is the first buffer encoded for it: a new headroom composes the image
again, whether or not its frames change. Layers are synchronized subsurfaces
of GDK's surface, placed directly above it and below GTK's own subsurfaces
such as popovers, which therefore need no replay. They are presented at the
end of the frame clock's `paint`, so they apply with GDK's commit in
`after-paint`. With `DT_PRESENTER_REPLAY_POPOVERS` set in the environment,
the transport places the layers above the popovers shown, as Quartz and
Win32 draw popovers into the toplevel, and declares so: the layered
backend's popover replay can then be tested on Wayland.

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
| `gtk_widget_draw()` replay into the foreground of what GTK stacks above a presenting widget: GtkOverlay children, and the toplevel's popovers where the transport's layers cover them; saved and put back on a kept foreground; a popover's changes draw the presenting widgets again | `presenter_layers.c`, `_replay_above()` | the layers cover GTK's drawing of those widgets (toasts, masks panel, second window buttons), and on Quartz and Win32 of popovers, which GTK3 draws into the toplevel there | same |
| composing and presenting in the frame clock's `paint` signal, after GDK's handler | `presenter_wayland.c`, `_paint()` | GTK3 runs all draws in `paint` and GDK commits its surface in `after-paint`; the layers must be ready in between | same |
| GTK's surface taken as sRGB, the window's device encoding | `presenter_wayland.c`, `_set_output()` | GTK3 does not tag its surface; an untagged surface is sRGB to the compositor, with a transfer function of its choosing | GTK tags its surfaces |
| a plain SDR sRGB output left to cairo with the system display profile | `presenter_wayland.c`, `_passes_untagged_through()` | mutter's default color mode composites as sRGB and does not apply the profile assigned to the screen (mutter#4597) | the compositor describes its real output (mutter MR !5177); nothing to do, the predicate stops matching |
| no opaque region on the layers | `presenter_wayland.c`, `_layer_init()` | a compositor may stop frame callbacks for a surface it considers occluded, which stops GTK's frame clock | GTK4 |
| surface feedback destroyed when the toplevel unmaps | `presenter_wayland.c`, `_toplevel_unmap()` | GDK destroys its `wl_surface` when a window is hidden, and asking the feedback on it for the preferred description is then a protocol error | GTK4 |
| a draw GTK clipped to a damaged area does not replace or drop the presentation `dt_presenter_keep()` shows | `presenter.c`, `dt_presenter_begin()` | GTK3 redraws only the damaged part of a widget, so such a draw records clipped images; it stays in `presenter.c` because both backends need it | GTK4: a widget's snapshot is always whole |
| a cairo window's kept presentation is a reference on GTK's target on X11 and a copy of the widget's part elsewhere | `presenter_cairo.c`, `dt_presenter_cairo_new()` | GTK3 keeps no rendering of a widget: what it drew is only in GDK's target, the window's buffer on Wayland and Quartz, which the next frame overwrites, and a surface that cannot be held on Win32 (#20831) | GTK4: a widget's render nodes are kept between frames |

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

## Common Mistakes

| Mistake | Instead |
|---|---|
| painting a frame with cairo in a presenting widget | `dt_presenter_image()` with the frame |
| reading `pipe->backbuf` of a screen pipe | `dt_dev_pixelpipe_frame()`; only non-screen pipes keep a backbuf |
| keeping a frame's pixels or renditions after dropping its reference | hold a reference (`dt_frame_ref()`) for as long as you may draw it |
| reading `darktable.color_profiles` for a presented pipe's display profile, or `pipe->output` for what its pixels are | `pipe->output_encoding` or the frame's encoding; `pipe->output` is the request |
| converting a module GUI's colors with the pipe's output profile | `dt_ioppr_get_gui_profile_info()` |
| converting a frame for GTK-side drawing yourself | `dt_frame_device_surface()`, made once and kept with the frame |
| converting with a develop's profile infos outside that develop | build them from the encoding's profile, as `frame.c` does: a temporary develop frees its list |
| checking for a platform in the pipeline, a module or a view | put it in `presenter_layers.c` or the platform's transport, with a row in [Workarounds](#workarounds) |
