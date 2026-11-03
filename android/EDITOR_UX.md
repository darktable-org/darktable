<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Fixed editing workspace

The preview and editing controls share the available screen equally above the
bottom navigation. The preview stays visible while parameters scroll. There is
no app-title bar, drawer, tools sheet, drag handle or swipe-to-dismiss gesture.
While the keyboard is open, the preview temporarily uses one quarter of the
remaining content height so the focused parameter still has room. Dismissing
the keyboard returns the equal split.

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
| Settings / Licenses | Runtime availability, project information and license notices; available without an image |

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

Open, Undo, Redo and Export remain in the preview action row. Opening/exporting
uses Android's document picker. Native work retains the last preview where one
is available, shows its progress and Cancel, and disables edit actions until the
operation completes. Settings and license information remain reachable.

Coordinate diagrams, precise parameter controls and native processing behavior
are unchanged by this layout. The diagrams remain separate from the processed
photo; this workspace change does not add photo-aligned mask/crop gestures.
