/*
    This file is part of darktable,
    copyright (c) 2011 -- 2014 Tobias Ellinghaus, johannes hanika, henrik andersson.

    and the initial plugin `stuck pixels' was
    copyright (c) 2011 bruce guenter

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <gtk/gtk.h>
#include <stdlib.h>
#include <xmmintrin.h>
#include "control/control.h"
#include "develop/imageop.h"
#include "dtgtk/resetlabel.h"
#include "dtgtk/button.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"

DT_MODULE_INTROSPECTION(1, dt_iop_invert_params_t)

typedef struct dt_iop_invert_params_t
{
  float color[3]; // color of film material
} dt_iop_invert_params_t;

typedef struct dt_iop_invert_gui_data_t
{
  GtkWidget *colorpicker;
  GtkDarktableResetLabel *label;
  GtkBox *pickerbuttons;
  GtkWidget *picker;
} dt_iop_invert_gui_data_t;

typedef struct dt_iop_invert_global_data_t
{
  int kernel_invert_1f;
  int kernel_invert_4f;
} dt_iop_invert_global_data_t;

typedef struct dt_iop_invert_params_t dt_iop_invert_data_t;

const char *name()
{
  return _("invert");
}

int groups()
{
  return IOP_GROUP_BASIC;
}

int flags()
{
  return IOP_FLAGS_ONE_INSTANCE;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_iop(self, FALSE, NC_("accel", "pick color of film material from image"), 0, 0);
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_invert_gui_data_t *g = (dt_iop_invert_gui_data_t *)self->gui_data;

  dt_accel_connect_button_iop(self, "pick color of film material from image", GTK_WIDGET(g->colorpicker));
}

int output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  if(!dt_dev_pixelpipe_uses_downsampled_input(pipe) && (pipe->image.flags & DT_IMAGE_RAW))
    return sizeof(float);
  return 4 * sizeof(float);
}

static void request_pick_toggled(GtkToggleButton *togglebutton, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;

  self->request_color_pick
      = (gtk_toggle_button_get_active(togglebutton) ? DT_REQUEST_COLORPICK_MODULE : DT_REQUEST_COLORPICK_OFF);

  if(self->request_color_pick != DT_REQUEST_COLORPICK_OFF)
  {
    dt_lib_colorpicker_set_area(darktable.lib, 0.99);
    dt_dev_reprocess_all(self->dev);
  }
  else
    dt_control_queue_redraw();

  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  dt_iop_request_focus(self);
}

static gboolean draw(GtkWidget *widget, cairo_t *cr, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return FALSE;
  if(self->picked_color_max[0] < 0.0f) return FALSE;
  if(self->request_color_pick == DT_REQUEST_COLORPICK_OFF) return FALSE;
  dt_iop_invert_gui_data_t *g = (dt_iop_invert_gui_data_t *)self->gui_data;
  dt_iop_invert_params_t *p = (dt_iop_invert_params_t *)self->params;

  if(fabsf(p->color[0] - self->picked_color[0]) < 0.0001f
     && fabsf(p->color[1] - self->picked_color[1]) < 0.0001f
     && fabsf(p->color[2] - self->picked_color[2]) < 0.0001f)
  {
    // interrupt infinite loops
    return FALSE;
  }

  p->color[0] = self->picked_color[0];
  p->color[1] = self->picked_color[1];
  p->color[2] = self->picked_color[2];
  GdkRGBA color = (GdkRGBA){.red = p->color[0], .green = p->color[1], .blue = p->color[2], .alpha = 1.0 };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->colorpicker), &color);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
  return FALSE;
}

static void colorpicker_callback(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(self->dt->gui->reset) return;
  dt_iop_invert_gui_data_t *g = (dt_iop_invert_gui_data_t *)self->gui_data;
  dt_iop_invert_params_t *p = (dt_iop_invert_params_t *)self->params;

  // turn off the other color picker so that this tool actually works ...
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->picker), FALSE);

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  p->color[0] = c.red;
  p->color[1] = c.green;
  p->color[2] = c.blue;

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static int FC(const int row, const int col, const unsigned int filters)
{
  return filters >> (((row << 1 & 14) + (col & 1)) << 1) & 3;
}

static uint8_t FCxtrans(const int row, const int col, const dt_iop_roi_t *const roi,
                        uint8_t (*const xtrans)[6])
{
  return xtrans[(row + roi->y) % 6][(col + roi->x) % 6];
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid,
             const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_invert_data_t *d = (dt_iop_invert_data_t *)piece->data;

  const float *const m = piece->pipe->processed_maximum;

  const float film_rgb[3] = { d->color[0], d->color[1], d->color[2] };
  const float film_rgb_f[3] = { d->color[0] * m[0], d->color[1] * m[1], d->color[2] * m[2] };

  // FIXME: it could be wise to make this a NOP when picking colors. not sure about that though.
  //   if(self->request_color_pick){
  // do nothing
  //   }

  const int filters = dt_image_filter(&piece->pipe->image);
  uint8_t (*const xtrans)[6] = self->dev->image_storage.xtrans;

  if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && (filters == 9u))
  { // xtrans float mosaiced
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid) schedule(static)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      const float *in = ((float *)ivoid) + (size_t)j * roi_out->width;
      float *out = ((float *)ovoid) + (size_t)j * roi_out->width;
      for(int i = 0; i < roi_out->width; i++, out++, in++)
        *out = CLAMP(film_rgb_f[FCxtrans(j, i, roi_out, xtrans)] - *in, 0.0f, 1.0f);
    }

    for(int k = 0; k < 3; k++) piece->pipe->processed_maximum[k] = 1.0f;
  }
  else if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && filters)
  { // bayer float mosaiced

    const __m128 val_min = _mm_setzero_ps();
    const __m128 val_max = _mm_set1_ps(1.0f);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid) schedule(static)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      const float *in = ((float *)ivoid) + (size_t)j * roi_out->width;
      float *out = ((float *)ovoid) + (size_t)j * roi_out->width;

      int i = 0;
      int alignment = ((4 - (j * roi_out->width & (4 - 1))) & (4 - 1));

      // process unaligned pixels
      for(; i < alignment; i++, out++, in++)
        *out = CLAMP(film_rgb_f[FC(j + roi_out->y, i + roi_out->x, filters)] - *in, 0.0f, 1.0f);

      const __m128 film = _mm_set_ps(film_rgb_f[FC(j + roi_out->y, roi_out->x + i + 3, filters)],
                                     film_rgb_f[FC(j + roi_out->y, roi_out->x + i + 2, filters)],
                                     film_rgb_f[FC(j + roi_out->y, roi_out->x + i + 1, filters)],
                                     film_rgb_f[FC(j + roi_out->y, roi_out->x + i, filters)]);

      // process aligned pixels with SSE
      for(; i < roi_out->width - (4 - 1); i += 4, in += 4, out += 4)
      {
        const __m128 input = _mm_load_ps(in);
        const __m128 subtracted = _mm_sub_ps(film, input);
        _mm_stream_ps(out, _mm_max_ps(_mm_min_ps(subtracted, val_max), val_min));
      }

      // process the rest
      for(; i < roi_out->width; i++, out++, in++)
        *out = CLAMP(film_rgb_f[FC(j + roi_out->y, i + roi_out->x, filters)] - *in, 0.0f, 1.0f);
    }
    _mm_sfence();

    for(int k = 0; k < 3; k++) piece->pipe->processed_maximum[k] = 1.0f;
  }
  else
  { // non-mosaiced
    const int ch = piece->colors;

    const __m128 film = _mm_set_ps(1.0f, film_rgb[2], film_rgb[1], film_rgb[0]);

#ifdef _OPENMP
#pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid) schedule(static)
#endif
    for(int k = 0; k < roi_out->height; k++)
    {
      const float *in = ((float *)ivoid) + (size_t)ch * k * roi_out->width;
      float *out = ((float *)ovoid) + (size_t)ch * k * roi_out->width;
      for(int j = 0; j < roi_out->width; j++, in += ch, out += ch)
      {
        const __m128 input = _mm_load_ps(in);
        const __m128 subtracted = _mm_sub_ps(film, input);
        _mm_stream_ps(out, subtracted);
      }
    }
    _mm_sfence();

    if(piece->pipe->mask_display) dt_iop_alpha_copy(ivoid, ovoid, roi_out->width, roi_out->height);
  }
}

#ifdef HAVE_OPENCL
int process_cl(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out,
               const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_invert_data_t *d = (dt_iop_invert_data_t *)piece->data;
  dt_iop_invert_global_data_t *gd = (dt_iop_invert_global_data_t *)self->data;

  const int devid = piece->pipe->devid;
  const int filters = dt_image_filter(&piece->pipe->image);
  cl_mem dev_color = NULL;
  cl_int err = -999;
  int kernel = -1;

  if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && filters)
  {
    kernel = gd->kernel_invert_1f;
  }
  else
  {
    kernel = gd->kernel_invert_4f;
  }

  dev_color = dt_opencl_copy_host_to_device_constant(devid, sizeof(float) * 3, d->color);
  if(dev_color == NULL) goto error;

  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1 };
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), (void *)&dev_color);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(uint32_t), (void *)&filters);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(uint32_t), (void *)&roi_out->x);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(uint32_t), (void *)&roi_out->y);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_color);
  for(int k = 0; k < 3; k++) piece->pipe->processed_maximum[k] = 1.0f;
  return TRUE;

error:
  if(dev_color != NULL) dt_opencl_release_mem_object(dev_color);
  dt_print(DT_DEBUG_OPENCL, "[opencl_invert] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_invert_params_t tmp = (dt_iop_invert_params_t){ { 1.0f, 1.0f, 1.0f } };
  memcpy(self->params, &tmp, sizeof(dt_iop_invert_params_t));
  memcpy(self->default_params, &tmp, sizeof(dt_iop_invert_params_t));

  self->default_enabled = 0;
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  module->data = malloc(sizeof(dt_iop_invert_global_data_t));

  dt_iop_invert_global_data_t *gd = module->data;
  gd->kernel_invert_1f = dt_opencl_create_kernel(program, "invert_1f");
  gd->kernel_invert_4f = dt_opencl_create_kernel(program, "invert_4f");
}

void init(dt_iop_module_t *module)
{
  module->params = g_malloc0(sizeof(dt_iop_invert_params_t));
  module->default_params = g_malloc0(sizeof(dt_iop_invert_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_invert_params_t);
  module->gui_data = NULL;
  module->priority = 33; // module order created by iop_dependencies.py, do not edit!
}

void cleanup(dt_iop_module_t *module)
{
  g_free(module->gui_data);
  module->gui_data = NULL;
  g_free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_invert_global_data_t *gd = (dt_iop_invert_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_invert_4f);
  dt_opencl_free_kernel(gd->kernel_invert_1f);
  free(module->data);
  module->data = NULL;
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_invert_params_t *p = (dt_iop_invert_params_t *)params;
  dt_iop_invert_data_t *d = (dt_iop_invert_data_t *)piece->data;
  memcpy(d, p, sizeof(dt_iop_invert_params_t));

  // x-trans images not implemented in OpenCL yet
  if(pipe->image.filters == 9u) piece->process_cl_ready = 0;
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = g_malloc0(sizeof(dt_iop_invert_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  g_free(piece->data);
  piece->data = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  self->request_color_pick = DT_REQUEST_COLORPICK_OFF;

  dt_iop_invert_gui_data_t *g = (dt_iop_invert_gui_data_t *)self->gui_data;
  dt_iop_invert_params_t *p = (dt_iop_invert_params_t *)self->params;

  gtk_widget_set_visible(GTK_WIDGET(g->pickerbuttons), TRUE);
  dtgtk_reset_label_set_text(g->label, _("color of film material"));

  GdkRGBA color = (GdkRGBA){.red = p->color[0], .green = p->color[1], .blue = p->color[2], .alpha = 1.0 };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->colorpicker), &color);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = g_malloc0(sizeof(dt_iop_invert_gui_data_t));
  dt_iop_invert_gui_data_t *g = (dt_iop_invert_gui_data_t *)self->gui_data;
  dt_iop_invert_params_t *p = (dt_iop_invert_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);

  g->label = DTGTK_RESET_LABEL(dtgtk_reset_label_new("", self, &p->color, 3 * sizeof(float)));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->label), TRUE, TRUE, 0);

  g->pickerbuttons = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->pickerbuttons), TRUE, TRUE, 0);

  GdkRGBA color = (GdkRGBA){.red = p->color[0], .green = p->color[1], .blue = p->color[2], .alpha = 1.0 };
  g->colorpicker = gtk_color_button_new_with_rgba(&color);
  gtk_widget_set_size_request(GTK_WIDGET(g->colorpicker), DT_PIXEL_APPLY_DPI(75), DT_PIXEL_APPLY_DPI(24));
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->colorpicker), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->colorpicker), _("select color of film material"));
  g_signal_connect(G_OBJECT(g->colorpicker), "color-set", G_CALLBACK(colorpicker_callback), self);
  gtk_box_pack_start(GTK_BOX(g->pickerbuttons), GTK_WIDGET(g->colorpicker), TRUE, TRUE, 0);

  g->picker = dtgtk_togglebutton_new(dtgtk_cairo_paint_colorpicker, CPF_STYLE_FLAT);
  g_object_set(G_OBJECT(g->picker), "tooltip-text", _("pick color of film material from image"), (char *)NULL);
  gtk_widget_set_size_request(g->picker, DT_PIXEL_APPLY_DPI(24), DT_PIXEL_APPLY_DPI(24));
  g_signal_connect(G_OBJECT(g->picker), "toggled", G_CALLBACK(request_pick_toggled), self);
  gtk_box_pack_start(GTK_BOX(g->pickerbuttons), g->picker, TRUE, TRUE, 5);

  g_signal_connect(G_OBJECT(self->widget), "draw", G_CALLBACK(draw), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  g_free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
