#ifndef DT_GUI_PRESETS_H
#define DT_GUI_PRESETS_H


/** create a db table with presets for all operations. */
void dt_gui_presets_init();

/** add or replace a generic (i.e. non-exif specific) preset for this operation. */
void dt_gui_presets_add_generic(const char *name, dt_dev_operation_t op, const void *params, const int32_t params_size, const int32_t enabled);

/** show a popup menu without initialized module. need a lot of params for that. */
void dt_gui_presets_popup_menu_show_for_params(dt_dev_operation_t op, dt_iop_params_t *params, int32_t params_size, void (*pick_callback)(GtkMenuItem*,void*), void *callback_data);

/** show the popup menu for the given module, with default behavior. */
void dt_gui_presets_popup_menu_show_for_module(dt_iop_module_t *module);

#endif
