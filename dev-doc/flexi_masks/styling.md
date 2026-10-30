# Styling the masks panel

How to restyle the masks panel from a theme or from your CSS tweaks
(preferences > general > "modify selected theme with CSS tweaks below"),
without reading the panel's code.

Every rule for the panel is in one section at the end of
`data/themes/darktable.css` ("Masks panel"). Every class it uses starts with
`.dt_masks_`, every id with `#masks-`, so a search for `dt_masks_` or
`masks-` finds them all.

## The rule of thumb

Start every rule with `#masks-panel`, the panel's body. Inside the list, the
theme styles the panel with classes only, so a rule with that id in front
always wins, whatever state the part is in and wherever your rule sits in the
file:

```css
#masks-panel .dt_masks_group_header .dt_masks_invert.dt_masks_inverted
{
  color: #e8e8e8;
}
```

The selection panel under the list is the exception: its theme rules carry
its own id, `#masks-selection-area`, which they need to outrank darktable's
stock section rules. Start a rule for it with
`#masks-panel #masks-selection-area`. The multiple selection panel that stands
in for it (see below) is `#masks-multi-area`, and follows the same rule.

## Rows and headers

Every row is a `.dt_masks_row`: its header line and whatever it
opens onto. Every header line is a `.dt_masks_header`, plus its kind:

| class | header |
|---|---|
| `.dt_masks_row_header` | a member shown as a row: an element (a shape, a parametric channel, a raster mask, an AI object), or a nested group that carries settings of its own |
| `.dt_masks_group_header` | a group's own; the mask's own group, at the top of the list, adds `.dt_masks_root` |

A header's state is a class on the header itself:

| state | when |
|---|---|
| `.dt_masks_selected` | it is the selection |
| `.dt_masks_selected_element_group` | it holds the selected element: only that group, not the ones above it |
| `.dt_masks_hovered` | under the pointer, in the list or on the canvas: a 3px bar along its right edge in `mask_selected_frame` (an inset shadow, so that a selected header's frame, drawn over its edge, does not hide it), the header keeps its shade |
| `.dt_masks_picked` | picked with others by shift+click, see the multiple selection below. The theme styles it as `.dt_masks_selected`. A pick that a picked group holds shows with the group, as `.dt_masks_selected_held`, so a group picked after its members looks as a selected group does |
| `.dt_masks_soloed` | a soloed group's header; a soloed row carries it instead |
| `.dt_masks_open` | its members show |
| `.dt_masks_drop_target` | a group a drag would drop into |
| `.dt_masks_pending` | on a row: a shape still being drawn on the canvas, not yet added |
| `.dt_masks_linked` | on an element's header: a shape linked with other modules or shown more than once in this mask. The theme gives it a dashed frame, since its link button is in its drawer |

The theme's state rules all have the same specificity, so their order in the
file decides between two states on one header: group of the selected element,
selected, soloed, drop target. The shades of the modifiers held over the list
(`.dt_masks_deleting` for ctrl+shift, on the list) applies to the hovered
header, and outranks those. `.dt_masks_picking`, for shift, is on the list too,
and the default theme leaves it unstyled. A tweak naming the
state wins over all of them:

```css
#masks-panel .dt_masks_header.dt_masks_selected
{
  background-image: none;
  background-color: #4a6a8a;
}
```

The default theme draws no rounded corners and leaves `.dt_masks_open`
unstyled: it is there for a theme that rounds headers, to square off the edge
where a header meets its rail.

`.dt_masks_selected_held` is on every row and header held by a selected or
picked group. The default theme leaves it unstyled (a held header is not
framed, and keeps its own shade); it is there for a theme that wants to mark
what the selection holds.

A header's `outline-color` is never drawn as an outline: it is the color of
the rail hanging from it, down the left edge of a group's members. The theme
sets it once, on `.dt_masks_header`, to `mask_group_rest`, so every rail has
the shade of a group's header at rest, whatever state the header is in.

The name is `.dt_masks_row_name`, on the label of an element and on the box
around the label of a group: `.dt_masks_row_name, .dt_masks_row_name label`
covers both.

## Groups and notes

| class | part |
|---|---|
| `.dt_masks_group_block` | a group: its header and its members |
| `.dt_masks_frame` | the frame of a header that is selected or picked; while shift is held over the list, or with picks made, a group's frame takes in what the group holds too. The panel draws it over the whole list, after everything in it, using this class's border for its width, color and corners, its margin for how far it is inset from the header, so that two adjacent frames do not touch, and its box-shadow for a glow (none by default); its background must stay none, as it would be painted over the list |
| `.dt_masks_group_members` | a group's members, with the rail on its left edge |
| `.dt_masks_note` | a preset group's note, in the selection panel's help section (`.dt_masks_help_panel`), above the controls |

## Header icons

Each part has one class, on the widget that paints it.

| class | part |
|---|---|
| `.dt_masks_lead` | the lead icon, at the left of an element's header. A group has none: its operator is in the drawer |
| `.dt_masks_drawer` | the box holding the icons on the right: the link or, on a group, its operator, then invert and the eye |
| `.dt_masks_icon` | every icon in the drawer, a group's expander, and a parametric channel's pickers |
| `.dt_masks_expander` | first on a group, at the left of its header, outside the drawer: shows or hides the group's members. Only groups expand |
| `.dt_masks_link` | first in the drawer of a raster mask or a linked shape: goes to the module the mask comes from, or to the next module using the shape (shift+click: gives this module its own copy) |
| `.dt_masks_operator` | first in a group's drawer, where the link is on an element: the group's operator. Click to change it |
| `.dt_masks_eye` | rightmost: click to disable, shift+click to solo |
| `.dt_masks_options` | the toggle at the left of the controls header while a parametric element is selected: shows the output slider (the input one is always shown) and, for a channel that has one, the boost factor. A plain icon button, plated on hover like the drawer's, and as an on button when on |
| `.dt_masks_slider_icon` | the icon before each of a parametric element's sliders, input or output |
| `.dt_masks_picker` | a parametric channel's picker, at the right end of each of its sliders: click sets the slider's range from an area, ctrl+click picks a GUI color (point), ctrl+shift+click the same (area) |
| `.dt_masks_invert` | second from the right on every header: inverts the element, or the group's output (shift+click: each of its members instead) |
| `.dt_masks_badge` | at the end of the name, hidden while there is nothing to warn of so that it takes room only when it shows: a member that does nothing or next to nothing (opacity under 10%), or a group holding one at any depth; its tooltip says which. It is not in the drawer |
| `.dt_masks_channel_picker` | the same button's place: over the right end of its slider, under the header's rightmost icon, not in the drawer |

Each icon's state is a class or a GTK state on the icon itself, so a state
rule is the part's class plus one more.

| part | state | when |
|---|---|---|
| `.dt_masks_lead` | `.dt_masks_channel` | a parametric row: the lead is the channel's code, not a glyph |
| `.dt_masks_eye` | `.dt_masks_soloed` | the member is soloed |
| `.dt_masks_eye` | `.dt_masks_disabled` | the member is disabled |
| `.dt_masks_invert` | `.dt_masks_inverted` | the element or the group's output is inverted |
| `.dt_masks_expander` | `:checked` | open (collapsed, the theme gives it the plate of an on button) |
| `.dt_masks_expander` | `:disabled` | an empty group's, with nothing to open |
| `.dt_masks_badge` | `.dt_masks_no_effect` | its first reason is that something does nothing at all (a channel still covering its whole range, a raster mask with nothing to read), rather than little |
| any button | `:hover` | under the pointer |

Whose icon it is comes from the header it sits in:

```css
/* every inverted invert button */
#masks-panel .dt_masks_invert.dt_masks_inverted { ... }
/* only a group's */
#masks-panel .dt_masks_group_header .dt_masks_invert.dt_masks_inverted { ... }
/* only an element's */
#masks-panel .dt_masks_row_header .dt_masks_invert.dt_masks_inverted { ... }
```

### Sizes

Every icon is an icon button sized by the theme like any other
(`.dt_module_btn`), with the icon inset of a module's sub-panel headers, so
the glyphs match theirs, but only as wide as its glyph, so the icons sit evenly
spaced. Invert and the eye are columns every row has; one the row has no icon
for (the pending row's) is a blank button of the same size
(`.dt_masks_blank`), so they stay lined up down the list. The expander and the
third icon are not columns: a row without one does not reserve its room, and its
name takes it. Padding on
`.dt_masks_drawer` adds room around all its icons, and grows the drawer by as
much: the theme's right padding is the side inset the sub-panels' expanders
keep, so the rows' rightmost icons end where theirs do.

### Backgrounds

Leads and the drawer have no plate: their icons sit on the header. To give
every lead a plate:

```css
#masks-panel .dt_masks_lead
{
  background-color: #3a3a3a;
}
```

A frame around an icon is a `box-shadow`, such as
`box-shadow: inset 0 0 0 1px red;`. An element's lead is drawn in code and
does not paint a CSS `border`.

## States are yours too

A rule outranks the theme in every state, so it also covers the states you
did not mention: a rule recoloring every eye recolors a soloed or disabled
one as well, which then looks like the rest. Give each state you want to keep
different its own rule; having one more class, it wins over your plain one:

```css
#masks-panel .dt_masks_eye
{
  color: #909090;
}

#masks-panel .dt_masks_eye.dt_masks_soloed
{
  color: #e8e8e8;
}
```

## Colors alone

Every color of the panel derives from the theme's own: grounds and shades
from `@plugin_bg_color`, the selection panel's sections from
`@collapsible_bg_color` as every other collapsible section, text from
`@fg_color` and the slider colors, the badge from the color label hues. A
theme that sets those retints the panel with them.

To change colors only, and keep every state's look, redefine the panel's color
tokens instead. Your CSS tweaks are loaded after the theme, into the same
stylesheet, so a redefinition replaces the token everywhere it is used.

| token | used for |
|---|---|
| `mask_rest` | a member's header at rest |
| `mask_group_rest` | a group's header at rest, and every rail |
| `mask_selected_element_group` | the header of the group holding the selected element |
| `mask_selected_top`, `mask_selected` | a selected header, top and bottom of its gradient (flat by default) |
| `mask_selected_frame` | the frame of a selected or picked header, and the bar of a hovered one |
| `mask_hover_top`, `mask_hover` | the group a drop would land in, top and bottom of its gradient, and the base of the delete shade |
| `mask_list_bg` | the ground of the whole list |
| `mask_lead_fg` | the glyph of a lead icon |
| `mask_badge`, `mask_badge_no_effect` | the warning badge (amber), and the same with `.dt_masks_no_effect` (red) |
| `mask_badge_outline` | the warning badge's edge, as its `outline-color` |
| `mask_text_rest`, `mask_text_member`, `mask_text_selected_element_group`, `mask_text_selected`, `mask_text_held`, `mask_text_hover` | header names, per header state |
| `mask_text_control`, `mask_text_control_hover` | labels and values of the sliders in the list |

```css
@define-color mask_lead_fg #f0d070;
@define-color mask_list_bg #202020;
```

## The selection panel

Under the list, `#masks-selection-area` holds a caption naming the selection
(`.dt_masks_selection_caption`, the box around its name `.dt_masks_peek` while
it names an element hovered on the canvas rather than the selection), with
the buttons acting on it (`.dt_masks_selection_action`): delete and compose on
its left, the link to the element's other end and the group layout presets on
its right. Then the toolbar of add buttons (`.dt_masks_selection_row`,
`.dt_masks_toolbar_add`): add group and import on its left, the shapes and
channels beside them. Then its help (`#masks-help-section`, shown only for a
group with a preset's note), controls and refinement, each a header
(`.dt_masks_selection_section`, plus
`.dt_masks_open` while open) over a card (`.dt_masks_selection_card`), styled as
any module's expander is, and sitting on the panel's own ground. It has tokens
of its own:

| token | used for |
|---|---|
| `mask_selection_text` | the section titles |
| `mask_selection_text_control`, `mask_selection_text_control_hover` | labels and values of the sliders |

## The multiple selection

Shift+click on rows and headers picks them together. From two on,
`#masks-multi-area` takes the selection panel's place: the same caption
(`.dt_masks_selection_caption`), counting what is picked, and under it
`.dt_masks_multi_actions`: a `button.dt_masks_multi_action` each for delete, move
and group together. Picked headers carry
`.dt_masks_picked`, which the theme styles as a selected header. A selected or picked header
is framed in `mask_selected_frame`, the accent color when one is set and the
color of its text otherwise, but for what is selected with its group.

## The panel's homes

| name | what |
|---|---|
| `#blending-tabs` | the "blend mask" header; `.dt_masks_embedded` inside the module, `.dt_masks_enabled` while the mask is on |
| `.dt_masks_header_controls` | the header's icons (overlay, eye, edit, solo, lock), right-aligned; docked and in the utility panel, inset to end over the columns of the icons below |
| `#masks-focus-row` | the row under the header naming the focused module, in the utility panel and on the canvas: `.dt_masks_focus_icon`, `.dt_masks_focus_module`, `.dt_masks_focus_instance` |
| `#masks-header-caption`, `#masks-collapse` | its caption and its collapse arrow |
| `.dt_masks_enable_toggle` | the mask's on/off toggle |
| `#masks-docked-panel` | the docked panel, `.dt_masks_left` or `.dt_masks_right` by its dock, `.dt_masks_floating` over the canvas, `.dt_masks_handle_hidden` with its handle narrowed |
| `#masks-panel-handle` | the docked panel's resize grip, `.dt_masks_handle_hidden` when narrowed |
| `#masks-sliver`, `#masks-halo` | the docked panel's way in from the canvas edge; the halo is `.dt_masks_left` or `.dt_masks_right` |
| `.dt_masks_panel_toggle` | the darkroom toolbar's mask panel button, `.dt_masks_empty` when the module has no mask |
| `.dt_masks_host_header`, `.dt_masks_host_controls`, `#masks-host-content` | the panel hosted in the utility panel |
