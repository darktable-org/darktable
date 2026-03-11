# Module Groups

Module groups organize IOP modules into logical categories (Technical, Grading, Effects, etc.) in the darkroom view. This helps users find tools relevant to their current task.

## The Concept

Darktable uses a tabs-based interface in the right panel. Each tab corresponds to a **Module Group**.
-   **Favorites**: User-selected modules.
-   **Technical**: Basic corrections (demosaic, lens, crop).
-   **Grading**: Color and tone grading.
-   **Effects**: Artistic effects.
-   **Quick Access Panel**: (Special case, detailed in `Quick_Access_Panel.md`).

## Implementation (`modulegroups.c`)

The referencing of modules into groups is handled by `src/libs/modulegroups.c`.

### Defining the Group for a Module
Each IOP module declares its default group(s) via the `default_group()` callback function in its source file.

```c
// in src/iop/mymodule.c
int default_group()
{
  return IOP_GROUP_TONES | IOP_GROUP_COLOR;
}
```

Predefined groups (in `iop_api.h`):
-   `IOP_GROUP_TECHNICAL`
-   `IOP_GROUP_GRADING`
-   `IOP_GROUP_EFFECTS`
-   `IOP_GROUP_NO_GROUP` (hidden by default)

### Custom Groups (Presets)
Users can override these defaults by creating **Module Group Presets**.
These are stored as presets for the `modulegroups` module.
The internal logic (`modulegroups.c`) checks the active preset to decide which modules appear in which tab.

## Adding a New Module to Groups

When creating a new module:
1.  Implement `default_group()` in your `.c` file.
2.  Choose the most appropriate category. Don't add to too many groups to avoid clutter.
3.  The module will automatically appear in that tab when the "default" module group preset is active.
