#ifndef DT_GUI_PRESETS_H
#define DT_GUI_PRESETS_H

// format flags stored into the presets database
typedef enum dt_gui_presets_format_flag_t
{
  FOR_LDR = 1 << 0,
  FOR_RAW = 1 << 1,
  FOR_HDR = 1 << 2
} dt_gui_presets_format_flag_t;

/** create a db table with presets for all operations. */
void dt_gui_presets_init();

/** add or replace a generic (i.e. non-exif specific) preset for this operation. */
void dt_gui_presets_add_generic(const char *name, dt_dev_operation_t op, const int32_t version,
                                const void *params, const int32_t params_size, const int32_t enabled);

/** update match strings for maker, model, lens. */
void dt_gui_presets_update_mml(const char *name, dt_dev_operation_t op, const int32_t version,
                               const char *maker, const char *model, const char *lens);
/** update ranges for iso, aperture, exposure, and focal length, respectively. */
void dt_gui_presets_update_iso(const char *name, dt_dev_operation_t op, const int32_t version,
                               const float min, const float max);
void dt_gui_presets_update_av(const char *name, dt_dev_operation_t op, const int32_t version, const float min,
                              const float max);
void dt_gui_presets_update_tv(const char *name, dt_dev_operation_t op, const int32_t version, const float min,
                              const float max);
void dt_gui_presets_update_fl(const char *name, dt_dev_operation_t op, const int32_t version, const float min,
                              const float max);
/** update ldr flag: 0-dont care, 1-low dynamic range, 2-raw */
void dt_gui_presets_update_ldr(const char *name, dt_dev_operation_t op, const int32_t version,
                               const int ldrflag);
/** set auto apply property of preset. */
void dt_gui_presets_update_autoapply(const char *name, dt_dev_operation_t op, const int32_t version,
                                     const int autoapply);
/** set filter mode. if 1, the preset will only show for matching images. */
void dt_gui_presets_update_filter(const char *name, dt_dev_operation_t op, const int32_t version,
                                  const int filter);

/** show a popup menu without initialized module. */
void dt_gui_presets_popup_menu_show_for_params(dt_dev_operation_t op, int32_t version, void *params,
                                               int32_t params_size, void *blendop_params,
                                               const dt_image_t *image,
                                               void (*pick_callback)(GtkMenuItem *, void *),
                                               void *callback_data);

/** show the popup menu for the given module, with default behavior. */
void dt_gui_presets_popup_menu_show_for_module(dt_iop_module_t *module);

/** show popupmenu for favorite modules */
void dt_gui_favorite_presets_menu_show();

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
