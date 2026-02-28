# Quick Access Panel (QAP)

The Quick Access Panel (QAP) is a specialized view in the darkroom that aggregates frequently used controls from different modules into a single, compact interface.

## Purpose

Instead of switching between tabs and scrolling to find specific sliders (e.g., "Exposure", "Contrast", "White Balance"), the user can have them all in one list. This mimics the "Basic" panel found in other raw processors.

## Mechanism

The QAP is implemented as part of the `modulegroups` library (`src/libs/modulegroups.c`).
It is technically a special "Basics" group (`DT_MODULEGROUP_BASICS`).

### How Modules Provide Widgets to QAP

A module doesn't explicitly push widgets to the QAP. Instead, the QAP pulls widgets from modules based on configuration.

1.  **Selection**: The QAP configuration lists modules and specific widgets (by name/ID) to include.
2.  **Reparenting**: When the QAP is active, `modulegroups.c` *steals* the widget from the original module's GUI and places it into the QAP box.
3.  **Restoration**: When leaving the QAP or the module, the widget is put back into its original container.

## Developer Considerations

To ensure your module works well with the QAP:

1.  **Use Standard Widgets**: Use `dt_bauhaus_*` widgets. Custom widgets are harder to integrate.
2.  **Naming**: Ensure your widgets have proper introspection names (via `_from_params`). The QAP relies on these IDs to identify which widget to grab.
    -   Example: If your slider is named "exposure" in `params_t`, the QAP can reference it as `exposure/exposure`.
3.  **Separability**: Avoid tight coupling between widgets in your layout if possible. The QAP extracts individual widgets, so they should make sense in isolation.
4.  **Tooltips**: Provide good tooltips, as they are preserved in the QAP.

### Restrictions
-   Complex custom widgets (graphs, curves) may not be suitable for the QAP.
-   Multi-instance modules have limitations in the QAP (often disabled or limited to the first instance).
