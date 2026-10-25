# Modifier clicks in the masks panel

What a click with a modifier held does on each part of the masks panel, and
the rules a new control follows to stay consistent with the rest of darktable.

## The vocabulary

| modifier | meaning |
|---|---|
| ctrl | rename (as on a module header) |
| shift | pick with others on a row or header; the variant of the part's action on a button |
| ctrl+shift | delete (darktable uses it for "clear" elsewhere, e.g. the AI object tool) |
| alt | not used in the panel (window managers grab it, and no other panel uses it) |

## Rows and headers

The handle, the name and the background of a row, and a group's
header and block.

| click | does |
|---|---|
| plain | select; again to deselect |
| ctrl | rename |
| shift | pick it with others (see below) |
| ctrl+shift | delete the element, or the group and every member in it; on the mask's own group, reset the mask |

Delete acts on the press, with the selection untouched: on macOS a click
without movement ends as a drag that did not move, which delivers no release
(`_masks_drag_end`). Holding ctrl+shift over the list shows a trash bin
pointer and shades the row (or group header) under it, so what goes is seen
first (`_armed_watch`).

Pick acts on the press for the same reason. Holding shift over the list, once
a member is selected or picked, shows the plus pointer (a minus over what is
selected already) and shades the row (or group header) under it
(`_armed_watch`); a click adds it to the picks, or takes it off. The first pick
joins the selected member, so a selection and one shift+click make two. With
the mask's own group selected, which is no member, nothing is armed and
shift+click is a plain click. From two picks on, the selection panel gives way
to the multiple selection panel, which deletes, moves (into a group chosen from
a menu) or groups the picks together (they must all be in the same group; when
they come down to one thing, the button reads "wrap in group"). A plain click
on a row selects only that row. The mask's own group cannot be picked.

A group is selected whole: selecting it selects everything it holds, at any
depth, and taking off one of those takes the group off too, as it is no longer
whole, leaving the others. This holds for the group the panel has selected, too:
a shift+click on one of its members takes that off. The actions then act on the
group, which takes what it holds along.

While several are picked, the eye, invert and operator buttons of a picked row
or header act on every pick, which all take the state the click gives that one,
but for what a picked group holds, which the group's state covers; a button of
such a row acts on that row alone. The chevron shows or hides what a row or
group holds.

Deleting, moving or grouping the picks consumes them; undoing that brings the
mask back with the same items picked (`_picks_undo_t`: the panel remembers them
by the hash of the mask before and after, not in the history).

## Buttons

| part | plain | shift | other modifiers |
|---|---|---|---|
| eye, member | disable | solo (not on the whole mask) | nothing |
| invert, element | invert | nothing | nothing |
| invert, group | invert its output | invert each of its members | nothing |
| group operator icon | operator menu | nothing | ctrl+shift deletes the group |
| chevron | show or hide | nothing | nothing |
| channel button | add the channel | toggle its persistent overlay | nothing |
| selection panel: delete | delete | empty the group, keeping it | nothing |
| selection panel: wand | open the menu: wrap in group, compose, simplify (groups only) | nothing | nothing |
| selection panel: link | go to the other end | give this module its own copy | nothing |

The parametric pickers list their own modifiers in their tooltips.

## The rule for a new control

A modifier+click does nothing unless the control gives that modifier a
meaning. GTK would otherwise treat it as a plain click.

- a button: `_button_allow_modifiers(button, allowed)` right after creating it
  (`0` for none)
- a click handler of its own: `_swallow_unregistered_modifier(gesture, allowed)`
  before acting, on the press
- a release handler, which must not claim: `_modifier_unregistered()`
- rows and headers: `MASKS_CLICK_MODS`

List the modifiers the control knows in its tooltip.
