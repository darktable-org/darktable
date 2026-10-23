# Flexi masks: data model

How a flexi mask is stored: a module's mask as a tree of groups, in the
structures darktable already has. There is no database schema change.

A module whose `mask_mode` has `DEVELOP_MASK_FLEXI` renders its mask
through `_group_get_mask_roi_flexi` in `masks/group.c`. Classic masks are
converted to this model when an edit is loaded, by `masks/migrate_legacy.c`.

## Summary

A module's mask is a tree of groups. Each group folds its members in list
order with one operator, then applies its own refinement, inversion and
opacity. A member is an element (a drawn shape, a parametric channel, a
raster mask reference or an AI object) or another group. Every kind of member
yields a 0-1 value per pixel and combines the same way.

## Storage

### Masks version 7

`DEVELOP_MASKS_VERSION` is 7. Besides the AI object's reference to its stored
pixels, the v6 to v7 step appends fields to the group point
`dt_masks_point_group_t`, which grows from 16 to 240 bytes:

| Field | Meaning | Read from a v6 blob |
|---|---|---|
| `refinement` | details, feathering, blur, contrast, brightness; `enabled` is off, element scope or group scope | zero: off |
| `name[128]` | a group's name, on its marker | zero: none |
| `group_opacity` | multiplies a group's folded mask, on its marker | set to 1.0 by the v6 to v7 step |
| `preset_note[64]` | the built-in group layout preset a group was made from, on its marker | zero: none |

A blob stores each point at the size of the masks version that wrote it.
The readers (`dt_masks_read_masks_history` in `masks/masks.c`, and the XMP
format 2 importer in `common/exif.cc`) step through it with
`dt_masks_point_stride` and zero-fill what an older point does not have. A
v6 group point is therefore a prefix of a v7 one.

Older darktable versions cannot read a v7 group blob. Nor can this one read
the group blobs of development builds made before the group point grew, which
already wrote v7 with 16-byte group points; no release did.

### Presets and styles

A preset or a style item stores a module's blend parameters in
`blendop_params`. When their mask is flexi, the forms of the mask group's
tree follow the struct in the same blob, each as a `main.masks_history` row
holds it. `masks/blend_blob.c` defines the format and is the only code that
reads or writes it. A blob without these forms is the plain struct, as it
was before. Applying the blob gives the forms fresh ids
(`dt_masks_blend_blob_remap`), so two applies never share a form.

An AI object stored as pixels is left out of the blob, along with the group
members that refer to it. Its pixels are in the sidecar of the image version
it was made on, so on any other image it could only show as missing. The
interactive saves ask first (`dt_gui_presets_confirm_pixel_masks`); an
object traced into paths is ordinary shapes and is kept.

Presets and styles made before flexi masks hold a classic `mask_mode` and no
forms, at blend version 14 or older, or at version 15 where a conversion had
no image to create the forms in. `dt_develop_blend_params_read` migrates
them when they are applied, as a history row is migrated.

### Form types

Two form types are new, each with its point struct in `develop/blend.h`:

- `DT_MASKS_PARAMETRIC` (`1 << 9`), `dt_masks_point_parametric_t`: its own
  blendif configuration and the colorspace it was made in. A single-channel
  form (`single`) edits the one channel `channel` names; with `single` 0 it
  is a classic multi-channel parametric mask.
- `DT_MASKS_RASTER` (`1 << 10`), `dt_masks_point_raster_t`: another
  module's raster mask, by that module's operation, instance and mask id.

An AI object (`DT_MASKS_OBJECT`) is a pixel form, as masks v7 stores it
(`masks/pixel_mask.c`): a member like a drawn shape. Traced into paths
("apply as paths"), it is an ordinary group of paths.

### State bits

The `state` of a group point gains bits that were unused before:

| Role | Bits |
|---|---|
| marker: this point is a group's record, not a member | `GROUP_MARKER` (18) |
| a group's operator, on its marker (none = maximum) | `FLEXI_MINIMUM` (12), `FLEXI_SCREEN` (9), `FLEXI_PRODUCT` (15), `FLEXI_SUM` (19), `FLEXI_DIFFERENCE` (20), `FLEXI_EXCLUSION` (21) |
| a group's modifiers, on its marker | `OP_INVERT` (16), `OP_DISABLE` = `OP_BYPASS` (14) |
| a member's flags | `HIDDEN` (8, set by solo), `DISABLE` (17) |

`DT_MASKS_STATE_OP` covers a classic member's operator and a group's
modifiers, `DT_MASKS_STATE_FLEXI_OP` a group's operator. Static asserts in
`develop/masks.h` keep these roles and the marker bit from overlapping:
the bits are stored, so a clash could not be fixed by renumbering.

### Blend parameters

`mask_mode` gains `DEVELOP_MASK_FLEXI` (`1 << 4`): `mask_id` then names the
module's mask group, the root of its tree.

The exception is an `IOP_FLAGS_NO_MASKS` module (retouch, spots): its
`mask_id` holds the shapes it uses itself in `process()`, so its mask group
is in `blend_mask_id` instead. Resolve the mask group with
`dt_develop_blend_mask_group_id()`, never from `mask_id` directly. Such a
module's mask panel offers parametric and raster elements but no shapes, and
leaves the canvas to the module's own tools (`bd->shapes_support`).

Blend parameters v15 have the same layout as v14. The version bump routes
every older edit through `dt_develop_blend_legacy_params_ext`, which runs
the migration. `mask_lock` and `blend_mask_id` take over the two reserved
fields: a locked mask survives reset, presets, styles and paste. Every
legacy conversion clears both, since the reserved fields were never
guaranteed to be zero.

## The tree

Three words name the parts of the tree, in the code, the panel and the
documentation alike: an **element** is a leaf, a **group** holds other
parts, and a **member** is either one, as seen from the group that holds it.
The mask's own group is a group but no group's member. "Shape" names one
kind of element, a drawn one.

- **Element.** A leaf: a drawn shape, a parametric channel, a raster mask
  reference or an AI object. It is never a group.
- **Group.** A `DT_MASKS_GROUP` form whose point list starts with a
  **marker**. The marker refers to no form: its `formid` is an id of its
  own, from the same id space as the forms, so it can never be mistaken
  for one. It holds the group's settings once: operator, inversion, bypass,
  opacity, refinement, name and preset note. The points after it are the
  members.
- **Member.** A point that refers to a form, an element or a group. It
  carries the member's own opacity, inversion (`INVERSE`), element
  refinement, `HIDDEN` and `DISABLE`. A member that refers to a group
  carries no settings of its own: that group's marker holds them.
- **Nesting.** A walk follows groups nested in groups at most
  `DT_MASKS_NESTING_MAX` (8) deep, which is also how deep the panel lets
  groups nest.
- **Refinement scopes.** Element scope applies to one member's mask before
  it is folded in, group scope to a group's folded mask, and the module's
  own refinement controls (blend parameters) to the whole mask, as for a
  classic mask.

## Operators

A group folds its visible members in list order:

| Operator | Result |
|---|---|
| maximum (none set) | max |
| `FLEXI_SCREEN` | a + b - ab |
| `FLEXI_MINIMUM` | min |
| `FLEXI_PRODUCT` | a * b |
| `FLEXI_SUM` | min(1, a + b) |
| `FLEXI_DIFFERENCE` | the first visible member, less each later one |
| `FLEXI_EXCLUSION` | classic's exclusion, member by member |

One ordered operator per group is a left fold, which is what a classic
group does along its list. Where a classic list changes operator, what
comes before becomes the first member of a new group, so classic's
arithmetic carries over unchanged. Difference and exclusion keep their
classic meaning; exclusion is not associative, so the order of its members
is part of the result.

After the fold, a group applies its refinement, then `OP_INVERT`, then
`group_opacity`.

## Why markers

A group's settings need somewhere to live. The alternatives were:

- **A group table in the database.** It needs a schema change, and a rule
  for a group whose last member is deleted. An empty group is a marker
  with no members.
- **Settings on the `DT_MASKS_GROUP` form.** The form row has no field for
  them, so this also needs a schema change. Classic puts them on the
  reference to a group, but the module's mask group has no reference.
- **Groups as runs of one flat list**, the settings copied onto every
  member. The copies must be kept in sync, two adjacent groups with the
  same operator need a boundary, and an empty group cannot be stored.

Markers, the new form types and the new fields all fit in the points blob,
which is versioned per form, and an old blob reads as a prefix of the new
struct.
