# Flexi masks: data model

How a flexi mask is stored: a module's mask as a tree of groups, in the
structures darktable already has. There is no database schema change.

A module whose `mask_mode` has `DEVELOP_MASK_FLEXI` renders its mask
through `_group_get_mask_roi_flexi` in `masks/group.c`. Classic masks are
converted to this model when an edit is loaded, by `masks/migrate_legacy.c`.

## Summary

A module's mask is a tree of groups. Each group folds its members in list
order with one operator, then applies its own refinement, inversion and
opacity. A member is a drawn shape, a parametric channel, a raster mask
reference, an AI object or another group. Every kind of member yields a 0-1
value per pixel and combines the same way.

## Storage

### Masks version 7

`DEVELOP_MASKS_VERSION` is 7. The v6 to v7 step appends fields to the group
point `dt_masks_point_group_t`, which grows from 16 to 240 bytes:

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

Older darktable versions cannot read a v7 group blob.

### Form types

Two form types are new, each with its point struct in `develop/blend.h`:

- `DT_MASKS_PARAMETRIC` (`1 << 9`), `dt_masks_point_parametric_t`: its own
  blendif configuration and the colorspace it was made in. A single-channel
  form (`single`) edits the one channel `channel` names; with `single` 0 it
  is a classic multi-channel parametric mask.
- `DT_MASKS_RASTER` (`1 << 10`), `dt_masks_point_raster_t`: another
  module's raster mask, by that module's operation, instance and mask id.

An AI object (`DT_MASKS_OBJECT`) is stored like a group: its point list
holds group points referring to its paths.

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

Blend parameters v15 have the same layout as v14. The version bump routes
every older edit through `dt_develop_blend_legacy_params_ext`, which runs
the migration. `mask_lock` takes over the first reserved field: a locked
mask survives reset, presets, styles and paste. Every legacy conversion
clears it, since the reserved field was never guaranteed to be zero.

## The tree

- **Group.** A `DT_MASKS_GROUP` form whose point list starts with a
  **marker**. The marker refers to no form: its `formid` is an id of its
  own, from the same id space as the forms, so it can never be mistaken
  for one. It holds the group's settings once: operator, inversion, bypass,
  opacity, refinement, name and preset note. The points after it are the
  members.
- **Member.** A point that refers to a form. It carries the member's own
  opacity, inversion (`INVERSE`), element refinement, `HIDDEN` and
  `DISABLE`. A member that refers to a group carries no settings of its
  own: that group's marker holds them.
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
