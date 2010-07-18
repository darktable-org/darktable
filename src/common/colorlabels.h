#ifndef DT_COMMON_COLORLABELS_H
#define DT_COMMON_COLORLABELS_H

/** remove assigned colorlabels of selected images*/
void dt_colorlabels_remove_labels_selection ();
/** remove labels associated to imgid */
void dt_colorlabels_remove_labels (const int imgid);
/** toggle color label of selection of images */
void dt_colorlabels_toggle_label_selection (const int color);
/** toggle color of imgid */
void dt_colorlabels_toggle_label (const int imgid, const int color);
/** assign a color label to imgid */
void dt_colorlabels_set_label (const int imgid, const int color);
/** remove a color label from imgid */
void dt_colorlabels_remove_label (const int imgid, const int color);

void dt_colorlabels_key_accel_callback(void *user_data);
void dt_colorlabels_register_key_accels();
void dt_colorlabels_unregister_key_accels();

#endif
