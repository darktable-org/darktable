#pragma once

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

/** get the assigned colorlabels of imgid*/
int dt_colorlabels_get_labels(const int imgid);
/** remove labels associated to imgid */
void dt_colorlabels_remove_labels(const int imgid);
/** assign a color label to imgid - no undo no image group*/
void dt_colorlabels_set_label(const int imgid, const int color);
/** assign a color label to image imgid or all selected for imgid == -1*/
void dt_colorlabels_set_labels(const int imgid, const int color, const gboolean clear_on, const gboolean undo_on, const gboolean group_on);
/** remove a color label from imgid */
void dt_colorlabels_remove_label(const int imgid, const int color);
/** get the name of the color for a given number (could be replaced by an array) */
const char *dt_colorlabels_to_string(int label);
/** check if an image has a color label */
int dt_colorlabels_check_label(const int imgid, const int color);

gboolean dt_colorlabels_key_accel_callback(GtkAccelGroup *accel_group, GObject *acceleratable, guint keyval,
                                           GdkModifierType modifier, gpointer data);

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
