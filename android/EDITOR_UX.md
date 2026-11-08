<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Fixed editing workspace

The preview and editing controls share the available screen equally above the
bottom navigation. The preview stays visible while parameters scroll. There is
no app-title bar, drawer, tools sheet, drag handle or swipe-to-dismiss gesture.
While the keyboard is open, the preview temporarily uses one quarter of the
remaining content height so the focused parameter still has room. Dismissing
the keyboard returns the equal split.

The default visual theme follows desktop `darktable-elegant-grey`: neutral
grey panels, a `#777777` photo surround, grey/white controls and small 3dp
corners. Selection uses grey/white emphasis in the existing indicators. Colour
roles are explicit and tonal surface tint is disabled, so the photo is not
surrounded by Material's default purple. Error indications retain their semantic
colour, and applied/draft curves keep different brightness levels.

This changes drawing only. Typography, line heights, density, padding, element
sizes, touch targets, pane proportions and navigation callbacks remain the same.
Settings → App theme offers Desktop gray, Light and OLED black. A choice applies
immediately and is saved for subsequent launches. Light uses white panels with
dark text; OLED uses true `#000000` for the panel, toolbar, photo surround and
system-bar backgrounds, with lighter controls and raised menus for separation.
Changing the theme preserves the open image, history, selected tools, scroll
positions and parameter drafts without recreating the Activity. The preview
bitmap itself is unchanged. The menu is available without an image or engine.
Android system-bar icon contrast and Activity window colours follow the selected
theme; window insets and edge-to-edge behaviour are unchanged. The OS starting
screen uses the manifest's default grey before the saved theme is loaded.

```text
┌──────────────────────────────────┐
│          Photo preview           │
│                                  │
│       Open  Undo  Redo  Export    │
├──────────────────────────────────┤
│ Selected tool ▾    Power  Search  │
│                                  │
│       Scrollable parameters      │
│                                  │
├──────────────────────────────────┤
│Light/Color Detail Geometry Settings│
└──────────────────────────────────┘
```

| Bottom tab | Tools and content |
| --- | --- |
| Light / Color | Exposure, white balance, tone curves, color grading and color transforms |
| Detail / Repair | Noise reduction, sharpening, optical detail correction, masks and repair tools |
| Geometry / Effects | Crop, perspective, lens geometry, framing, overlays, grain and finishing effects |
| Settings / Licenses | Language and theme menus, runtime availability, project information and license notices; available without an image |

Each editing tab presents a compact tool selector and a separate power switch
above the parameter scroller. Only the selected tool's controls are shown, so
users do not scroll through dozens of expanded cards. Native instances keep
their complete identities when selected.

Search replaces the parameter area with a scrollable result list across all
editing tabs. It matches tool names, native identities and parameter labels;
the Enabled filter narrows results without changing the selected tool. Each
result shows its category, native identity and On/Off state. Typing does not
select a result or create an edit. Tap a result to switch to its category and
exact instance, close search and show all of that tool's controls. Disabled
tools stay off until their power switch is explicitly used. Close or Back
cancels search and restores the current tool without changing it.
Choosing a bottom editing tab also closes search and opens that tab's remembered
tool. Results from the previous search do not cover the destination controls.

Selecting a tab or tool changes the visible controls without creating a native
edit or enabling a tool. The power switch explicitly enables/disables the
selected tool. Disabled tools remain inspectable; their parameter controls stay
disabled until enabled. Mandatory native modules keep their original power
restrictions. Switching a tool off retains its settings for later use.

Each category remembers its selected tool for the current image session. Each
tool's parameter scroll position is retained while moving between tabs. History
restoration resolves selections against the live native module list so a retired
instance is never edited through a stale selector. A newly opened image starts
with its own selection state.
The visible target is reconciled with the active selection after delayed state
updates and after native work finishes, so image operations follow the current
editing tab as well as the parameter panel.
An editing-tab click resolves and forwards that tab's remembered native tool ID
in the same event. The workspace owns these selections; its selector, parameter
panel and photo helpers share the resolved tool. Each tool has a separate
composition identity within its saved-state scope, so switching tools rebuilds
their control trees while retaining saved drafts and scroll positions.

Open, Undo, Redo and Export remain in the preview action row. Opening/exporting
uses Android's document picker. Native work retains the last preview where one
is available, shows its progress and Cancel, and disables edit actions until the
operation completes. Settings and license information remain reachable.

Coordinate diagrams, precise parameter controls and native processing behavior
are unchanged by this layout. The diagrams remain separate from the processed
photo; this workspace change does not add photo-aligned mask/crop gestures.
