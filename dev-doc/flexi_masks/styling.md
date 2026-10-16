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
#masks-panel .dt_masks_group_header .dt_masks_lead.dt_masks_inverted
{
  background-image: none;
  background-color: #e8e8e8;
}
```

The selection panel under the list is the exception: its theme rules carry
its own id, `#masks-selection-area`, which they need to outrank darktable's
stock section rules. Start a rule for it with
`#masks-panel #masks-selection-area`.

## Rows and headers

Every element row is a `.dt_masks_row`: its header line and whatever it
opens onto. Every header line is a `.dt_masks_header`, plus its kind:

| class | header |
|---|---|
| `.dt_masks_element_header` | an element's row: shapes, parametric channels, raster masks and nested groups alike |
| `.dt_masks_group_header` | a group's; the mask's own group, at the top of the list, adds `.dt_masks_root` |
| `.dt_masks_cluster_header` | a run of elements of one kind, folded into one line |

A header's state is a class on the header itself:

| state | when |
|---|---|
| `.dt_masks_selected` | it is the selection |
| `.dt_masks_implied` | it holds the selection |
| `.dt_masks_hovered` | under the pointer, in the list or on the canvas |
| `.dt_masks_soloed` | a soloed group's header; a soloed element's row carries it instead |
| `.dt_masks_open` | its controls or members show; a group's adds `.dt_masks_has_card` when its card hangs from it |
| `.dt_masks_drop_target` | a group a drag would drop into |
| `.dt_masks_pending` | on a row: a shape still being drawn on the canvas, not yet added |

The theme's state rules all have the same specificity, so their order in the
file decides between two states on one header: implied, selected, soloed,
hovered, drop target. A tweak naming the state wins over all of them:

```css
#masks-panel .dt_masks_header.dt_masks_selected
{
  background-image: none;
  background-color: #4a6a8a;
  outline-color: #4a6a8a;
}
```

The default theme draws no rounded corners and leaves `.dt_masks_open`,
`.dt_masks_has_card` and `.dt_masks_joined` unstyled: they are there for a
theme that rounds headers and cards, to square off the edges where a header
meets its card or rail.

A header's `outline-color` is never drawn as an outline: it is the color of
the rail hanging from it, down the left edge of a group's members. Set it
beside the background, so the two agree.

The name is `.dt_masks_row_name`, on the label of an element and on the box
around the label of a group: `.dt_masks_row_name, .dt_masks_row_name label`
covers both.

## Groups and cards

| class | part |
|---|---|
| `.dt_masks_group_block` | a group: its header and its members |
| `.dt_masks_group_elements` | a group's members, with the rail on its left edge |
| `.dt_masks_cluster_elements` | an open cluster's members |
| `.dt_masks_card` | the controls under an open header, on the card ground |
| `.dt_masks_param_card` | a parametric row's sliders |
| `.dt_masks_props_card` | a row's properties |
| `.dt_masks_group_card` | a group's opacity slider |
| `.dt_masks_note` | a preset group's note; `.dt_masks_joined` when the slider follows it on the same card |

## Header icons

Each part has one class, on the widget that paints it.

| class | part |
|---|---|
| `.dt_masks_lead` | the lead icon, at the left of a header |
| `.dt_masks_drawer` | the box holding the icons on the right |
| `.dt_masks_icon` | every icon in the drawer but the badge and the notes, and a parametric channel's eye |
| `.dt_masks_expander` | rightmost: shows or hides the row's controls or the group's elements |
| `.dt_masks_eye` | second from the right: click to disable, shift+click to solo |
| `.dt_masks_notes` | third from the right on a group made by a preset, half size under the badge: its notes |
| `.dt_masks_picker` | third from the right on a parametric row: its color picker |
| `.dt_masks_link` | third from the right on a linked shape or a raster mask: the chain to the other end |
| `.dt_masks_badge` | half size, at the top of the first free column from the right (over a group's notes, in the third column of an element with no picker or link, else the fourth): an element or group that does nothing or next to nothing (opacity under 10%), or a group holding one at any depth; its tooltip says which |
| `.dt_masks_channel_eye` | in a parametric row's controls: bypasses one channel range |

Each icon's state is a class or a GTK state on the icon itself, so a state
rule is the part's class plus one more.

| part | state | when |
|---|---|---|
| `.dt_masks_lead` | `.dt_masks_inverted` | the element's or group's output is inverted |
| `.dt_masks_lead` | `.dt_masks_channel` | a parametric row: the lead is the channel's code, not a glyph |
| `.dt_masks_eye` | `.dt_masks_soloed` | the element or group is soloed |
| `.dt_masks_eye` | `.dt_masks_disabled` | the element or group is disabled |
| `.dt_masks_expander` | `:checked` | open |
| `.dt_masks_expander` | `:disabled` | an empty group's, with nothing to open |
| `.dt_masks_notes` | `:checked` | the notes are shown |
| `.dt_masks_badge` | `.dt_masks_no_effect` | its first reason is that something does nothing at all (a channel still covering its whole range, a raster mask with nothing to read), rather than little |
| `.dt_masks_channel_eye` | `:checked` | the range is bypassed |
| any button | `:hover` | under the pointer |

Whose icon it is comes from the header it sits in:

```css
/* every inverted lead */
#masks-panel .dt_masks_lead.dt_masks_inverted { ... }
/* only a group's */
#masks-panel .dt_masks_group_header .dt_masks_lead.dt_masks_inverted { ... }
/* only an element's */
#masks-panel .dt_masks_element_header .dt_masks_lead.dt_masks_inverted { ... }
```

### Sizes

Every icon is an icon button sized by the theme like any other
(`.dt_module_btn`), with the icon inset of a module's sub-panel headers, so
the glyphs match theirs, but only as wide as its glyph, so the icons sit evenly
spaced. The badge and a group's notes are half that and share a column; a
column a row has no icon for is a blank button of the same size
(`.dt_masks_blank`), so the icons of every row stay lined up. Padding on
`.dt_masks_drawer` adds room around all its icons, and grows the drawer by as
much: the theme's right padding is the side inset the sub-panels' expanders
keep, so the rows' expanders end where theirs do.

### Backgrounds

Leads and the drawer have no plate: their icons sit on the header. Only an
inverted lead has one, in `mask_lead_inverted_bg`, with an edge drawn as an
inset `box-shadow`. To give every lead a plate:

```css
#masks-panel .dt_masks_lead
{
  background-color: #3a3a3a;
}
```

A frame around an icon is a `box-shadow` too, such as
`box-shadow: inset 0 0 0 1px red;`. An element's lead is drawn in code and
does not paint a CSS `border`.

## States are yours too

A rule outranks the theme in every state, so it also covers the states you
did not mention. The rule above recolors inverted leads as well, which then
look like the rest. Give each state you want to keep different its own rule;
having one more class, it wins over your plain one:

```css
#masks-panel .dt_masks_lead
{
  background-color: #3a3a3a;
}

#masks-panel .dt_masks_lead.dt_masks_inverted
{
  background-color: #e8e8e8;
  color: #202020;
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
| `mask_rest` | the headers at rest |
| `mask_implied` | the header of a group holding the selection |
| `mask_selected_top`, `mask_selected` | a selected header, top and bottom of its gradient |
| `mask_hover_top`, `mask_hover` | a hovered header, top and bottom of its gradient |
| `mask_card` | the ground of a row's open controls |
| `mask_list_bg` | the ground of the whole list |
| `mask_lead_fg` | the glyph of a lead icon |
| `mask_lead_inverted_bg`, `mask_lead_inverted_fg` | an inverted lead's plate, and its glyph and edge |
| `mask_badge`, `mask_badge_no_effect` | the warning badge (amber), and the same with `.dt_masks_no_effect` (red) |
| `mask_badge_outline` | the warning badge's edge, as its `outline-color` |
| `mask_text_rest`, `mask_text_implied`, `mask_text_selected`, `mask_text_hover` | header names, per header state |
| `mask_text_control`, `mask_text_control_hover` | labels and values of the sliders in the list |

```css
@define-color mask_lead_inverted_bg #f0d070;
@define-color mask_lead_inverted_fg #202020;
```

A rail takes its header's shade, the bottom one of a gradient.

## The selection panel

Under the list, `#masks-selection-area` holds the selection's icon and name
(`.dt_masks_selection_row`, `.dt_masks_selection_name`), then its properties
and refinement, each a header (`.dt_masks_selection_section`, plus
`.dt_masks_open` while open) over a card (`.dt_masks_selection_card`). It has
tokens of its own, which start out as the list's:

| token | used for |
|---|---|
| `mask_selection_bg` | the ground of the panel |
| `mask_selection_header` | the section headers |
| `mask_selection_card` | the ground of a section's controls |
| `mask_selection_text` | the selection's name and the section titles |
| `mask_selection_text_control`, `mask_selection_text_control_hover` | labels and values of the sliders |

## The panel's homes

| name | what |
|---|---|
| `#blending-tabs` | the "blend mask" header; `.dt_masks_embedded` inside the module, `.dt_masks_enabled` while the mask is on |
| `#masks-header-caption`, `#masks-collapse` | its caption and its collapse arrow |
| `.dt_masks_enable_toggle` | the mask's on/off toggle |
| `#masks-docked-panel` | the docked panel, `.dt_masks_left` or `.dt_masks_right` by its dock, `.dt_masks_floating` over the canvas, `.dt_masks_handle_hidden` with its handle narrowed |
| `#masks-panel-handle` | the docked panel's resize grip, `.dt_masks_handle_hidden` when narrowed |
| `#masks-sliver`, `#masks-halo` | the docked panel's way in from the canvas edge; the halo is `.dt_masks_left` or `.dt_masks_right` |
| `.dt_masks_panel_toggle` | the darkroom toolbar's mask panel button, `.dt_masks_empty` when the module has no mask |
| `.dt_masks_host_header`, `.dt_masks_host_controls`, `#masks-host-content` | the panel hosted in the utility panel |
