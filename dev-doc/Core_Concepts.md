# darktable core concepts: images, modules, and pipes

darktable produces a displayed or exported image by passing image data through
a sequence of _image operations_ (_IOPs_). Each one might change exposure, crop
the image, or convert its colors. darktable records their settings in the
_edit history_, which it stores in the library database and, by default, in an
XMP sidecar file. It renders the image again from the source file and the edit
history each time it needs to display or export it. Editing never changes the
source image file.

## The main objects

| Term in code | Simple meaning |
| --- | --- |
| _Image operation_ (_IOP_), also called a _module_ or _processing module_ | One kind of image change, such as exposure. Its code lives in `src/iop/`. |
| _Module type_ (`dt_iop_module_so_t`) | The loaded code and resources shared by all instances of one IOP, such as OpenCL kernels. `so` means "shared object": each IOP is built as a separate plugin library. |
| _Module instance_ (`dt_iop_module_t`) | One use of an IOP in an edit, with its own settings. In the darkroom, an instance is usually shown as a module panel (some, like `finalscale`, never have a visible UI panel); export (including `darktable-cli`) uses instances without panels. Users can usually add, remove, enable, disable, or reset instances. |
| _Pixelpipe_ (`dt_dev_pixelpipe_t`, often called a _pipe_) | The pipeline used to produce a particular output, such as the main darkroom view, its smaller preview or an exported image. Each pipe has its own sequence of pieces (next row), one per module instance. |
| _Piece_ (`dt_dev_pixelpipe_iop_t`) | One module instance's processing state in one pipe. It holds prepared processing data; `piece->module` points back to the module instance. |
| `dt_image_t` | Information about an image, such as its ID and dimensions. It is not the pixels being processed. |
| _dev_ (`dt_develop_t`) | A working context for processing one image at a time. It holds image information (`dt_image_t`), module instances, edit history, and, when used for display, pipes. |

"Module" has other meanings in darktable too: _utility modules_ (the side
panels of each view, in `src/libs/`) and _view modules_ (lighttable, darkroom,
and others, in `src/views/`).

In IOP code, a variable named `module` usually means a module instance. So
does the parameter `self` in most IOP functions, and `self->dev` points back to
its dev. In `init_global()` and `cleanup_global()`, `self` is the module type
instead.

## How the objects connect

A dev holds module instances and edit history. It has at least one instance of
every IOP, whether or not the edit uses it. An instance with no history items
keeps its default settings and default enabled state, which is off for most
IOPs.

A dev used for display has three pipes: main-view, preview, and second-window.
A dev used for export has none of its own; export creates a pipe and fills it
from that dev's module instances and history. Each pipe has one piece per
module instance, including disabled ones. Pieces are not copies of history
items. Repeated edits to the same exposure module instance update one piece per
pipe; adding another exposure module instance gives each pipe a second piece.

A module instance holds editable settings and whether it is enabled. In the
darkroom it can also hold widgets; export uses module instances without them.
Each piece holds processing data prepared for its pipe and its own enabled
state. The pipe or the module's `commit_params()` can override that state for
this pipe only; for example, demosaic is switched off for an image that isn't
raw. `process()` reads `piece->data`.

When darktable rebuilds a pipe's settings, it starts each piece with default
settings, then applies the active history items in order. An edit that only
changes the newest history item updates just the pieces of that module instance.
An edit that adds a history item rebuilds the settings of all pieces as above.
See [Module Lifecycle](Module_Lifecycle.md#3-editing-operations) for when each
case applies.

darktable can create several dev objects:

- the main darkroom one, which is reused when you switch images
- separate ones for slideshow images, darkroom snapshots, duplicate manager
  previews, and the image pinned in the second window
- ones without pipes of their own: for exports (including `darktable-cli`),
  and short-lived ones for operations such as copying history or applying
  styles

Lighttable and map normally display cached thumbnails rather than owning a dev
each. A missing thumbnail is rendered through the export code, which uses a
temporary dev.

## Settings and processing

| Term in code | Simple meaning |
| --- | --- |
| `self->params` (`params_t`) | The module instance's current editable settings. These are the values that edit history records. |
| _History item_ (`dt_dev_history_item_t`) | A saved step in an image's edit history: a module instance's settings, whether it was enabled, and related blending settings. The user can select an earlier step in the history panel; the later items stay saved but are not applied. The applied items are the _active_ history. |
| `piece->data` (often `data_t`) | The settings prepared for this pipe to use while processing. A simple IOP may keep a copy of `params_t` here; another may calculate a different `data_t`. |
| `self->data` (module type level) in `init_global(dt_iop_module_so_t *self)` | Resources shared by all module instances of one type, often OpenCL kernels. Each instance accesses them through its `global_data` field. |
| `self->data` (module instance level) in functions using `dt_iop_module_t *self` | Optional data shared by one module instance's pipes. Pipes run in parallel threads, so access must be locked. For example, `rasterfile` caches a _mask_ (an image marking where an edit applies) loaded from a file, and protects it with a mutex. |
| `self->gui_data` (often `gui_data_t`) | Widgets and other interface state. It is absent when there is no module GUI, such as during export. |

When settings change, darktable records the edit in edit history and updates
the affected pipes. `commit_params()` prepares each pipe's `piece->data`
from the settings it is given. `process()` then reads that data and transforms
input pixels into output pixels. Some IOPs can instead use `process_cl()` on an
OpenCL device (usually a GPU); darktable falls back to `process()` if OpenCL
fails.

The pipe can reuse a cached result when its inputs and settings still match.
This avoids repeating work when only part of the edit has changed.

### Settings, prepared data, and instance data

`params_t` holds settings in a form that suits the user and the edit history;
`data_t` holds them in a form that suits the processing code, and
`commit_params()` converts one into the other. For example, color equalizer's
white level is set in EV but processed as a multiplier, so `commit_params()`
stores 2^EV (`colorequal.c`). Color balance rgb's hue shift is set in degrees
and converted to radians (`colorbalancergb.c`).

Instance-level `self->data` lies between the two. It lasts as long as the
module instance, across commits and renders, but it is never saved. It is not
shared between instances: each instance has its own, shared by that instance's
pipes and its GUI. The module decides when to refresh it. The data can outlive
an image: when you switch images in the darkroom, one instance of each IOP is
kept and reused. So `rasterfile` keys its cached mask on the settings and the
image ID. It reloads the mask when either one changes, and also retries after
a failed load.

## Other terms

- _Processing order_: IOPs run in a defined sequence. A later IOP receives
  the earlier IOP's output, so order can change the result. This is different
  from edit history, which records changes over time.
- _ROI_ (_region of interest_): The area and scale of the image requested for
  a processing step. The main view, preview, and export can request different
  regions or sizes. IOPs that change geometry (crop, rotate, lens correction)
  convert between their input and output ROI; see
  [Regions of Interest](pixelpipe_architecture.md#regions-of-interest-roi).

For details, see [IOP Module API](IOP_Module_API.md) for module settings and
callbacks, [Module Lifecycle](Module_Lifecycle.md) for when those callbacks
run, and [Pixelpipe Architecture](pixelpipe_architecture.md) for pipe
execution and caching. The corresponding types are declared in
[`develop.h`](../src/develop/develop.h),
[`imageop.h`](../src/develop/imageop.h), and
[`pixelpipe_hb.h`](../src/develop/pixelpipe_hb.h).
