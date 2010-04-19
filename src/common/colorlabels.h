#ifndef DT_COMMON_COLORLABELS_H
#define DT_COMMON_COLORLABELS_H

void dt_colorlabels_remove_labels_selection ();
void dt_colorlabels_remove_labels (const int imgid);
void dt_colorlabels_toggle_label_selection (const int color);
void dt_colorlabels_toggle_label (const int imgid, const int color);
void dt_colorlabels_key_accel_callback(void *user_data);

void dt_colorlabels_register_key_accels();
void dt_colorlabels_unregister_key_accels();

#endif
