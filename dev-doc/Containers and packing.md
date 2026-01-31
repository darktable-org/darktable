Unlike some GUI systems where you might specify exact pixel coordinates (x, y), GTK uses a layout management system. You place widgets *inside* **container widgets**, and tell the container *how* to arrange its children.

**1. Containers:**

*   Think of containers as boxes, grids, or special holders that manage the position and size of other widgets placed inside them.
*   Common examples you'll see in darktable:
    *   **`GtkBox`**: The most basic container. It arranges its children in a single row (horizontal orientation) or a single column (vertical orientation). This is used *very* frequently.
    *   **`GtkGrid`**: Arranges children in a grid of rows and columns, like a spreadsheet. More complex but offers more precise alignment.
    *   **`GtkNotebook`**: Manages multiple "pages" (each usually a container itself) with tabs to switch between them.
    *   **`GtkExpander`**: A collapsible section with a header and a content area (often containing a `GtkBox`).
    *   **`GtkOverlay`**: Stacks widgets on top of each other, useful for putting controls *over* an image or drawing area.
    *   **`GtkScrolledWindow`**: Provides scrollbars for a single child widget that might be larger than the allocated space.
    *   **`GtkEventBox`**: A simple container that just holds one child but allows it to receive events it might not normally handle (like clicks on a label).

**2. Darktable Layout API**

To ensure compatibility with GTK4, darktable uses specific wrapper functions. You must use these instead of standard GTK packing functions.

*   **`dt_gui_box_add(GtkWidget *box, GtkWidget *child)`**
    *   **Replaces:** `gtk_box_pack_start`, `gtk_box_pack_end`.
    *   **Usage:** Adds a widget to the end of the container.
*   **`dt_gui_vbox()` / `dt_gui_hbox()`**
    *   **Replaces:** `gtk_box_new(..., spacing)`.
    *   **Usage:** Creates a box with standard darktable spacing (`DT_BAUHAUS_SPACE`).
*   **`dt_ui_label_new(const char *text)`**
    *   **Replaces:** `gtk_label_new()`.
    *   **Usage:** Creates a label that correctly ellipsizes text (shortens with "...") to prevent panel stretching.

**3. Controlling Layout: Order, Expansion, and Alignment**

Since `dt_gui_box_add` does not accept layout arguments (like "expand" or "fill"), you control the layout by setting properties **on the widget itself** before adding it.

**A. Order**
Widgets appear in the order they are added.
*   **Vertical Box:** Top to Bottom.
*   **Horizontal Box:** Left to Right.

**B. Expansion (`hexpand`, `vexpand`)**
Determines if a widget should claim empty space in the container.
*   **`gtk_widget_set_hexpand(widget, TRUE)`**: Widget requests extra horizontal space.
*   **`gtk_widget_set_vexpand(widget, TRUE)`**: Widget requests extra vertical space.
*   **Default:** `FALSE` (Buttons, Labels, Toggles). Set to `TRUE` for Graphs, Text Entries, or Scroll Areas.

**C. Alignment (`halign`, `valign`)**
Determines how the widget fits *within* its allocated space.
*   **`GTK_ALIGN_FILL`**: Stretch to fill the slot (Standard for inputs/sliders).
*   **`GTK_ALIGN_CENTER`**: Center within the slot (Standard for checkboxes).
*   **`GTK_ALIGN_START` / `GTK_ALIGN_END`**: Align to Left/Top or Right/Bottom.
*   **Usage:** `gtk_widget_set_halign(widget, GTK_ALIGN_FILL);`

**D. Example: Search Toolbar**

```c
// 1. Create container (Horizontal)
GtkWidget *hbox = dt_gui_hbox();

// 2. Create widgets
GtkWidget *label = dt_ui_label_new("Search:");
GtkWidget *entry = gtk_entry_new();
GtkWidget *button = gtk_button_new_with_label("Go");

// 3. Pack in Order

// Left: Label (Natural size)
dt_gui_box_add(hbox, label);

// Middle: Entry (Expands and Fills)
gtk_widget_set_hexpand(entry, TRUE);          // Claim empty width
gtk_widget_set_halign(entry, GTK_ALIGN_FILL); // Stretch to fill it
dt_gui_box_add(hbox, entry);

// Right: Button (Natural size, pushed to end by Entry)
dt_gui_box_add(hbox, button);
```

**4. Practical Pattern for IOP Modules**

In `gui_init()`, you typically see:

```c
void gui_init(dt_iop_module_t *self)
{
  dt_iop_mymodule_gui_data_t *g = IOP_GUI_ALLOC(mymodule);

  // Create main container
  self->widget = dt_gui_vbox();

  // dt_bauhaus_*_from_params() auto-packs into self->widget
  g->slider1 = dt_bauhaus_slider_from_params(self, "param1");
  g->slider2 = dt_bauhaus_slider_from_params(self, "param2");

  // For non-introspection widgets, manually add:
  GtkWidget *label = dt_ui_section_label_new(_("advanced"));
  dt_gui_box_add(self->widget, label);

  g->slider3 = dt_bauhaus_slider_from_params(self, "param3");
}
```

Note: The `dt_bauhaus_*_from_params()` functions automatically pack into `self->widget`, so you don't need explicit packing calls for them. Only use `dt_gui_box_add()` for widgets that don't auto-pack (like section labels or manually created widgets).
