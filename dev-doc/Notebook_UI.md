**Core Concept: GtkNotebook**

The foundation for tabbed interfaces in GTK (and therefore darktable's usage) is the `GtkNotebook` widget. It's a container that manages multiple "pages" (each being another container like a `GtkBox`) and displays tabs to switch between them.

**Darktable Helper Functions**

Darktable provides convenient wrapper functions to simplify the creation and management of notebooks within the IOP GUI structure:

1.  **`GtkNotebook *dt_ui_notebook_new(dt_action_def_t *def)`** (`gui/gtk.h`, `gui/gtk.c`)
    *   **Purpose:** Creates a new `GtkNotebook` widget, ready to have pages added.
    *   **`dt_action_def_t *def`:** This is crucial for integrating with darktable's shortcut system. You pass a pointer to a `dt_action_def_t` struct. As you add pages using `dt_ui_notebook_page`, this struct will be populated with elements corresponding to each tab, allowing you to define shortcuts for switching directly to specific tabs (e.g., "activate tab 'look'"). You typically define a static `dt_action_def_t` struct within your module's C file for this purpose. It gets filled dynamically by the helper functions.
    *   **Returns:** A pointer to the newly created `GtkNotebook`.

2.  **`GtkWidget *dt_ui_notebook_page(GtkNotebook *notebook, const char *text, const char *tooltip)`** (`gui/gtk.h`, `gui/gtk.c`)
    *   **Purpose:** Adds a new page (tab) to an existing `GtkNotebook`.
    *   **`GtkNotebook *notebook`:** The notebook created by `dt_ui_notebook_new`.
    *   **`const char *text`:** The translatable label for the tab (e.g., `N_("look")`).
    *   **`const char *tooltip`:** An optional translatable tooltip for the tab.
    *   **Returns:** A `GtkWidget*` which is the *content container* for the newly added page (typically a `GtkBox`). You pack all the controls (sliders, comboboxes, etc.) for that specific tab *into this returned widget*.
    *   **Side Effect:** This function also updates the `dt_action_def_t` struct originally passed to `dt_ui_notebook_new`, adding a new element definition corresponding to this tab, using the provided `text` as the element name.

**Key `dt_iop_module_t` Member**

*   **`self->widget`**: This pointer **must** be set to the *top-level container* widget for your module's entire UI. If your UI includes elements *outside* the notebook (like the graphs in `filmicrgb` or `toneequal`), `self->widget` should point to the container holding *both* the notebook *and* those other elements (often a `GtkVBox`). If the notebook *is* the entire UI, you can point `self->widget` to it, but it's generally safer to wrap it in a box.

**Standard Widget Creation Helpers**

You'll populate the pages returned by `dt_ui_notebook_page` using standard darktable GUI helper functions, often linked to module parameters defined via introspection:

*   **`dt_bauhaus_slider_from_params(dt_iop_module_t *self, const char *param)`** (`develop/imageop_gui.h`, `develop/imageop_gui.c`)
*   **`dt_bauhaus_combobox_from_params(dt_iop_module_t *self, const char *param)`** (`develop/imageop_gui.h`, `develop/imageop_gui.c`)
*   **`dt_bauhaus_toggle_from_params(dt_iop_module_t *self, const char *param)`** (`develop/imageop_gui.h`, `develop/imageop_gui.c`)
*   **`dt_ui_section_label_new(const gchar *str)`** (`gui/gtk.h`, `gui/gtk.c`): For adding visual separators/headers *within* a tab page.

**Steps to Create a Tabbed Module UI (Inside `gui_init`)**

1.  **Create Main Container:** Create the top-level container for your module's UI, usually a vertical `GtkBox`.
    ```c
    // In gui_init(dt_iop_module_t *self)
    dt_iop_yourmodule_gui_data_t *g = IOP_GUI_ALLOC(yourmodule);
    GtkWidget *main_vbox = dt_gui_vbox();
    ```

2.  **Define Action Struct:** Define a static `dt_action_def_t` for the notebook shortcuts. It will be populated later.
    ```c
    static dt_action_def_t notebook_def = { }; // Name and process filled later
    ```
`notebook_def` needs to be `static` in this context:
  - **Lifetime of Automatic Variables:** Variables declared inside a function without the `static` keyword are *automatic* variables. They are typically allocated on the function's stack frame. When the function (`gui_init` in this case) finishes executing and returns, its stack frame is destroyed, and all automatic variables within it cease to exist. Their memory locations are reclaimed and can be overwritten by subsequent function calls.
  - **Passing by Address:** Both `dt_ui_notebook_new` and `dt_action_define_iop` take the *address* of the `notebook_def` struct (`&notebook_def`). They don't make a full copy of the struct itself.
  - **Action System Registration:** The `dt_action_define_iop` function registers the action (and its associated definition structure, including the `.elements` array which gets populated by `dt_ui_notebook_page`) with darktable's central action/shortcut system. This system needs to be able to refer back to this definition *later*, potentially long after the `gui_init` function has returned (e.g., when a shortcut is pressed, or when displaying the shortcut mapping preferences).
  - **The Dangling Pointer Problem:** If `notebook_def` were *not* static, it would be an automatic variable. When `gui_init` returned, the memory where `notebook_def` resided would be invalid. However, the action system would still hold a pointer (`&notebook_def`) to that now-invalid memory location. This is called a **dangling pointer**. Attempting to access the data through this dangling pointer later (e.g., to process a shortcut for switching tabs) would lead to **undefined behavior** – most likely a crash, but potentially silent data corruption.
  - **`static` Solution:** By declaring `notebook_def` as `static`, you change its storage duration. It is no longer allocated on the stack. Instead, it's allocated in a static memory segment and persists for the *entire lifetime of the program*. This ensures that the address (`&notebook_def`) passed to the action system remains valid throughout the program's execution, and the action system can safely access the definition struct whenever needed.
  - **In short:** `static` is required here to guarantee that the `dt_action_def_t` struct, which the action system needs to reference later, remains alive and valid in memory even after the `gui_init` function has completed.

3.  **Create Notebook:** Call `dt_ui_notebook_new` with the address of your action definition struct.
    ```c
    g->notebook = dt_ui_notebook_new(&notebook_def);
    ```

4.  **Add Graph/Other Elements (Optional):** If you have UI elements *outside* the notebook (like a graph area), pack them into the `main_vbox` *before* or *after* the notebook.
    ```c
    g->area = GTK_DRAWING_AREA(dt_ui_resize_wrap(...));
    
    // Configure expansion on the widget directly (replaces expand=TRUE, fill=TRUE)
    gtk_widget_set_vexpand(GTK_WIDGET(g->area), TRUE);
    gtk_widget_set_valign(GTK_WIDGET(g->area), GTK_ALIGN_FILL);
    
    // add to the box
    dt_gui_box_add(main_vbox, GTK_WIDGET(g->area));     // Pack graph first
    dt_gui_box_add(main_vbox, GTK_WIDGET(g->notebook)); // Then the notebook
    ```

5.  **Add Pages and Controls:** For each desired tab:
    *   Call `dt_ui_notebook_page` to get the content container for that page.
    *   Create and pack the necessary sliders, comboboxes, labels, etc., *into* the widget returned by `dt_ui_notebook_page`. Use `dt_bauhaus_*_from_params` helpers.
    *   Optionally use `dt_ui_section_label_new` or `dt_gui_new_collapsible_section` for organization within the page.
    *   Optionally use the `DT_IOP_SECTION_FOR_PARAMS` macro to associate specific controls with a named section within the tab for shortcut mapping.

    ```c
    // Page 1: "Look"
    GtkWidget *page_look = dt_ui_notebook_page(g->notebook, N_("look"), _("Look adjustments"));
    self->widget = page_look; // Temporarily set self->widget for dt_bauhaus_* helpers
    dt_bauhaus_slider_from_params(self, "look_offset");
    dt_bauhaus_slider_from_params(self, "look_slope");
    // ... add other widgets for the "Look" tab ...

    // Page 2: "Curve"
    GtkWidget *page_curve = dt_ui_notebook_page(g->notebook, N_("curve"), _("Curve adjustments"));
    self->widget = page_curve; // Temporarily set self->widget
    // Use DT_IOP_SECTION_FOR_PARAMS to group shortcuts under "curve/range"
    dt_iop_module_t *sect_range = DT_IOP_SECTION_FOR_PARAMS(self, N_("range"));
    dt_bauhaus_slider_from_params(sect_range, "range_black_relative_exposure");
    dt_bauhaus_slider_from_params(sect_range, "range_white_relative_exposure");
    // ... add other widgets for the "Curve" tab ...

    // Add more pages as needed...
    ```

6.  **Set `self->widget`:** Point `self->widget` to the *main container* created in step 1.
    ```c
    self->widget = main_vbox; // Restore the main container as self->widget
    ```

7.  **Register Notebook Action:** Register the notebook itself with the action system using the struct populated by `dt_ui_notebook_page`.
    ```c
    // Define the action name and process function for the notebook struct
    notebook_def.name = "page"; // Or any suitable name
    notebook_def.process = _action_process_tabs; // (If using the standard tab switching logic)
    dt_action_define_iop(self, NULL, N_("page"), GTK_WIDGET(g->notebook), &notebook_def);
    ```

**Advanced Patterns**

*   **Using `GtkStack` with `GtkNotebook` (`colorequal.c`):**
    *   Instead of packing controls directly into the page returned by `dt_ui_notebook_page`, you create a `GtkStack`.
    *   For each set of controls that should appear for a specific tab, create a container (e.g., `GtkBox`) and add it as a named child to the `GtkStack`.
    *   Connect to the notebook's `"switch-page"` signal. In the callback, get the current page number and set the `GtkStack`'s visible child using `gtk_stack_set_visible_child_name`.
    *   This is useful when you have *similar controls* (like banks of sliders) but they need to operate on different parameters based on the active tab (hue vs. saturation vs. brightness).

*   **Collapsible Sections (`dt_gui_collapsible_section_t`):**
    *   Used in `colorequal.c` within the "options" tab.
    *   Call `dt_gui_new_collapsible_section` to create a header with a toggle and a container.
    *   Pack widgets into the `cs->container` GtkBox.
    *   This provides further organization *within* a single notebook page.

**Important Points & Caveats**

1.  **`self->widget` Assignment:** This is critical. It *must* point to the widget that contains *all* UI elements for the module that should be packed into the main GUI panel (often the right-hand panel). Failure to do this correctly can lead to UI elements not appearing or malfunctioning.
2.  **Widget Packing:** Widgets intended for a specific tab *must* be packed into the container widget *returned by* `dt_ui_notebook_page` for that tab (or into a `GtkStack` child if using that pattern).
3.  **Shortcut Definition:** Tab-switching shortcuts are defined via the `dt_action_def_t` struct passed to `dt_ui_notebook_new`. The `dt_ui_notebook_page` function automatically adds elements to this definition. You then register the *notebook itself* using `dt_action_define_iop`. Shortcuts for controls *within* a tab are defined normally using `dt_bauhaus_*_from_params` or manually with `dt_action_define_iop` on the specific control.
4.  **`DT_IOP_SECTION_FOR_PARAMS`:** This macro is specifically for organizing shortcuts hierarchically. It doesn't create visual sections but allows shortcuts like "module/Tab Name/Section Name/Parameter Name". Use `dt_ui_section_label_new` for visual separation within a tab.
5.  **State Management:** Remember to save/restore the active tab index using `dt_conf_set_int`/`dt_conf_get_int` in `gui_cleanup`/`gui_update` if you want the module to remember the last used tab. (See `colorequal.c` and `toneequal.c`).
6.  **Translations:** Use `N_("...")` for static translatable strings (like tab labels passed to `dt_ui_notebook_page`) and `_("...")` for runtime translatable strings.
7.  **Complexity:** While notebooks offer good organization, overly complex nesting (notebooks containing stacks containing collapsible sections) can become harder to manage. Choose the simplest structure that meets the organizational need.

By following these steps and patterns, referencing the example modules provided, you can effectively create multi-tabbed user interfaces for your darktable modules. Remember that the `gui_init` function is the central place where this UI structure is built.
