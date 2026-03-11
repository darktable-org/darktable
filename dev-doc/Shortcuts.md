# Shortcuts System

The shortcut system in darktable is built upon a unified **Action System**. Actions abstract user interactions (like pressing a key, clicking a button, or scrolling) and map them to specific functions in modules or views.

## The Action System (`dt_action_def_t`)

Every shortcut-able action is defined by a `dt_action_def_t` structure. This structure links a human-readable name to a processing function.

### Key Components

-   **`dt_action_t`**: The runtime representation of an action. It forms a hierarchical tree (e.g., `iop/exposure/exposure`).
-   **`dt_action_def_t`**: The static definition.
    ```c
    typedef struct dt_action_def_t
    {
      const gchar *name;          // Internal name
      // Function to execute when action is triggered
      float (*process)(gpointer target, dt_action_element_t, dt_action_effect_t, float size);
      const dt_action_element_def_t *elements; // Sub-elements (optional)
      const dt_shortcut_fallback_t *fallbacks; // Default shortcuts
      const gboolean no_widget;   // True if not associated with a specific widget
    } dt_action_def_t;
    ```

## Implementing Shortcuts in Modules

For most IOP modules, you don't need to manually define actions. The `dt_bauhaus` widget library handles this automatically.

### Automatic Registration
When you create a widget using introspection (e.g., `dt_bauhaus_slider_from_params`), the system automatically registers an action for it. The action ID is derived from the parameter name.

### Manual Registration
If you are creating custom widgets or non-widget actions, you can register them manually.

#### 1. Define the Action
In your module code (often in `gui_init` or a helper), define the action structure.

#### 2. Register with `dt_action_register`
```c
dt_action_register(parent_action, "action_name", callback_function, default_key, modifiers);
```

### Shortcuts for Buttons
To create a button that has a shortcut, use `dt_action_button_new`:
```c
GtkWidget *btn = dt_action_button_new(module, _("button label"), 
                                      callback, user_data, 
                                      _("tooltip"), 
                                      GDK_KEY_e, GDK_CONTROL_MASK); // Default shortcut Ctrl+E
```

## Adding Shortcuts to existing Modules
If an existing module control lacks a shortcut:
1.  Check if it's a standard `dt_bauhaus` widget. If so, it should already have one. Check `shortcuts` tab in preferences.
2.  If it's a custom widget (e.g., a drawing area), you may need to implement a specific action definition and connect it.

## User Configuration
Users can customize shortcuts in **Preferences > Shortcuts**. The system scans all registered actions and presents them in a hierarchical list.
