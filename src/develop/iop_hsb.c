#include "develop/iop_hsb.h"
#include "gui/gtk.h"
// #include "control/settings.h"
#include "control/control.h"
#include "develop/imageop.h"

#include <math.h>
#include <string.h>

void
dt_iop_gui_reset_hsb ()
{
  GtkWidget *widget;
  float hue, sat, bri;
  dt_dev_operation_t op;
  DT_CTL_GET_GLOBAL_STR(op, dev_op, 20);
  DT_CTL_GET_GLOBAL(hue, dev_op_params.f[0]);
  DT_CTL_GET_GLOBAL(sat, dev_op_params.f[1]);
  DT_CTL_GET_GLOBAL(bri, dev_op_params.f[2]);
  if(strncmp(op, "hsb", 20))
  {
    hue = sat = bri = 1.0;
    widget = glade_xml_get_widget (darktable.gui->main_window, "hsb_hue");
    gtk_range_set_value(GTK_RANGE(widget), hue);
    widget = glade_xml_get_widget (darktable.gui->main_window, "hsb_saturation");
    gtk_range_set_value(GTK_RANGE(widget), sat);
    widget = glade_xml_get_widget (darktable.gui->main_window, "hsb_brightness");
    gtk_range_set_value(GTK_RANGE(widget), bri);
  }
}

void
dt_iop_gui_init_hsb ()
{
  // hsb expander
  GtkWidget *widget;
  widget = glade_xml_get_widget (darktable.gui->main_window, "hsb_hue");
	g_signal_connect (G_OBJECT (widget), "value-changed",
                    G_CALLBACK (dt_iop_gui_callback_hsb), (gpointer)0);

  widget = glade_xml_get_widget (darktable.gui->main_window, "hsb_saturation");
	g_signal_connect (G_OBJECT (widget), "value-changed",
                    G_CALLBACK (dt_iop_gui_callback_hsb), (gpointer)1);

  widget = glade_xml_get_widget (darktable.gui->main_window, "hsb_brightness");
	g_signal_connect (G_OBJECT (widget), "value-changed",
                    G_CALLBACK (dt_iop_gui_callback_hsb), (gpointer)2);
}

void
dt_iop_gui_callback_hsb (GtkRange *range, gpointer user_data)
{
  if(darktable.gui->reset) return;
  // FIXME: is this block needed??
  dt_dev_operation_t op;
  DT_CTL_GET_GLOBAL_STR(op, dev_op, 20);
  if(strncmp(op, "hsb", 20))
  {
    DT_CTL_SET_GLOBAL(dev_op_params.f[0], 1.0);
    DT_CTL_SET_GLOBAL(dev_op_params.f[1], 1.0);
    DT_CTL_SET_GLOBAL(dev_op_params.f[2], 1.0);
    DT_CTL_SET_GLOBAL_STR(dev_op, "hsb", 20);
  }
  // FIXME: end.
  if(user_data == (gpointer)0)
  {
    float tmp = gtk_range_get_value(range);
    DT_CTL_SET_GLOBAL(dev_op_params.f[0], tmp);
  }
  else if(user_data == (gpointer)1)
  {
    float tmp = gtk_range_get_value(range);
    DT_CTL_SET_GLOBAL(dev_op_params.f[1], tmp);
  }
  else
  {
    float tmp = gtk_range_get_value(range);
    DT_CTL_SET_GLOBAL(dev_op_params.f[2], tmp);
  }
  dt_dev_add_history_item(darktable.develop, "hsb");
}

// helper functions:
void
dt_rgb_to_hsv_f(const float *rgb, float *hsv)
{
	float min, max, delta;
	min = fminf(rgb[dt_red], fminf(rgb[dt_green], rgb[dt_blue]));
	max = fmaxf(rgb[dt_red], fmaxf(rgb[dt_green], rgb[dt_blue]));
	hsv[2] = max;
	delta = max - min;

	if(max == 0) hsv[1] = hsv[0] = 0;
	else
  {
    hsv[1] = delta / max;
    float tmph;
    if (delta == 0) tmph = 0;
    else if(rgb[dt_red] == max)   tmph =     (rgb[dt_green] - rgb[dt_blue])  / delta; // between yellow & magenta
    else if(rgb[dt_green] == max) tmph = 2 + (rgb[dt_blue]  - rgb[dt_red])   / delta; // between cyan & yellow
    else                          tmph = 4 + (rgb[dt_red]   - rgb[dt_green]) / delta; // between magenta & cyan
    hsv[0] = tmph*60./360.;
  }
}

void
dt_hsv_to_rgb_f(const float *hsv, float *rgb)
{
  const float h = hsv[0]*6.0;
  const int i = (int)h;
  const float f = (h - i), s = hsv[1], v = hsv[2];
  const float p = v * (1. -  s);
	const float q = v * (1. - (s * f));
	const float t = v * (1. - (s * (1. - f )));
  switch(i)
  {
    case  0: rgb[dt_red] = v; rgb[dt_green] = t; rgb[dt_blue] = p; break;
    case  1: rgb[dt_red] = q; rgb[dt_green] = v; rgb[dt_blue] = p; break;
    case  2: rgb[dt_red] = p; rgb[dt_green] = v; rgb[dt_blue] = t; break;
    case  3: rgb[dt_red] = p; rgb[dt_green] = q; rgb[dt_blue] = v; break;
    case  4: rgb[dt_red] = t; rgb[dt_green] = p; rgb[dt_blue] = v; break;
    default: rgb[dt_red] = v; rgb[dt_green] = p; rgb[dt_blue] = q; break;
  }
}

#if 0
void dt_rgb_to_hsv_16(const uint16_t *rgb, uint16_t *hsv)
{
	uint64_t min, max, delta;
	min = MIN(rgb[red], MIN(rgb[green], rgb[blue]));
	max = MAX(rgb[red], MAX(rgb[green], rgb[blue]));
	hsv[2] = max;
	delta = max - min;

	if(max == 0) hsv[1] = hsv[0] = 0;
	else
  {
    hsv[1] = (0xffff * delta) / max;//I16CLAMP(delta / (float)max);
    double tmph;
    if (delta == 0) tmph = 0;
    else if(rgb[red] == max)   tmph =     (rgb[green] - rgb[blue])  / (double)delta; // between yellow & magenta
    else if(rgb[green] == max) tmph = 2 + (rgb[blue]  - rgb[red])   / (double)delta; // between cyan & yellow
    else                       tmph = 4 + (rgb[red]   - rgb[green]) / (double)delta; // between magenta & cyan
    hsv[0] = (uint16_t)(0x10000*tmph*60./360.); // rotation allowed
  }
}

void dt_hsv_to_rgb_16(const uint16_t *hsv, uint16_t *rgb)
{
  const double h = hsv[0]*6.0/0x10000;
  const int i = (int)h;
  const int64_t f = 0x10000*(h - i), s = hsv[1], v = hsv[2];
  const uint16_t p = v * (0x10000 -  s)/0x10000;
	const uint16_t q = v * (0x10000 - (s * f)/0x10000)/0x10000;
	const uint16_t t = v * (0x10000 - (s * ( 0x10000 - f ))/0x10000)/0x10000;
  switch(i)
  {
    case  0: rgb[red] = v; rgb[green] = t; rgb[blue] = p; break;
    case  1: rgb[red] = q; rgb[green] = v; rgb[blue] = p; break;
    case  2: rgb[red] = p; rgb[green] = v; rgb[blue] = t; break;
    case  3: rgb[red] = p; rgb[green] = q; rgb[blue] = v; break;
    case  4: rgb[red] = t; rgb[green] = p; rgb[blue] = v; break;
    default: rgb[red] = v; rgb[green] = p; rgb[blue] = q; break;
  }
}
#endif

// main op function: convert buffer applying hue saturation brightness changes.
void
dt_iop_execute_hsb(float *dst, const float *src, const int32_t wd, const int32_t ht, const int32_t bufwd, const int32_t bufht,
           dt_dev_operation_t operation, dt_dev_operation_params_t *params)
{
  float hsbmul[3];
  hsbmul[0] = params->f[0];
  hsbmul[1] = params->f[1];
  hsbmul[2] = params->f[2];
  float hsv[3];
  const int ht2 = MIN(ht, bufht);
  const int wd2 = MIN(wd, bufwd);
  int idx = 0;
  for(int j=0;j<ht2;j++)
  {
    for(int i=0;i<wd2;i++)
    {
      dt_rgb_to_hsv_f(src + 3*idx, hsv);
      for(int k=0;k<3;k++) hsv[k] = fminf(1.0, fmaxf(0.0f, hsv[k]*hsbmul[k]));
      dt_hsv_to_rgb_f(hsv, dst + 3*idx);
      idx++;
    }
    idx = bufwd*j;
  }
}

