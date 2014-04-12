/*
    This file is part of darktable,
    copyright (c) 2009--2011 Henrik Andersson.

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "common/darktable.h"
#include "control/conf.h"

#include "gui/gtk.h"
#include "gui/contrast.h"

#include <math.h>

#define CONTRAST_STEP 0.1
#define CONTRAST_AMOUNT 0.4
#define BRIGHTNESS_STEP 0.1
#define BRIGHTNESS_AMOUNT 1.0

GtkStyle *_main_window_orginal_style;
GtkStyle *_module_orginal_style;

#define CLIP(v) (v>1.0?1.0:(v<0.0?0.0:v) )

void
_gui_contrast_apply ()
{
  /* calculate contrast multipliers */
  float contrast = dt_conf_get_float ("ui_contrast");
  float contrast_amount = contrast*CONTRAST_AMOUNT;
  float contrast_increase = 1.0+contrast_amount;
  float contrast_decrease = 1.0-contrast_amount;

  /* calculate a brightness multiplier */
  float brightness = (1.0 + ((-0.2 + dt_conf_get_float ("ui_brightness")) * BRIGHTNESS_AMOUNT) );

  gchar rc[4096]= {0};
  g_snprintf (rc,sizeof(rc),"\
style \"clearlooks-default\" \
{ \
  text[NORMAL] = \"#%.2x%.2x%.2x\" \
  text[ACTIVE] = \"#%.2x%.2x%.2x\" \
  text[INSENSITIVE] = \"#%.2x%.2x%.2x\" \
  bg[NORMAL] = \"#%.2x%.2x%.2x\" \
  bg[ACTIVE] = \"#%.2x%.2x%.2x\" \
  bg[SELECTED] = \"#%.2x%.2x%.2x\" \
  base[NORMAL] = \"#%.2x%.2x%.2x\" \
  base[ACTIVE] = \"#%.2x%.2x%.2x\" \
} \
 \
style \"clearlooks-brightbg\" = \"clearlooks-default\" \
{ \
  bg[NORMAL] = \"#%.2x%.2x%.2x\" \
}\
\
style \"clearlooks-vbrightbg\" = \"clearlooks-default\" \
{ \
  bg[NORMAL]   = \"#606060\" \
  bg[PRELIGHT] = \"#D0D0D0\" \
}   \
",
              /* clearlooks-default */
              //text[NORMAL]
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->text[GTK_STATE_NORMAL].red * contrast_increase)/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->text[GTK_STATE_NORMAL].green * contrast_increase)/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->text[GTK_STATE_NORMAL].blue * contrast_increase)/65535.0)*brightness),0,255),
              //text[ACTIVE]
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->text[GTK_STATE_ACTIVE].red * contrast_increase)/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->text[GTK_STATE_ACTIVE].green * contrast_increase)/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->text[GTK_STATE_ACTIVE].blue * contrast_increase)/65535.0)*brightness),0,255),
              //text[INSENSITIVE]
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->text[GTK_STATE_INSENSITIVE].red * contrast_increase)/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->text[GTK_STATE_INSENSITIVE].green * contrast_increase)/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->text[GTK_STATE_INSENSITIVE].blue * contrast_increase)/65535.0)*brightness),0,255),
              // bg[NORMAL]
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_NORMAL].red * contrast_decrease)/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_NORMAL].green * contrast_decrease)/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_NORMAL].blue * contrast_decrease)/65535.0)*brightness),0,255),
              // bg[ACTIVE[
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_ACTIVE].red * contrast_decrease)/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_ACTIVE].green * contrast_decrease)/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_ACTIVE].blue * contrast_decrease)/65535.0)*brightness),0,255),
              // bg[SELECTED]
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_SELECTED].red * contrast_decrease)/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_SELECTED].green * contrast_decrease)/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_SELECTED].blue * contrast_decrease)/65535.0)*brightness),0,255),

              // base[NORMAL]
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->base[GTK_STATE_NORMAL].red * contrast_decrease)/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->base[GTK_STATE_NORMAL].green * contrast_decrease)/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->base[GTK_STATE_NORMAL].blue * contrast_decrease)/65535.0)*brightness),0,255),
              // base[ACTIVE]
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->base[GTK_STATE_ACTIVE].red * contrast_decrease)/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->base[GTK_STATE_ACTIVE].green * contrast_decrease)/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_main_window_orginal_style->base[GTK_STATE_ACTIVE].blue * contrast_decrease)/65535.0)*brightness),0,255),

              /* clearlooks-brightbg */
              (int)CLAMP((255*CLIP ((_module_orginal_style->bg[GTK_STATE_NORMAL].red * (1.0+(contrast_amount*0.1)) )/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_module_orginal_style->bg[GTK_STATE_NORMAL].green * (1.0+(contrast_amount*0.1)) )/65535.0)*brightness),0,255),
              (int)CLAMP((255*CLIP ((_module_orginal_style->bg[GTK_STATE_NORMAL].blue * (1.0+(contrast_amount*0.1)) )/65535.0)*brightness),0,255)


             );

  gtk_rc_parse_string (rc);

  /* apply newly parsed colors */
  gtk_rc_reset_styles (gtk_settings_get_default());
}

void
dt_gui_contrast_init ()
{
  /* create a copy of original style of window */
  GtkWidget *window = dt_ui_main_window(darktable.gui->ui);

  /* realize window to ensure style is applied before copy */
  gtk_widget_realize(window);
  _main_window_orginal_style = gtk_style_copy (gtk_widget_get_style(window));

  /* get clearlooks-brightbg original style */

  /* create a eventbox and add to */
  GtkWidget *ev = gtk_event_box_new();
  dt_ui_container_add_widget(darktable.gui->ui, DT_UI_CONTAINER_PANEL_LEFT_CENTER,ev);
  gtk_widget_realize(ev);
  _module_orginal_style = gtk_style_copy (gtk_widget_get_style(ev));

  gtk_widget_destroy(ev);

  /* apply current contrast value */
  _gui_contrast_apply ();
}

void
dt_gui_contrast_increase ()
{
  float contrast = dt_conf_get_float ("ui_contrast");
  if (contrast < 1.0)
  {
    /* calculate new contrast and store value */
    contrast = fmin (1.0,contrast+CONTRAST_STEP);
    dt_conf_set_float ("ui_contrast",contrast);
    /* apply new contrast setting */
    _gui_contrast_apply ();
  }
}

void
dt_gui_contrast_decrease ()
{
  float contrast = dt_conf_get_float("ui_contrast");
  if (contrast > 0.0)
  {
    /* calculate new contrast and store value */
    contrast = fmax (0.0,contrast-CONTRAST_STEP);
    dt_conf_set_float("ui_contrast",contrast);
    /* apply new contrast setting */
    _gui_contrast_apply ();
  }
}


void dt_gui_brightness_increase()
{
  float brightness = dt_conf_get_float ("ui_brightness");
  if (brightness < 1.0)
  {
    /* calculate new brightness and store value */
    brightness = fmin (1.0,brightness+BRIGHTNESS_STEP);
    dt_conf_set_float ("ui_brightness",brightness);
    /* apply new brightness setting */
    _gui_contrast_apply ();
  }
}

void dt_gui_brightness_decrease()
{
  float brightness = dt_conf_get_float ("ui_brightness");
  if (brightness > 0.0)
  {
    /* calculate new brightness and store value */
    brightness = fmax (0.0,brightness-BRIGHTNESS_STEP);
    dt_conf_set_float ("ui_brightness",brightness);
    /* apply new brightness setting */
    _gui_contrast_apply ();
  }
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
