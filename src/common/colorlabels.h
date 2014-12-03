#ifndef DT_COMMON_COLORLABELS_H
#define DT_COMMON_COLORLABELS_H

#include "common/darktable.h"
#include <gtk/gtk.h>
/** array of names and constant to ease label manipulation */
typedef enum dt_colorlables_enum
{
  DT_COLORLABELS_RED,
  DT_COLORLABELS_YELLOW,
  DT_COLORLABELS_GREEN,
  DT_COLORLABELS_BLUE,
  DT_COLORLABELS_PURPLE,
  DT_COLORLABELS_LAST,
} dt_colorlabels_enum;
/** array with all names as strings, terminated by a NULL entry */
extern const char *dt_colorlabels_name[];

/** remove assigned colorlabels of selected images*/
void dt_colorlabels_remove_labels_selection();
/** remove labels associated to imgid */
void dt_colorlabels_remove_labels(const int imgid);
/** toggle color label of selection of images */
void dt_colorlabels_toggle_label_selection(const int color);
/** toggle color of imgid */
void dt_colorlabels_toggle_label(const int imgid, const int color);
/** assign a color label to imgid */
void dt_colorlabels_set_label(const int imgid, const int color);
/** remove a color label from imgid */
void dt_colorlabels_remove_label(const int imgid, const int color);
/** get the name of the color for a given number (could be replaced by an array) */
const char *dt_colorlabels_to_string(int label);
/** check if an image has a color label */
int dt_colorlabels_check_label(const int imgid, const int color);

gboolean dt_colorlabels_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data);

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
