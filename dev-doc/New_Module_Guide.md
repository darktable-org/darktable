# Guide to Creating a New IOP Module

This guide walks you through the process of creating a new Image Operation (IOP) module in darktable.

## 1. Files to Create

Create a new C file in `src/iop/`. Use the template `useless.c` as a reference.
Example: `src/iop/mymodule.c`

For a full API reference, see [IOP_Module_API.md](IOP_Module_API.md).

## 2. Registering the Module

### `src/iop/CMakeLists.txt`
Add your module file to the list:
```cmake
add_iop(mymodule "mymodule.c")
```

### `src/common/iop_order.c`
Add your module to **all** relevant pipeline order lists (e.g., `legacy_order`, `v30_order`, `v50_order`, `v30_jpg_order`, `v50_jpg_order`).
You must decide where your processing should happen (before or after which other operations).
```c
// Example: placing it after exposure
{ { 12.0f }, "exposure", 0 },
{ { 13.0f }, "mymodule", 0 },
```

You must also inject your module into `dt_ioppr_get_iop_order_list()`, which runs dynamically when upgrading or falling back. Find the `_insert_before` section and add your module:
```c
_insert_before(iop_order_list, "module_it_should_run_before", "mymodule");
```

**Troubleshooting "missing iop_order for module" Fatal Error:**
If darktable crashes on startup with `[dt_init] ERROR: iop order looks bad, aborting.` and logs `missing iop_order for module mymodule`, it means your module was registered in `CMakeLists.txt` but the iop string name does not perfectly match its placement in `iop_order.c`. Make sure:
1. You have correctly defined the string identifier (e.g., `"mymodule"`) in every `iop_order` table array.
2. You added an `_insert_before()` rule in `dt_ioppr_get_iop_order_list()`.

## 3. Implementing the Module (`mymodule.c`)

### Structures
1.  **Parameters**: `dt_iop_mymodule_params_t`
    -   Use `DT_MODULE_INTROSPECTION` macro. See [introspection.md](introspection.md) for metadata tags and versioning.
    -   Define fields with default values and ranges in comments.
2.  **GUI Data**: `dt_iop_mymodule_gui_data_t` (if needing GUI).

### Required Functions
-   `name()`: unique internal name.
-   `default_colorspace()`: usually `IOP_CS_RGB` or `IOP_CS_LAB`.
-   `process()`: The core logic.

See [IOP_Module_API.md — Required Functions](IOP_Module_API.md#required-functions) for details.

### Optional Functions
-   `description()`: Tooltip description.
-   `flags()`: `IOP_FLAGS_SUPPORTS_BLENDING`, etc.
-   `default_group()`: Which tab it belongs to (Technical, Grading, etc.). See [Module_Groups.md](Module_Groups.md).
-   `gui_init()`: Create widgets.
-   `gui_update()`: Sync widgets from params.
-   `commit_params()`: If you need to precalculate data for processing.

See [IOP_Module_API.md — Optional Functions](IOP_Module_API.md#optional-functions) for the full list.

## 4. GUI Implementation
In `gui_init()`:
-   Use `dt_bauhaus_slider_from_params(self, "param_name")` to create sliders linked to your params.
-   These will automatically handle history updates, undo/redo, and shortcuts.

For the full GUI architecture (layout API, event flow, thread safety, widget reparenting), see [GUI.md](GUI.md).

## 5. Integration

### Pipeline Order
Ensure your module is in the correct place in `iop_order.c`.
-   **Scene-referred** modules should generally be earlier.
-   **Display-referred** modules should be later.

**Note:** Pipeline ordering affects `commit_params()` execution order. Modules that share state (e.g., via `dev->chroma`) depend on earlier modules having committed first. See [pixelpipe_architecture.md](pixelpipe_architecture.md#pipeline-ordering-asymmetry) for details.

### Shortcuts
By using `dt_bauhaus` widgets, shortcuts are automatically supported.
-   Verify in **Preferences > Shortcuts**.
-   See [Shortcuts.md](Shortcuts.md) for manual registration and the action system.

### Module Groups
Implement `default_group()` to place your module in the right tab.
-   `IOP_GROUP_TECHNICAL`: Technical corrections.
-   `IOP_GROUP_GRADING`: Color/Tone grading.
-   `IOP_GROUP_EFFECTS`: Aesthetic effects.

### Quick Access Panel (QAP)
To allow your widgets to be used in the QAP:
-   Use standard `dt_bauhaus` widgets.
-   No extra code needed; users can add your widgets to their QAP config.
-   Ensure your widgets make sense in isolation (have clear labels/tooltips).
-   See [Quick_Access_Panel.md](Quick_Access_Panel.md) for developer considerations and restrictions.

## 6. Testing
1.  Compile: `./build.sh`
2.  Run: `./build/bin/darktable -d pipe`
3.  Check terminal for any introspection errors (e.g. invalid default values).
4.  Open an image and verify your module appears and processes correctly.
