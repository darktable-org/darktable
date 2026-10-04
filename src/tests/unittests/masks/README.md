# Flexi masks panel — behavioural tests

Headless regression tests for the flexi mask panel's *behaviour*: grouping,
drag-and-drop, selection, and cache invalidation. They run in ~0.2s, need no
display, and are the safety net for refactoring the panel.

```sh
cmake -B build-test -S . -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-test -j8
ctest --test-dir build-test -R flexi --output-on-failure
```

| Suite | Covers |
|---|---|
| `test_flexi_model` | grouping and partitioning, element drag-and-drop, the selection state machine, marking classic runs as groups, solo/mute primitives |
| `test_flexi_cache` | which edits must — and must not — invalidate the pixelpipe's mask cache |
| `test_flexi_persistence` | mask blob version migration, i.e. carrying already-saved edits forward |
| `test_flexi_dnd` | the drop paths other than element-onto-element: group headers, empty groups, clusters, deleting and emptying groups |
| `test_flexi_groups` | the solo family's mutual exclusivity, group numbering, refinement scope |
| `test_flexi_panel` | what the panel shows: the no-op badge, adaptive parametric rows, preferences |
| `test_flexi_migrate` | the classic → flexi migration case table (structure, not pixels) |
| `test_flexi_compose` | the mask operators themselves: what union/intersection/… compute, and the algebraic properties the design relies on |

## Why these are unit tests and not simulated GTK

The panel looks like it needs a GUI harness to test. It mostly does not.

Its group model is a plain structure — a tree of `dt_masks_form_t` of type
`DT_MASKS_GROUP`, each one's `points` list holding first a marker record with
the group's own settings (`DT_MASKS_STATE_GROUP_MARKER`), then one
`dt_masks_point_group_t` per member, bottom-up. A nested group is a member
referring to another group form. Every gesture the panel offers is ultimately
a mutation of those lists. The functions that perform those mutations take a mask group and
plain values; the only global they touch is `darktable.develop`, and only to
resolve a formid to a form.

So the mock is a `dt_develop_t` holding a forms list, a module pointing at it,
and a `blend_data` for the panel's scratch state. No `gtk_init`, no display, no
database, no pixelpipe.

Simulating real GTK events against a real widget tree was considered and
rejected: it needs a display in CI, and event-injection tests are flaky enough
that they tend to get disabled rather than fixed — which is worse than not
having them. The seam below buys most of the coverage at none of that cost.

## The seam

A gesture handler is split in two:

- **the GTK handler** decodes the event into plain values (which element, which
  target, above or below) and commits the result afterwards — history, pipe,
  widget rebuild;
- **the model function** performs the gesture, and is what the tests call.

`_masks_row_drag_received` and `dt_masks_model_drop_element_onto_element` in
`blend_gui.c` are the worked example. The handler owns nothing but decode and
commit, so the tests and the real panel run *identical* logic — there is no
second implementation to drift.

Model functions are declared in `develop/blend_gui_internal.h`. Adding a
gesture to the suite means extracting its handler the same way first.

## Layout strings

Scenarios read as the tree the panel shows, members bottom-up:

```
"u{1,2,d{},i{3}}"
```

The mask, a maximum group holding elements 1 and 2, then an empty difference
group, then a minimum group holding element 3. A group's letter is its
operator: `u` maximum, `o` screen, `s` sum, `i` minimum, `m` product,
`d` difference, `x` exclusion. `~` inverts and `@0.5` sets an opacity, of a
group or an element.

```c
dt_masks_form_t *grp = flexi_build("u{1,2,i{3,4}}");
dt_masks_model_drop_element_onto_element(&flexi_module, grp, 1, 3, TRUE);
assert_layout("u{2,i{3,1,4}}");
```

`flexi_layout()` writes a live mask back in the same notation, settings
included, so a layout assertion reads what the panel shows.

## test_flexi_cache — the invalidation contract

`dt_masks_group_hash()` tells the pixelpipe whether a mask still renders the
same. Two opposite failure modes, both invisible until someone notices the
wrong thing happening:

- a rendering input **missing** from the hash → the edit does not appear (stale
  cached mask; "I moved the slider and nothing happened");
- a non-rendering value **included** → everything recomputes on cosmetic
  changes (renaming a group should not re-render the image).

Neither shows up in a pixel-comparison suite: the rendering is correct, it is
the decision to *re*-render that is wrong. Hence a dedicated suite — hash,
mutate one field, hash again, assert whether the two differ.

The negative cases matter as much as the positive ones. Solo-*edit* narrows
which shapes are editable on canvas and must **not** invalidate; solo/mute
(`DT_MASKS_STATE_HIDDEN`) changes what the mask renders to and **must**. Those
two are easy to conflate, so they are pinned apart explicitly — as are
selection, cluster collapse/expand, canvas edit mode, and group renaming.

Some of those negative tests look tautological today, because the state they
poke lives in `blend_data` rather than in the group. That is what makes them
worth keeping: they are the tripwire for someone later storing presentation
state inside `dt_masks_point_group_t`, where it would silently start dragging a
full mask recompute behind every cosmetic click.

**When you add a value the group renderer reads, add a test here — and when you
add panel state that it doesn't, add one too.** That is the whole contract.

## test_flexi_persistence — version migration

`dt_masks_legacy_params` carries every already-saved edit forward when the group
point struct gains a field. It deserves its own suite because it runs against
data nobody can regenerate (a user's existing library), it fails silently, and
it is the one place where a zero-filled field is not automatically safe:
appended fields are read at the historic stride and zero-filled, which is
neutral for most of them, but `group_opacity` is multiplicative — a zero-fill
would blank out every pre-v7 group's mask.

The tests cover the v6 → v7 step that runs after the read, and the read-time
stride (`dt_masks_point_stride`).

## test_flexi_compose — what the operators mean

`_combine_masks_*` in `masks/group.c` is the arithmetic behind every operator
name the panel shows. Until this suite it was only ever checked end-to-end, by
rendering an image and comparing pixels — which proves the pipeline agrees with
itself on the fixtures it has, but does not pin what an operator *means*, and
cannot state the properties the rest of the design leans on:

- **the order-free group operators are commutative and associative.** This is
  what lets the panel reorder members freely within such a group. Difference
  is asserted *not* to be: a difference group takes its first member as the
  base, so order is the user's choice.
- **each operator's identity element.** An empty group contributes nothing,
  which the compositor implements by skipping it. These tests pin the
  arithmetic that makes skipping the right choice — in particular that
  intersection's identity is an all-*one* mask, so an empty intersection group
  could never be allowed to composite as all-zero and blank the whole mask.
- **every operator keeps the mask in [0,1]**, across opacity and invert, for
  every input in range.

## test_probe_image — is the probe adequate?

The migration verifier replays harvested masks against a synthetic image
generated by `develop/masks/probe_image.c`. That image is the single point
where the verifier's claim can quietly become vacuous: a parametric mask that
selects a range the probe never produces renders all-zero, and all-zero
compares equal to all-zero no matter how badly the migration mangled it.

So the probe's adequacy is measured rather than assumed, and the bar comes from
the colour space rather than from any library of real edits — profiling one
user's masks would produce a bar shaped by that user's habits, and ranges
nobody in the sample happened to use would then go unverified for everyone.
The suite sweeps the linear-RGB cube to discover what each blendif channel can
physically take, and holds the probe to that.

- **global coverage**: every channel occupied across everything the cube can
  reach, with no interior gap.
- **local coverage**: the same under windows the size of a plausible drawn
  mask, at many positions, taken over the diffuse range ([0,1] linear) rather
  than the full scene range — a patch a sixty-fourth of the image across
  spanning four stops uniformly is not something a photograph does either.
- **hard edges at every orientation**, because guided-filter feathering is
  edge-aware and degenerates towards a plain blur on smooth input.
- **texture at every scale**, because the detail mask is a wavelet
  decomposition and responds per band.
- **determinism** and **scene-referred range** (values above 1.0 exist, none
  below 0).

Each of these has been shown to fail on a deliberately defective probe. That
mattered more here than elsewhere: the edge and texture tests were both
*rewritten twice* because the first two versions passed on a probe with every
last bit of noise stripped out of it.

- Total per-octave energy failed because the tile lattice is periodic, and a
  square lattice has harmonics in every band.
- Energy restricted to edge-free quads failed because the selection is
  circular: the noise is what makes a quad non-flat, so filtering to flat
  quads filters out the signal. That version agreed with the noiseless probe
  to five decimal places.
- The version that works takes the diagonal (HH) wavelet coefficient over
  every quad with no selection, and reads its median. HH is identically zero
  for any linear function, so the tile's ramp contributes nothing; hard steps
  are sparse and cannot move a median. The median collapses to exactly zero
  with the noise switched off.

The same exercise found two real generator bugs: the "irregular" edge cells
were sized `tile * (2 << level)`, so every one of them landed on a tile
boundary and added no edge the lattice did not already have; and the
orientation bar was first written as a share of the edge population, which
rises as the axis-aligned lattice grows and so penalised the probe for having
more structure.

## Remaining gaps

The six gaps this suite started with are closed. What is left needs machinery
the fixture does not have:

- **Undo/redo interaction.** Needs a real history stack, so it belongs in an
  integration test (`--undo-masks`) rather than here.
- **Anything requiring the database.** The read-time stride selection in
  `dt_masks_read_forms_ext` (which picks how many bytes of each stored point to
  read, per masks version) is SQLite-coupled; only the migration chain that runs
  *after* it is covered. The deferred migration path
  (`dev->pending_flexi_migrations`, taken when `history_num >= 0`) is likewise
  out of reach -- the tests drive the inline path with `history_num = -1`.

## Not covered — manual checklist

These are properties of GTK rather than of the panel's logic, and need a real
widget tree and real event delivery:

- **event propagation between nested widgets.** A handler returning `FALSE` on
  a child bubbles to an ancestor carrying the same handler, firing it twice,
  so a group-header click would toggle back on release; the
  `_event_on_own_window` filter prevents it. Check: click a group header, an
  element row, an element's editor body, and empty space — each selects or
  deselects exactly once.
- **CSS rendering.** `.dt_masks_group_block` borders and selected-group shading,
  and the single drop-indicator line between two groups.
- **widget packing**, tooltips, drag icons, panel relocation between
  embedded / utility / left / right positions.
- **in-place row refresh after an inversion.** Inverting a whole group refreshes
  every row from one place (`_update_shape_row_state`), and a parametric row
  shows its polarity in its sliders' markers rather than on the handle icon.
  Check: with a group holding a drawn shape and a parametric element, "invert
  all elements" must flip the shape's handle icon *and* the parametric row's
  slider markers, exactly as inverting either one on its own does.
- **the expander option** (hamburger → options). Only the rules behind it are
  covered here (`dt_masks_model_row_is_expandable`, the anchor pair, and the conf
  default); which widget ends up where needs a real tree. Check, with a group
  holding a drawn shape, a parametric element and a raster mask:
  - *auto-expand selected* on — clicking each of the three in turn expands it
    and collapses the previously expanded one. Expanding a parametric element
    this way must not add a history item: the undo stack must not grow while
    merely clicking through the list. With two or more groups, clicking a group
    header opens that group's members and closes the other's, and selecting an
    element opens both its group and itself. A group's own chevron still wins
    over the option: collapsing the open group by its chevron must leave it
    collapsed, even though that same click also selects the group
    (`masks_group_collapse_click`).
  - opacity is never in a header: each expanded panel leads with a full slider
    — shapes, parametric elements, raster masks and groups alike.
- **header icons.** Every header keeps the same columns, counted from the
  right: expander, visibility eye, then a group's notes toggle, a parametric
  row's picker or the link of a linked shape or raster mask, then the
  low-opacity badge. The first three share one framed drawer, always three
  icons wide; a column a row does not use stays blank inside it, so the icons
  of every row line up. Check: click the eye (disable, and back), shift+click it (solo, and
  back), on an element and on a group; the eye, its tooltip and the actions
  menu's "disable"/"solo" entries must agree, and shift+click must do nothing
  on a disabled element or an empty group, where the menu offers no solo.

- **hover highlight across an interaction.** Hovering a row highlights its shape
  on the canvas; the highlight must survive the interaction, not just the
  pointer being inside the row. Check: drag a row's opacity slider (or a
  parametric slider) well outside the panel, and open a bauhaus popup from the
  row -- the shape stays highlighted throughout, and stops being highlighted
  once the pointer settles somewhere else. The highlight must also *look* like a
  canvas hover, bold outline included, not just feather and anchors: check a
  circle, an ellipse, a path, a brush stroke and a gradient side by side against
  hovering each one on the canvas.


## gen_raster_matrix.py — raster masks, enumerated

The cmocka suites are structural: they assert that migration produced
well-formed output, never that it renders the same pixels. Raster masks are
verified by rendering instead, through `darktable --verify-masks` (see
`src/develop/masks/verify.h`).

Classic raster mode is exclusive — it cannot be combined with a drawn or
parametric mask — so a raster edit is fully described by its source, an invert
flag, the opacity, and the global refinements applied downstream. That space is
small enough to enumerate, so this script enumerates it:

    ./gen_raster_matrix.py raster_matrix.json
    darktable --verify-masks raster_matrix.json

288 edits (invert x opacity x feathering x blur x tone curve x signed details x
colour space), all of which render a mask that genuinely varies. Contains no
user data and needs no library.

It exists alongside the real-library harvest rather than replacing it. The
harvest proves the migration handles edits people actually made; the matrix
covers the corners a personal corpus does not reach. Mutating the blend so
raster masks skip post-processing (`!uniform` -> `!uniform && !raster`) is
caught on 264 of the 288 generated edits and only 22 of 118 real ones.

## Adding a test

1. If it is a gesture, extract its handler into a model function first
   (see **The seam**) and declare it in `blend_gui_internal.h`. Keep history,
   pipe and widget work in the handler — the model half must not commit.
2. Write the scenario as a layout string.
3. **Prove the test can fail** — break the code deliberately and watch it go
   red before committing it. A test that has never failed has not been shown to
   test anything. Two mutations worth knowing:
   - in `dt_masks_model_drop_point_onto_group`, inserting after the group's
     marker instead of after its last member sends an element dropped on a
     group header to the bottom of that group instead of the top, and fails
     the header-drop tests;
   - removing the solo-edit clear in `dt_masks_model_toggle_solo_form` breaks the
     solo / solo-edit mutual exclusivity.
4. Check the invariant after **every** step of a sequence, not just at the end.
   Checked only at the end, a violation hides whenever the last step happens
   to be the one that cleans up; the isolation-mode test checks after each
   toggle, across all six orderings.
