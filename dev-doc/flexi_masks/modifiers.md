# Modifier clicks in the masks panel

What a click with a modifier held does on each part of the masks panel, and
the rules a new control follows to stay consistent with the rest of darktable.

## The vocabulary

| modifier | meaning |
|---|---|
| ctrl | rename (as on a module header) |
| shift | show or hide what the part holds; or the variant of the part's action |
| ctrl+shift | delete (darktable uses it for "clear" elsewhere, e.g. the AI object tool) |
| alt | not used in the panel (window managers grab it, and no other panel uses it) |

## Rows and headers

The handle, the name and the background of a row, and a group's
header and block.

| click | does |
|---|---|
| plain | select; again to deselect |
| ctrl | rename |
| shift | show or hide the row's controls, or the group's members |
| ctrl+shift | delete the element, or the group and every member in it; on the mask's own group, reset the mask |

Delete acts on the press, with the selection untouched: on macOS a click
without movement ends as a drag that did not move, which delivers no release
(`_masks_drag_end`). Holding ctrl+shift over the list shows a trash bin
pointer and shades the row (or group header) under it, so what goes is seen
first (`_armed_watch`).

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
| selection panel: compose | open the menu | simplify the group | nothing |
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
