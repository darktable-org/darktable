# Quick Access Panel (QAP)

The Quick Access Panel (QAP) is a specialized view in the darkroom that aggregates frequently used controls from different modules into a single, compact interface.

## Purpose

Instead of switching between tabs and scrolling to find specific sliders (e.g., "Exposure", "Contrast", "White Balance"), the user can have them all in one list. This mimics the "Basic" panel found in other raw processors.

## Mechanism

The QAP is implemented as part of the `modulegroups` library (`src/libs/modulegroups.c`). It is technically a special "Basics" group (`DT_MODULEGROUP_BASICS`).

### How Modules Provide Widgets to QAP

A module doesn't explicitly push widgets to the QAP. Instead, the QAP pulls widgets from modules based on configuration.

1.  **Selection**: The QAP configuration lists modules and specific widgets (by name/ID) to include.
2.  **Reparenting**: When the QAP is active, `modulegroups.c` *steals* the widget from the original module's GUI and places it into the QAP box.
3.  **Restoration**: When leaving the QAP or the module, the widget is put back into its original container.

In detail, the framework:

1.  `g_object_ref()`s the widget, so that removing it from its parent does not destroy it.
2.  Removes it from that parent.
3.  Inserts a placeholder at the original position, so the module's layout keeps the gap.
4.  Packs the widget into the QAP container.
5.  Connects `notify::visible` signals, so the two copies of the layout stay in sync.
6.  On QAP hide, reverses all of this, restoring the widget at its original position.

Step 2 and step 6 are why a QAP-eligible widget has to sit in a `GtkBox` or a `GtkGrid` parent: those are the containers the framework knows how to remove from and restore into. See [GUI.md](GUI.md#qap-reparenting-framework-managed) for what this asks of a module author.

## Developer Considerations

To ensure your module works well with the QAP:

1.  **Use Standard Widgets**: Use `dt_bauhaus_*` widgets. Custom widgets are harder to integrate.
2.  **Naming**: Ensure your widgets have proper introspection names (via `_from_params`). The QAP relies on these IDs to identify which widget to grab.
    -   Example: If your slider is named "exposure" in `params_t`, the QAP can reference it as `exposure/exposure`.
3.  **Separability**: Avoid tight coupling between widgets in your layout if possible. The QAP extracts individual widgets, so they should make sense in isolation.
4.  **Tooltips**: Provide good tooltips, as they are preserved in the QAP.

### Restrictions
-   Complex custom widgets (graphs, curves) may not be suitable for the QAP: they are harder to reparent, and they rarely make sense out of their module's context.
-   A QAP entry names a module by its operation name, not by instance. When several instances of a module exist, only one of them supplies the widget — the QAP binds the entry to the first instance it meets and leaves the others alone (`src/libs/modulegroups.c`). The layout editor lists one instance per module for the same reason.
