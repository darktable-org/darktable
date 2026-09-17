# Introspection System

The **introspection system** in darktable is a powerful mechanism that allows the application to understand and manipulate the internal structure of C structs at runtime. It is primarily used for **Image Operation (IOP) modules** to define their parameters.

## Purpose

Introspection describes each field of a module's params struct: its name, type, offset and size, and the values of its `$MIN`, `$MAX`, `$DEFAULT` and `$DESCRIPTION` tags. The framework, and code outside the module, use that description for:

1.  **Default parameters**: `dt_iop_default_init()` sizes `params` and `default_params` from the introspection data and writes each field's `$DEFAULT` into `default_params` (`src/develop/imageop.c`). The framework installs it as the `init()` of every module that does not define one. A module defines its own `init()` for what the tags cannot set: `default_params` values no `$DEFAULT` can express, such as a string or an array whose elements differ (`watermark`, `basecurve`), and module state outside the params, such as `default_enabled`, `hide_enable_button` or `request_histogram` (`colorout`, `tonecurve`). An `init()` that leaves the other fields to the tags calls `dt_iop_default_init()` before anything else, since it allocates both blocks and resets `default_enabled` to FALSE. An `init()` may instead allocate both blocks and fill `default_params` itself, as `filmic` does (`src/iop/filmic.c`), leaving `params` to the framework, which copies `default_params` into it (`dt_iop_load_default_params()`, `src/develop/imageop.c`); such an `init()` must set `params_size` (`src/iop/iop_api.h`) and zero both blocks (see [Serialization and Initialization](#serialization-and-initialization)). A module without introspection has to define its own `init()`: `dt_iop_default_init()` reads the introspection data without checking that it exists.
2.  **GUI generation**: `dt_bauhaus_slider_from_params()`, `dt_bauhaus_combobox_from_params()` and `dt_bauhaus_toggle_from_params()` (`src/develop/imageop_gui.c`) look a field up by name and create a widget bound to it and labeled from it. Each accepts only some field types (see [Usage in GUI](#usage-in-gui) and [imageop_gui.md](imageop_gui.md)):
    - a slider takes a float, int or unsigned short field, with `$MIN` and `$MAX` as its range, and the field's value in `default_params` as its default
    - a toggle takes a bool field, with the same default
    - a combobox takes an enum field, with an entry for each constant that has a `$DESCRIPTION` (see [Metadata Tags](#metadata-tags)), or a bool field, with "no" and "yes"; both get the same default. It also binds an int or unsigned int field, but adds no entries and sets no default for it
3.  **Describing and checking values**: the history panel's tooltip walks the introspection tree to list, field by field, what a history item changed from the module's previous one (`src/libs/history.c`). With `-d params`, `dt_iop_commit_params()` checks the fields of an enabled module against their declared ranges and logs the ones outside them (`src/develop/imageop.c`; see [sliders.md](sliders.md#31-range-and-limits)).
4.  **Field access by name**: a module's `get_p()` returns a pointer to a named field inside a params block, so other code can read or write the field without the struct's definition. darktable's core reads `colorin`'s and `colorout`'s profile, `flip`'s `orientation` and `overlay`'s `imgid` this way (`src/common/colorspaces.c`, `src/common/iop_profile.c`, `src/imageio/imageio.c`, `src/common/image.c`, `src/common/overlay.c`), and `dt_image_set_flip()` writes `orientation` into a zeroed block to build a new `flip` history entry (`src/common/image.c`). The lookup compares strings and returns NULL for a name it does not know, so renaming one of these fields still compiles, and breaks the code that uses it at run time. `get_p()` also knows only the current layout, while a block read from the database keeps the version it was written with (the history row's `module` column). Reading such a block needs that version checked first, and an older block converted with `dt_iop_legacy_params()`, which takes a module instance; the database readers in `colorspaces.c`, `image.c` and `overlay.c` do neither.
5.  **Scripting access**: The `darktable-mcp` server (`src/mcp/dt_bridge.c`, built unless `USE_MCP` is off) uses introspection to describe a module's scalar fields to its clients and to set them by name, refusing a number outside the field's `$MIN`/`$MAX`. Other fields, such as `char` arrays, are neither listed nor settable by name: a client passes the whole params block instead, its bytes written as a hex string (`blob_hex`). Lua does not use introspection, and no params field is exposed to Lua scripts. A script changes one setting through the action that drives the module's widget (`darktable.gui.action`, `src/lua/gui.c`), and whole edits through styles, sidecar files, or a duplicate made with its history.

Storing params does not need introspection. History, presets and styles store, copy and hash them as raw bytes (see [Serialization and Initialization](#serialization-and-initialization)). Code that builds a params block field by field, such as `dt_image_set_flip()` and the MCP server, uses introspection to fill the block, and stores the result the same way. The version stored with them is `DT_MODULE_INTROSPECTION`'s first argument, which the macro passes on to `DT_MODULE`, dropping the struct type (`src/common/darktable.h`). Params stored in an older version are upgraded by the module's `legacy_params()`, called through `dt_iop_legacy_params()` (`src/develop/imageop.c`; see [Versioning](#versioning)), when history, a style or a module's presets are loaded (`src/develop/develop.c`, `src/common/styles.c`, `src/develop/imageop.c`). The MCP server converts nothing: it refuses a params block whose size is not the current one (`src/mcp/dt_bridge.c`), and reads one of the current size with the current layout.

## Defining Introspection

Introspection is defined directly in the C code using the `DT_MODULE_INTROSPECTION` macro. This macro associates a struct type with a version number.

Example from `src/iop/exposure.c`:

```c
DT_MODULE_INTROSPECTION(7, dt_iop_exposure_params_t)

typedef enum dt_iop_exposure_mode_t
{
  EXPOSURE_MODE_MANUAL,   // $DESCRIPTION: "manual"
  EXPOSURE_MODE_DEFLICKER // $DESCRIPTION: "automatic"
} dt_iop_exposure_mode_t;

typedef struct dt_iop_exposure_params_t
{
  dt_iop_exposure_mode_t mode;      // $DEFAULT: EXPOSURE_MODE_MANUAL
  float black;                      // $MIN: -1.0 $MAX: 1.0 $DEFAULT: 0.0 $DESCRIPTION: "black level correction"
  float exposure;                   // $MIN: -18.0 $MAX: 18.0 $DEFAULT: 0.0
  float deflicker_percentile;       // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 50.0 $DESCRIPTION: "percentile"
  float deflicker_target_level;     // $MIN: -18.0 $MAX: 18.0 $DEFAULT: -4.0 $DESCRIPTION: "target level"
  gboolean compensate_exposure_bias;// $DEFAULT: FALSE $DESCRIPTION: "compensate exposure bias"
  gboolean compensate_hilite_pres;  // $DEFAULT: TRUE $DESCRIPTION: "compensate highlight preservation"
} dt_iop_exposure_params_t;
```

### Metadata Tags

The comment on the same line as a field or enum constant is **parsed** during the build process to generate metadata. Put the tags there: the parser looks them up by the line the declaration is on (`tools/introspection/ast.pm`), so tags in a comment on the line above are silently ignored. The supported tags are:

-   `$MIN`: The minimum value. `dt_bauhaus_slider_from_params()` makes it the slider's hard minimum, but nothing enforces it on the field itself; see [sliders.md](sliders.md#31-range-and-limits).
-   `$MAX`: The maximum value, used and not enforced in the same way.
-   `$DEFAULT`: The default value.
-   `$DESCRIPTION`: A human-readable name. On a field, the `dt_bauhaus_*_from_params()` functions use it as the widget label, and fall back to the field name with underscores turned into spaces (`src/develop/imageop_gui.c`). On an enum constant, it is the text of that entry in the combobox. A constant without one is left out of the combobox, not shown by its name: the generated entry carries an empty description (`tools/introspection/ast.pm`), and the combobox skips entries with empty text (`src/bauhaus/bauhaus.c`).

An enum needs no tag listing its values: introspection collects the constants from the `typedef enum` itself.

## Internal Structure

The introspection data is compiled into a tree of `dt_introspection_field_t` structures.

### `dt_introspection_t`
The top-level descriptor for a params type.
-   `params_version`: The version number passed to `DT_MODULE_INTROSPECTION`.
-   `size`: The size of the struct in bytes.
-   `field`: The root of the tree, a `DT_INTROSPECTION_TYPE_STRUCT` field describing the whole struct, with an empty name and offset 0. Its members are `field->Struct.fields`, a NULL-terminated array of `field->Struct.entries` pointers.

### `dt_introspection_field_t`
Describes a single field in the struct. It is a union of various types (`Float`, `Int`, `Bool`, `Enum`, `Struct`, etc.), all sharing a common header.

#### `dt_introspection_type_header_t`
-   `type`: The type enum (`DT_INTROSPECTION_TYPE_FLOAT`, etc.).
-   `name`: The field's path from the params struct, with parent struct names separated by `.`, as in `curve_nodes[0][0].x`; `field_name` is the last part alone.
-   `offset`: The byte offset of the field from the start of the params struct, not from its parent. Add it to the params pointer as it is: adding the parent's offset as well counts the parent twice.
-   `size`: The size of the field.

A field inside an array is described once, as its first element. `rgbcurve`'s node array has a single descriptor for `x`, named `curve_nodes[0][0].x`, and its `offset` points into `curve_nodes[0][0]`, so it cannot locate any other node. To reach another element, start from the array's own descriptor and step by the element size, once per dimension. The helpers in `src/common/introspection.h` do the stepping. Each returns a pointer to the data it selects, or NULL, and stores that data's descriptor in its last argument. `dt_introspection_access_array()` selects an array element. `dt_introspection_get_child()` selects a struct member by name, from the difference between the member's offset and the struct's. Pass each call's pointer and descriptor to the next:

```c
dt_introspection_field_t *f = self->get_f("curve_nodes");
dt_introspection_field_t *row = NULL, *node = NULL, *x = NULL;
void *p = (uint8_t *)self->params + f->header.offset;
p = dt_introspection_access_array(f, p, c, &row);    // curve_nodes[c], itself an array
p = dt_introspection_access_array(row, p, n, &node); // curve_nodes[c][n], the node struct
p = dt_introspection_get_child(node, p, "x", &x);    // curve_nodes[c][n].x
```

`dt_introspection_get_child()` accepts only a struct or union, so it returns NULL if you call it on `row` after a single step.

## Usage in GUI

The `dt_bauhaus_*` family of functions (e.g., `dt_bauhaus_slider_from_params`) uses introspection to bind a widget to a struct field.

When you call:
```c
g->exposure = dt_bauhaus_slider_from_params(self, "exposure");
```

1.  Darktable looks up the "exposure" field in the module's introspection data.
2.  It reads the `$MIN` and `$MAX` values from the introspection data, and the default from the field's value in `self->default_params` (`src/develop/imageop_gui.c`). That value is the `$DEFAULT` tag's only if nothing replaced it after `dt_iop_default_init()`: a module's own `init()` or its `reload_defaults()` may write `default_params`, as `exposure`'s `reload_defaults()` does.
3.  It configures the slider range and default value.
4.  It binds the slider's value to the memory address `(uint8_t *)self->params + field->header.offset`.
5.  When the user moves the slider, the slider writes its new value to that address. A call to `dt_bauhaus_slider_set()` does the same only outside `DT_ENTER_GUI_UPDATE()`; under the guard, which is where `gui_update()` and the framework's own widget sync run, only the widget changes (see [sliders.md](sliders.md#31-range-and-limits)).

## Serialization and Initialization

An IOP `params_t` is not converted to a neutral representation before it is stored. The struct's bytes *are* the record: `params_size` bytes are copied straight out of memory, and copied straight back on load. Everything below follows from that, including the parts that are easy to get wrong.

**Where the bytes go:**

- **Database.** `_dev_write_history_item()` binds the block as a blob into `main.history.op_params` in `library.db` (`src/develop/develop.c`). `data.db` holds the same kind of blob for presets and style items (`src/gui/presets.c`, `src/common/styles.c`).
- **XMP.** Each history entry is written as `Xmp.darktable.history[n]/darktable:params` (`src/common/exif.cc`), encoded by `dt_exif_xmp_encode()`: plain hex, or zlib-compressed and base64-encoded behind a `gz` prefix when compression is on.
- **Exported images.** When the export's metadata flags include `DT_META_DT_HISTORY`, that same XMP history is embedded in the exported file (`src/common/exif.cc`); AVIF and JXL have their own embedding paths (`src/imageio/format/avif.c`, `src/imageio/format/jxl.c`).

**So every byte has to be initialized**, including the ones introspection never names:

- padding the compiler inserts between fields, or after the last one, to satisfy alignment
- the tail of a fixed-size `char[]` past the terminating NUL
- any field a migration or a custom initializer forgets to write

Two things go wrong when those bytes are indeterminate. Whatever the allocator last left in them is written to disk, and travels on into XMP sidecars and exported images that users share. And they reach `dt_iop_commit_params()`, which hashes `module->params` over its full `params_size` (`src/develop/imageop.c`): two blocks differing only in padding hash differently, so the pixelpipe cache misses on an edit that changed nothing.

The framework starts you off correctly. `dt_iop_default_init()` `calloc()`s both `params` and `default_params` before writing the introspected `$DEFAULT` values (`src/develop/imageop.c`) — a change made in `b661b4675c`, after Valgrind reported branches depending on uninitialized values in `colorbalance` and `watermark`. What you must not do is defeat it afterwards:

- a hand-written `init()` that allocates `params` or `default_params` itself has to zero both: `dt_iop_load_default_params()` copies `default_params` whole into `params` (`src/develop/imageop.c`)
- `legacy_params()` must return a fully initialized block: allocate it with `calloc()`, since assigning every named field still leaves the padding indeterminate, and the caller copies the whole `params_size` into the module's params (`src/develop/imageop.c`)
- anything that builds a params block for a preset or a style has the same obligation

`calloc()` is the usual mechanism, not the only acceptable one; the requirement is that the bytes are defined, not that a particular function produced them.

## Versioning

When you change the layout of a `params` struct, you **must** increment the introspection version number.

```c
DT_MODULE_INTROSPECTION(8, dt_iop_exposure_params_t)
```

You must then implement the `legacy_params` function in your module to migrate data from the old version (7) to the new version (8). This ensures that edits made with older versions of darktable are preserved.
