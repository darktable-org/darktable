/*
    This file is part of darktable,
    copyright (c) 2009--2010 Henrik Andersson.

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
#include <math.h>
#include "gui/gtk.h"
#include "gui/contrast.h"

#include "common/darktable.h"
#include "control/conf.h"

#define CONTRAST_STEP 0.1
#define CONTRAST_AMOUNT 0.4

GtkStyle *_main_window_orginal_style;
GtkStyle *_module_orginal_style;

#define CLIP(v) (v>1.0?1.0:(v<0.0?0.0:v) )

void 
_gui_contrast_apply ()
{
  float contrast = dt_conf_get_float ("ui_contrast");
  float amount=contrast*CONTRAST_AMOUNT;
  float increase = 1.0+amount;
  float decrease = 1.0-amount;
  
  gchar rc[4096]={0};
  g_snprintf (rc,4096,"\
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
    (int)(255*CLIP ((_main_window_orginal_style->text[GTK_STATE_NORMAL].red * increase)/65535.0)),
    (int)(255*CLIP ((_main_window_orginal_style->text[GTK_STATE_NORMAL].green * increase)/65535.0)),
    (int)(255*CLIP ((_main_window_orginal_style->text[GTK_STATE_NORMAL].blue * increase)/65535.0)),
    //text[ACTIVE]
    (int)(255*CLIP ((_main_window_orginal_style->text[GTK_STATE_ACTIVE].red * increase)/65535.0)),
    (int)(255*CLIP ((_main_window_orginal_style->text[GTK_STATE_ACTIVE].green * increase)/65535.0)),
    (int)(255*CLIP ((_main_window_orginal_style->text[GTK_STATE_ACTIVE].blue * increase)/65535.0)),
    //text[INSENSITIVE]
    (int)(255*CLIP ((_main_window_orginal_style->text[GTK_STATE_INSENSITIVE].red * increase)/65535.0)),
    (int)(255*CLIP ((_main_window_orginal_style->text[GTK_STATE_INSENSITIVE].green * increase)/65535.0)),
    (int)(255*CLIP ((_main_window_orginal_style->text[GTK_STATE_INSENSITIVE].blue * increase)/65535.0)),
    // bg[NORMAL]
    (int)(255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_NORMAL].red * decrease)/65535.0)),
    (int)(255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_NORMAL].green * decrease)/65535.0)),
    (int)(255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_NORMAL].blue * decrease)/65535.0)),
    // bg[ACTIVE[
    (int)(255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_ACTIVE].red * decrease)/65535.0)),
    (int)(255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_ACTIVE].green * decrease)/65535.0)),
    (int)(255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_ACTIVE].blue * decrease)/65535.0)),
    // bg[SELECTED]
    (int)(255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_SELECTED].red * decrease)/65535.0)),
    (int)(255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_SELECTED].green * decrease)/65535.0)),
    (int)(255*CLIP ((_main_window_orginal_style->bg[GTK_STATE_SELECTED].blue * decrease)/65535.0)),
  
     // base[NORMAL]
    (int)(255*CLIP ((_main_window_orginal_style->base[GTK_STATE_NORMAL].red * decrease)/65535.0)),
    (int)(255*CLIP ((_main_window_orginal_style->base[GTK_STATE_NORMAL].green * decrease)/65535.0)),
    (int)(255*CLIP ((_main_window_orginal_style->base[GTK_STATE_NORMAL].blue * decrease)/65535.0)),
      // base[ACTIVE]
    (int)(255*CLIP ((_main_window_orginal_style->base[GTK_STATE_ACTIVE].red * decrease)/65535.0)),
    (int)(255*CLIP ((_main_window_orginal_style->base[GTK_STATE_ACTIVE].green * decrease)/65535.0)),
    (int)(255*CLIP ((_main_window_orginal_style->base[GTK_STATE_ACTIVE].blue * decrease)/65535.0)),
    
    /* clearlooks-brightbg */
    (int)(255*CLIP ((_module_orginal_style->bg[GTK_STATE_NORMAL].red * (1.0+(amount*0.1)) )/65535.0)),
    (int)(255*CLIP ((_module_orginal_style->bg[GTK_STATE_NORMAL].green * (1.0+(amount*0.1)) )/65535.0)),
    (int)(255*CLIP ((_module_orginal_style->bg[GTK_STATE_NORMAL].blue * (1.0+(amount*0.1)) )/65535.0))
    
    
  );
      
 // fprintf(stderr,"RC: %s\n",rc);  
  
  gtk_rc_parse_string (rc);
  
  //  GtkWidget *window = glade_xml_get_widget (darktable.gui->main_window, "main_window");
  gtk_rc_reset_styles (gtk_settings_get_default());
}

void 
dt_gui_contrast_init ()
{
  /* create a copy of orginal style of window */
  GtkWidget *window = glade_xml_get_widget (darktable.gui->main_window, "main_window");
  
  /* realize window to enshure style is applied before copy */
  gtk_widget_realize(window);
  _main_window_orginal_style = gtk_style_copy (window->style);
  
  
  /* get clearlooks-brightbg orginal style */
  window = glade_xml_get_widget (darktable.gui->main_window, "import_eventbox");
  gtk_widget_realize(window);
  _module_orginal_style = gtk_style_copy (window->style);
  
  
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

