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
  taken from its window when the run starts, or in proof mode to an RGB
  proof profile, whose frames are converted like any other. Workers read `pipe->output`;
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
`image` is the encoding its pipes render to, unless they soft proof to an
RGB profile: display-referred RGB in a
profile (type, file, intent), 1.0 at reference white and up to `headroom`
above it. `device`, the device encoding, is what GTK's 8-bit values mean
as the platform shows them, everything GTK draws in the window: untagged
pixels on X11, Windows and Wayland, pixels GTK tags sRGB on macOS.
`platform_owned` says a backend reported it,
so a new system display profile does not replace it. Where image and device are the same
encoding, as on every cairo window, the values reach the screen as device
values. The mask and channel views and the clipping indicators are UI
tones, authored as device values and converted from the device encoding
to the pipe's `output_encoding`, which a run resolves after every commit
(`dt_ioppr_device_to_output()`). The gamut check's mark, device
(0, 1, 1), is one too: colorout renders it once per commit as it renders
pixels, so a proof narrower than the device clips it, and an ink proof's
goes straight to the window's output.

- Both windows have an output at all times, from `dt_colorspaces_init()` on:
  develop and its pipes exist before the GUI, and headless runs never
  realize a window. It is stored in `darktable.color_profiles`, indexed by
  window and written under `xprofile_lock`, and its `serial` changes
  whenever anything else in it does.
- A cairo window ([Backends](#backends)) renders to the screen profile the
  platform publishes to applications: the X atom on X11, colord on X11
  and, when built with GDK X11 support, on Wayland, the monitor's default
  profile from WCS on Windows (none, as under Advanced Color, means sRGB),
  and sRGB where nothing is published, as on macOS, where GTK tags its
  backing store sRGB. Cairo never decides
  whether the platform converts. The one exception is the preference
  `ui_last/display_profile_file`: an ICC file that replaces what the
  platform publishes, for both windows, read in its place by
  `dt_colorspaces_set_display_profile()`. A file that is unreadable or not
  an RGB profile counts as none, with a message once per file. A native
  window ignores it, as it ignores colord's profile, and so does its
  registry entry (below).
  There is no other user selection; previewing
  another display, print or sRGB is a soft proof. Unless the platform owns
  it, a window's output is its system display profile (`DISPLAY` or
  `DISPLAY2`) as image and device encoding, relative colorimetric, with
  headroom 1 (`_describe_system_display()` in `colorspaces.c`). A new
  system display profile republishes it. Limits:
  - darktable on XWayland is an X11 client and cannot see the compositor's
    description. It renders to the X atom or colord as on X11: where the
    compositor converts untagged pixels to the profile also published
    there, as with xiccd, the image is converted twice, and mutter outside
    its default color mode (before mutter !5177) reads them as sRGB.
  - A profile reassigned in the system settings while the window stays on
    its monitor is picked up, as in master, at the next configure event
    that queries again: a move of the main window, any configure on
    Wayland or of the second window. A persistent `CdWindow` per window,
    listening to its changed signal, and a listener on the X atom would
    remove that.
  - The display profile file is applied as is. On macOS, and on Windows
    with automatic color management or HDR, the system converts the image
    again, from sRGB, so its colors are converted twice.
- The registry's entries "system display profile" (`DISPLAY`) and "system
  display profile (second window)" (`DISPLAY2`) are what their window shows
  8-bit values as, its device encoding: the system display profile on a
  cairo window, the device the backend declared on a platform-owned one,
  sRGB on Wayland, whatever the platform or the file reports. What names
  the entry by type therefore renders for what the window shows:
  thumbnails with `cache_color_managed` off, the style preview. No menu
  offers the entries, not the histogram's, the soft proof's or export's:
  darktable cannot say what reaches the screen where the platform
  converts, so nothing it computes can claim to be the display's, as
  Krita names no monitor profile where the OS manages color. A stored
  histogram or soft proof profile no menu offers becomes sRGB
  (`_drop_unoffered_choices()`).
  The window's system display profile, the platform's report or the file,
  is kept per window (`system_display` in `dt_colorspaces_t`) and describes
  the window only when cairo shows it; in an output, `DISPLAY` and
  `DISPLAY2` name that profile. The entry follows every published change
  of the device profile (`_entry_follow_device()`), the platform taking
  the window over or giving it back included. Krita does the same: its UI
  profile is its surface's where the OS manages color, and the monitor
  profile only where it does not.
- What GTK draws converts to the device encoding: the thumbnail transforms
  follow the main window's, picker swatches convert from what the preview
  pipe produced to its window's, frames drawn by GTK take their device
  rendition ([frames](#frames)), and module GUIs take
  `dt_ioppr_get_gui_profile_info()`, the main window's device profile, or
  sRGB for a profile without matrices. On a cairo window device and image
  are the same encoding, so these conversions are the identity there,
  except for a frame tagged with an RGB proof profile, which converts like
  any other frame.
- The presenter takes a window's output over for the platform, and gives
  it back when the window goes away (`dt_colorspaces_set_window_output()`).
- A change is announced by what changed (`dt_display_output_diff()`), the
  same way for the presenter and a new system display profile
  (`_output_changed()` in `colorspaces.c`). Any change moves
  the serial and queues a redraw of the window, where its producers find
  it. A new device encoding or owner also rebuilds the thumbnail
  transforms and raises `DT_SIGNAL_CONTROL_PROFILE_USER_CHANGED` with
  `DISPLAY` or `DISPLAY2`, for what GTK draws there: thumbnails (main
  window only) and module GUI colors. A new image encoding alone, such as
  a new [headroom target](#headroom-target) published when its description
  is ready, reruns that window's producers and nothing else.
  A new device profile is also a new registry entry, announced with
  `DT_SIGNAL_CONTROL_PROFILE_CHANGED` for what names the entry by type.
  It is raised exactly when the entry changes: for a new
  system display profile on a cairo window, and when the platform takes a
  window or gives it back; a new system display profile on a native window
  changes neither the output nor the entry and announces nothing. A new
  intent or headroom never touches the registry.
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
- colorout renders to `pipe->output` (an RGB soft proof to its proof
  profile instead), with the profile the run took
  (`profile`), not one it looks up again by type. The pipe's `output_headroom`, which
  sigmoid and agx tone map to, honors `output.headroom`: on a native
  window the [headroom target](#headroom-target) as its transport encodes
  it, so with follow output on
  what the darkroom shows depends on the screen's current headroom, once
  it settled, and not on the export's PQ peak. A soft proof sets
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
  profile under the same type and file name, and a new device profile the
  registry entry's. A replaced profile stays valid for the session, in
  `system_display_profiles` in `dt_colorspaces_t` or as another registry
  entry's (sRGB), so pixels made for the old screen keep converting
  with the profile they were made with: conversions take their profile
  info with `dt_ioppr_add_profile_info_for_content()`. Encodings and
  profile infos are the same content when profile, type and file name
  match; the type counts for PQ's and HLG's transfer functions. A query
  that yields no usable profile (no X atom and none from colord, an empty
  Windows query, bytes lcms rejects) makes the window's system display
  profile sRGB again, the profile it starts with, rather than leaving the
  previous screen's. A
  new system display profile that changes a window's output changes its
  device encoding, so thumbnails drawn before it arrived, as colord's
  asynchronous answer at startup, are redone.
- A soft proof is an export to the proof profile. In proof mode colorout
  renders the full and the preview pipe (not the second window, the
  thumbnail pipe or exports) as an export to that profile, through the same
  matrix and curves or littlecms, PQ and HLG through `dt_hdr_encode()`,
  with the proof's own intent and black point compensation
  (`softproof_intent`, `softproof_bpc`, set next to the proof profile in
  the darkroom). The matrix path serves a proof only where it gives what
  littlecms gives for that intent and compensation (`_matrix_like_lcms()`).
  A proof equals a darktable export with the same settings wherever that
  export honors them. An export asks for no compensation, which littlecms
  still applies for perceptual and saturation into a v4 profile, and its
  matrix path ignores the intent unless "always use LittleCMS 2" is on: a
  proof the matrix path cannot serve (absolute colorimetric, compensation
  with a nonzero black) matches the export made with that preference on.
  The GPU's 3D LUT covers Lab L 0..100, so PQ and HLG outputs off the
  matrix path run on the CPU. The proof clips where an integer export clips
  (a float export does not), and the gamut check marks what that clip
  changes: from the linear values on the matrix path and for PQ and HLG,
  by littlecms' gamut check otherwise. The output encoding is the proof
  profile, so the frame is tagged with it, and the presenter shows it as
  any other frame and clips what the screen cannot show. The preview's
  proof has no gamut marks: the scopes, the color pickers, navigation and
  the filmstrip's thumbnail of the edited image read it. In proof mode
  the scopes and pickers show values in what the preview recorded for its
  output, whatever the histogram setting
  (`dt_ioppr_get_scope_profile_info()`): the proof profile, or sRGB where
  colorout replaced it. An ink proof, as
  for a CMYK printer, goes on from the proof's values to the output
  relative colorimetric, since a frame holds RGB, and its output encoding
  is the window's, which the scopes then show, with no second proof.
  darktable does not export ink: an export to a CMYK
  profile writes sRGB pixels but embeds the CMYK profile. Limit: while
  the second window pans or zooms, its pipe can show the main preview's
  frame, proofed while the second window is not. Comparing the frames'
  encodings would not tell: a second window on another screen has another
  output for the same content, and the gamut marks change no encoding. The hidden Lab
  and XYZ outputs store other units than the proof and are left out.
- Export, thumbnail and AI pipes have no output description (`type` is
  `DT_COLORSPACE_NONE`).

### Headroom target

A native window's image headroom follows a target the presenter core
settles, not what the platform reports. The window's platform reports whenever it
says something about the screen (`dt_presenter_window_report_headroom()`,
`dt_presenter_headroom_t` in `gui/presenter_target.h`): the peak over
reference white now, or NAN when the platform has no value (the display
sleeps), the most the screen can show in the window if the platform says,
and a generation, which changes when earlier reports say nothing about
this one: another screen or mode, a display woken up. The macOS backend
reports no value before EDR is engaged and its potential EDR headroom as
the ceiling ([macOS](#macos)). A report never reaches pipes, and settling
is never a platform's or a backend's job. The policy is one pure function on a clock passed in
(`gui/presenter_target.c`, tested by `test_presenter_target`):

- The level is log2 of the value, clipped to the ceiling, rounded down to
  1/8 EV and at least 0 EV, so the target never declares more than the
  platform offers; an epsilon of 1e-4 step keeps an exact step from
  losing it to float error. A 1-nit sweep from 203 to 1600 nits gives 24
  targets.
- Hysteresis: a level above the target counts once the value clears it
  by half a step; any level below it counts, since the screen then shows
  less than the pipes render for. A value on a step boundary flips once.
- Settling: a new level is taken 1 s after the reported value stopped
  changing, longer than the gaps between brightness steps a person makes,
  and at the latest 3 s after its first report. A value back in the
  target's band before then costs nothing.
- A report without a value keeps the target and drops a level waiting to
  settle.
- A new generation (and a window's first report) forgets the target's
  band: its first value is taken at once, clipped to the ceiling, and
  until one comes the target is 1.0. The ceiling never stands in for a
  value: it is what the screen could show, not what it shows now. On a
  macOS cold start the first value is EDR's idle one, a change only above
  1.0; the ramp of up to 2 s after it changes the target once, where it
  ends.

A settled change goes to the backend's `target()`, which prepares showing
frames rendered for it and then publishes the window's output with the
headroom it can show, through `dt_presenter_window_set_output()`: a new
image encoding alone ([output description](#output-description)). The
Wayland transport describes its image layer for the target and publishes
when that description is ready ([Wayland](#wayland)); until then, and if
it fails, pipes and transport stay on the description in use. Until a
window's producers deliver, its frames keep the tone mapping they were
rendered with: the transport clips them to the range of the image
description in use, and the compositor clips or fits that range to the
screen (KWin clamps under relative intent).
A cairo window has headroom 1 and publishes no target; the target goes
on settling from the platform's reports, so a window that becomes native
again ([Backends](#backends)) starts at the screen's.

Why settle: every published image-headroom change reruns the window's
pipes from the start, since `output_headroom` is in every cache line's hash
(`pixelpipe_cache.c:126`), while KWin sends a preferred description per
frame of its brightness animation and macOS about 230 notifications per EDR
ramp. No surveyed editor follows the screen at all (Lightroom, Resolve and
Krita fix their target); viewers re-fit every frame on the GPU, and even
Chromium throttles headroom-only changes on macOS to 12 Hz.

The constants are hypotheses. `-d control -d verbose` logs every report
with its time and every settled target: the gaps between the reports of
one gesture bound the quiet time, the longest platform ramp the maximum
latency, and the noise of a screen left alone the hysteresis.

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
the develop that made it, such as `dt_dev_image()`'s temporary one, with
the intent of the encoding converted to (`dt_presenter_image()`); matrix
profiles go through darktable's matrix and curves, the matrix on unclipped
values and an RGB24 rendition clipped after it, others through lcms.

GTK-side drawing (navigation, the filmstrip thumbnail of the edited image,
the correction cursor, focus peaking) takes a reference on the frame and
draws `dt_frame_device_surface()`, its RGB24 rendition in the window's device
encoding. On a cairo window that is the frame's own pixels, unless the frame
is in another profile, as an RGB proof's is: the GUI thread does what master
did with its 8-bit backbuf. Elsewhere the worker makes,
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
the context's current transformation and clip, of any shape, and paint
order is kept, so whatever is drawn afterwards is on top. The presenter
holds a reference on the frame for as long as it shows or keeps it, so the
caller may drop its own when the call returns. The window shows the frame
in its output's image encoding: cairo paints the frame's RGB24 rendition, a
native backend composes its float one. A frame made for that output is its
own pixels; one in another profile or format, made before the output
changed, is shown converted, never refused, as GTK4's renderers and Chromium
convert at draw. It stays until its producer renders it again: an output
change moves the window's serial and redraws it, and the producers compare
the serial there ([output description](#output-description)).

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

A window's backend is layered while its platform color-manages the
window's tagged content to the panel and the transport sets up, otherwise
cairo. With the preference `ui_last/display_force_cairo` set
(preferences > miscellaneous), every window is a cairo window
([output description](#output-description)): an escape hatch for
compositor or driver bugs in a native backend. The key is read whenever
the backend is chosen, and closing the preferences dialog
(`DT_SIGNAL_PREFERENCES_CHANGE`) chooses every window's backend again.

The platform (`dt_presenter_platform_t` in `gui/presenter_backend.h`)
watches the window for as long as it exists, whatever its backend, and its
verdict can change while the window exists: a Wayland output switches
color mode, or the window moves to a monitor described otherwise; on
Windows, Advanced Color is set per monitor and toggled by the user. The
backend is chosen when the window's first presenting widget draws and
again on every new verdict (`dt_presenter_window_choose()` in
`gui/presenter.c`), as Chromium picks the output color space again for the
display a window is on. When the choice changes, every presenting widget's
backend data goes, kept presentations included, the old backend is freed,
the new one reports the window's output if it is native, or the core gives
the output back to the system display profile, and the window draws again.
Where no transport exists, as on X11 and Windows today, a window
has no platform and is a cairo window for as long as it exists.

The core, `gui/presenter.c`, keeps the widget records, the references on the frames
of each widget's last draw, the test for a draw GTK clipped to a damaged
area, and the matrix of each image; it has no branch on the backend. A
backend (`dt_presenter_backend_t` in `gui/presenter_backend.h`) begins a
widget's draw and returns the context to draw on, shows an image, paints
the kept presentation, ends the draw and clears the widget, and with
`target()` prepares showing frames rendered for a new headroom target. Each keeps
the presentation its own way, with the same meaning: what the widget's
retained draw showed.

### cairo

`gui/presenter_cairo.c`. The widget draws on GTK's own context, and each
frame's RGB24 pixels are painted there: frames for a cairo window are made
in RGB24, so a frame in the window's encoding is neither converted nor
copied on the GUI thread; one made for an earlier output is converted
there once.

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
its own above GTK's surface, from bottom to top (above, not below: on
Windows DirectComposition always composes above a window's GDI content, so
an underlay would not port):

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
`dt_presenter_window_set_output()`, at setup and whenever it changes, and
the platform the screen's headroom through
`dt_presenter_window_report_headroom()`; the image headroom of the
window's output is what the transport encodes for, which follows the
core's [headroom target](#headroom-target). The
pipes of that window then render to that output, and everything GTK draws
there converts to the device profile
([output description](#output-description)).

Code is split by what it works around:

- `gui/presenter_layers.c`: the layered backend, the GTK3 part every
  native platform shares. It composes the image layer and the foreground,
  keeps the presentation, and computes the layers' size, scale and
  position in the toplevel. A GTK4 port, where widgets draw textures with
  color states, deletes it.
- `gui/presenter_<platform>.c`: the platform, which tells whether the
  window is native and reports the screen's headroom, and the transport and
  its timing: output description, buffers, and `reserve()`,
  `commit()`, `clear()`, `release()`, `free()` and `target()`, which
  describes the image for a new headroom target (`dt_presenter_transport_t` in
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
  That point is verified on Wayland only: before GDK's commit in
  `after-paint` ([Wayland](#wayland)). On macOS it is expected at the end
  of the presenting widget's draw, inside `GdkQuartzView`'s
  `updateLayer`, and on Windows after GTK's paint; both are open, to be
  verified by their backends.

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
The platform keeps the globals, GDK's surface, its feedback and the
preferred description; the transport the layers, their buffers and its
image descriptions, made when the window becomes native and destroyed when
it stops.

The platform also leaves mutter's default color mode to cairo
(`_mutter_default_color_mode()`, [Workarounds](#workarounds)): the registry
announces `gtk_shell1`, GTK's private protocol, which mutter implements,
`XDG_CURRENT_DESKTOP` lists `GNOME` (Wayfire's gtk-shell plugin announces
`gtk_shell1` too), and the surface's preferred description is plain SDR
sRGB: sRGB primaries and D65 white within 1e-3,
transfer function srgb, gamma22 or compound_power_2_4, no ICC file, and a
target max luminance at most the reference, or no luminances. mutter then
passes untagged pixels to the panel unchanged and only programs the gamma
ramp with the profile assigned in Settings > Color (mutter#4597), so the
window is a cairo window on its system display profile, which on GNOME is
that profile, from colord, as on X11. Any other compositor describing plain
SDR sRGB stays native. The test is made on every preferred description,
so a window follows a switch of the color mode or a move to a monitor in
another one ([Backends](#backends)). Limit: sway with a `color_profile`
set only in sway describes its output as sRGB while converting
everything to that profile. A native window leaves that conversion to
sway, which converts from sRGB, so its image is limited to sRGB there; a
cairo window on that output, forced or darktable on XWayland, converts
to the profile a second time when a tool such as xiccd also publishes it
to colord or the X atom.

The backend reports target max (else max) over reference luminance of the
surface's preferred description as a hint, not a measurement, re-read
whenever the preference changes: no value (NAN) without a reference luminance,
with a ceiling of 1 without `set_luminances`. A preferred description that
differs in more than its luminances (primaries, white, transfer function,
an ICC file) starts a new generation; KWin's HDR brightness moves only the
reference luminance. The image description is made for the core's
[headroom target](#headroom-target), never for the raw preference:
ext_linear, BT.2020, luminances (0, ceil(headroom x 203), 203), so a value
v is sent as v x 203 / max, clipped to [0, max / 203]: the reservation's
clip range and matrix diag(203 / max). Max is rounded up to whole nits, so
that range holds the target. The output is
linear BT.2020 with the headroom of the description in use, max / 203 (1
without `set_luminances`), the range the transport clips to rather than
the target it was made for. A new description is set on the image layer
and published when it is ready, and applies with that layer's next
commit, which is the first buffer encoded for it: the window draws again
and the image is composed again for the new headroom, whether or not its
frames change. A failed one is dropped, the one in use stays, and the next
target is described again. Setup waits for the events that end it, the
preferred description's information and the transport's first
description, however many round trips they take; a failed first
description leaves the window on cairo. Preference changes during setup
do not drop the description it waits for: the latest preference is read
once setup is over. Layers are synchronized subsurfaces
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

### macOS

`gui/presenter_quartz.m`, built on macOS when GTK has the Quartz backend
and CMake finds an Objective-C compiler; it links Metal and QuartzCore.
macOS color-manages every tagged layer, so the platform's verdict is
always native. The platform keeps the toplevel's `NSWindow` and GDK's
`NSView` and watches the window's screen; the transport adds, for each
presenting widget, two views above GDK's view that take no input
(`hitTest:` returns nil). The image layer is a `CAMetalLayer` in RGBA half
floats tagged extended linear BT.2020 with
`wantsExtendedDynamicRangeContent`; the foreground above it is a
`CAMetalLayer` in BGRA8 tagged sRGB, what GTK tags its own surface with.
Each layer is written by a blit from one of two shared Metal buffers, the
one the GPU has finished reading, into a drawable. `reserve()` takes both
buffers and both drawables, so that `commit()` cannot fail; without the
foreground's it refuses the present, which is tried again next frame,
and without the image's it gives no image buffer. `nextDrawable` blocks
for up to a second while every drawable of a layer is on screen or
queued. Without a Metal device the transport does not set up and the
window is on cairo.

The output is linear BT.2020 and the device encoding sRGB, both with a
relative colorimetric intent: Core Animation offers no rendering intent,
and both spaces are matrix based. EDR puts 1.0
at SDR white and lets values go up to the screen's current headroom,
which macOS clips to, so the reservation's matrix is the identity and its
clip range [0, target]. Only that range changes with the
[headroom target](#headroom-target), so `target()` publishes the output
with the new headroom at once.

The platform reports the screen's `maximumExtendedDynamicRangeColorComponentValue`,
or NAN until the image layer has presented a frame (macOS reports about
1.0 before EDR is engaged, 1.2 on an idle built-in XDR), and NAN again
after the layers are cleared or the displays wake, until the image layer
presents again; with
`maximumPotentialExtendedDynamicRangeColorComponentValue` as the ceiling;
with a reference preset
(`maximumReferenceExtendedDynamicRangeColorComponentValue` above 0) both
are the reference headroom. It reports on every
`NSApplicationDidChangeScreenParametersNotification`, which macOS posts
about every 8 ms while an EDR ramp or a brightness change runs. A move to
another screen (`NSWindowDidChangeScreenNotification`) starts a new
generation. A display wake (`NSWorkspaceScreensDidWakeNotification`) is the
same screen, so it only stops counting values and asks for a draw, until
the layers present again: the target from before the sleep stays, and
what macOS reports then goes through the usual settling. A screen
without EDR has a potential headroom of 1: the window stays native, with
wide gamut and a target of 1.

Layers are presented at the end of the frame clock's `paint`, as on
Wayland, and from a run loop observer that runs just before Core
Animation commits (`kCFRunLoopBeforeWaiting`, order 1999999), which
catches a draw made outside `paint`. A present is one Core Animation
transaction, and both layers have `presentsWithTransaction` on, so their
drawables apply with a transaction commit and never on their own. GTK3
draws popovers into the toplevel's view, below the layers, so the
transport declares `popovers_above` false and the layered backend replays
them.

## Workarounds

Each lives in one file and goes away with the limitation it works around.
GTK4 removes the layers and their companions only on Wayland: it has no
output color states on Windows and macOS, so a native backend there would
keep its layers under GTK4 as well.

| Workaround | Where | Works around | Removed when |
|---|---|---|---|
| image and foreground layers above GTK's surface | `presenter_layers.c` | GTK3 cannot give a widget a surface with a color state | GTK4: the image is a texture with a color state |
| `gtk_widget_draw()` replay into the foreground of what GTK stacks above a presenting widget: GtkOverlay children, and the toplevel's popovers where the transport's layers cover them; saved and put back on a kept foreground; a popover's changes draw the presenting widgets again | `presenter_layers.c`, `_replay_above()` | the layers cover GTK's drawing of those widgets (toasts, masks panel, second window buttons), and on Quartz and Win32 of popovers, which GTK3 draws into the toplevel there | same |
| composing and presenting in the frame clock's `paint` signal, after GDK's handler | `presenter_wayland.c`, `_paint()` | GTK3 runs all draws in `paint` and GDK commits its surface in `after-paint`; the layers must be ready in between | same |
| GTK's surface taken as sRGB, the window's device encoding | `presenter_wayland.c`, `_set_output()` | GTK3 does not tag its surface; an untagged surface is sRGB to the compositor, with a transfer function of its choosing | GTK tags its surfaces |
| mutter's default color mode (`gtk_shell1`, a GNOME session and a plain SDR sRGB preferred description) left to cairo with the system display profile | `presenter_wayland.c`, `_mutter_default_color_mode()` | mutter's default color mode composites as sRGB and does not apply the profile assigned to the screen (mutter#4597) | mutter describes its real output (mutter MR !5177); nothing to do, the predicate stops matching |
| presenting at the end of `paint` and from a run loop observer before Core Animation commits | `presenter_quartz.m`, `_paint()`, `_transport_new()` | the layers must not appear before GTK's drawing of the same frame, and GTK3 on Quartz documents no point between its draws and the commit of its view | same |
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
thumbnails, encoded for the main window's registry entry, its device
encoding, so they are shown as the rest of GTK's surface is. Thumbnails
already in the cache keep the profile they were encoded for when that
changes, as in master; the option's description warns of it.
They follow the window's output once thumbnails go through the presenter.

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
