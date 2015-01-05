/*
    This file is part of darktable,
    copyright (c) 2014 LebedevRI. (based on code by johannes hanika)

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
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "bauhaus/bauhaus.h"
#include "gui/gtk.h"
#include "gui/accelerators.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <xmmintrin.h>

DT_MODULE_INTROSPECTION(1, dt_iop_rawprepare_params_t)

typedef struct dt_iop_rawprepare_params_t
{
  int32_t x, y, width, height; // crop, now unused, for future expansion
  uint16_t raw_black_level_separate[4];
  uint16_t raw_white_point;
} dt_iop_rawprepare_params_t;

typedef struct dt_iop_rawprepare_gui_data_t
{
  GtkWidget *box_raw;
  GtkWidget *black_level_separate[4];
  GtkWidget *white_point;
  GtkWidget *label_non_raw;
} dt_iop_rawprepare_gui_data_t;

typedef struct dt_iop_rawprepare_data_t
{
  float sub[4];
  float div[4];
} dt_iop_rawprepare_data_t;

const char *name()
{
  return C_("modulename", "raw black/white point");
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_ONE_INSTANCE;
}

int groups()
{
  return IOP_GROUP_BASIC;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  for(int i = 0; i < 4; i++)
  {
    gchar *label = g_strdup_printf(_("black level %i"), i);
    dt_accel_register_slider_iop(self, FALSE, NC_("accel", label));
    g_free(label);
  }

  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "white point"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_rawprepare_gui_data_t *g = (dt_iop_rawprepare_gui_data_t *)self->gui_data;

  for(int i = 0; i < 4; i++)
  {
    gchar *label = g_strdup_printf(_("black level %i"), i);
    dt_accel_connect_slider_iop(self, label, g->black_level_separate[i]);
    g_free(label);
  }

  dt_accel_connect_slider_iop(self, "white point", GTK_WIDGET(g->white_point));
}

void tiling_callback(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *const roi_in,
                     const dt_iop_roi_t *const roi_out, dt_develop_tiling_t *tiling)
{
  float ioratio = (float)roi_out->width * roi_out->height / ((float)roi_in->width * roi_in->height);

  tiling->factor = 1.0f + ioratio; // in + out, no temp
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->xalign = 2; // Bayer pattern
  tiling->yalign = 2; // Bayer pattern
  return;
}

int output_bpp(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  if(!dt_dev_pixelpipe_uses_downsampled_input(pipe) && (pipe->image.flags & DT_IMAGE_RAW))
    return sizeof(float);
  else
    return 4 * sizeof(float);
}

static int BL(const dt_iop_roi_t *const roi_out, const dt_iop_rawprepare_data_t *const d, const int row,
              const int col)
{
  return ((((row + roi_out->y) & 1) << 1) + ((col + roi_out->x) & 1));
}

void process(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid, void *ovoid,
             const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  const dt_iop_rawprepare_data_t *const d = (dt_iop_rawprepare_data_t *)piece->data;

  if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && dt_image_filter(&piece->pipe->image))
  { // raw mosaic
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(ovoid)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      const float *in = ((float *)ivoid) + (size_t)roi_in->width * j;
      float *out = ((float *)ovoid) + (size_t)roi_out->width * j;

      int i = 0;
      int alignment = ((4 - (j * roi_out->width & (4 - 1))) & (4 - 1));

      // process unaligned pixels
      for(; i < alignment; i++, out++, in++)
      {
        const int id = BL(roi_out, d, j, i);
        *out = MAX(0.0f, (*in - d->sub[id]) / d->div[id]);
      }

      const __m128 sub = _mm_set_ps(d->sub[BL(roi_out, d, j, i + 3)], d->sub[BL(roi_out, d, j, i + 2)],
                                    d->sub[BL(roi_out, d, j, i + 1)], d->sub[BL(roi_out, d, j, i)]);

      const __m128 div = _mm_set_ps(d->div[BL(roi_out, d, j, i + 3)], d->div[BL(roi_out, d, j, i + 2)],
                                    d->div[BL(roi_out, d, j, i + 1)], d->div[BL(roi_out, d, j, i)]);

      // process aligned pixels with SSE
      for(; i < roi_out->width - (4 - 1); i += 4, in += 4, out += 4)
      {
        const __m128 input = _mm_load_ps(in);

        const __m128 scaled = _mm_div_ps(_mm_sub_ps(input, sub), div);

        _mm_stream_ps(out, _mm_max_ps(_mm_setzero_ps(), scaled));
      }

      // process the rest
      for(; i < roi_out->width; i++, in++, out++)
      {
        const int id = BL(roi_out, d, j, i);
        *out = MAX(0.0f, (*in - d->sub[id]) / d->div[id]);
      }
    }
  }
  else
  { // pre-downsampled buffer that needs black/white scaling
#ifdef _OPENMP
#pragma omp parallel for default(none) schedule(static) shared(ovoid)
#endif
    for(int j = 0; j < roi_out->height; j++)
    {
      const float *in = ((float *)ivoid) + (size_t)4 * roi_in->width * j;
      float *out = ((float *)ovoid) + (size_t)4 * roi_out->width * j;

      for(int i = 0; i < roi_out->width; i++, in += 4, out += 4)
      {
        for(int c = 0; c < 3; c++)
        {
          out[c] = MAX(0.0f, (in[c] - d->sub[0]) / d->div[0]);
        }
      }
    }
  }
  _mm_sfence();
}

void commit_params(dt_iop_module_t *self, const dt_iop_params_t *const params, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  const dt_iop_rawprepare_params_t *const p = (dt_iop_rawprepare_params_t *)params;
  dt_iop_rawprepare_data_t *d = (dt_iop_rawprepare_data_t *)piece->data;

  const float white = (float)p->raw_white_point / (float)UINT16_MAX;

  if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && dt_image_filter(&piece->pipe->image))
  {
    for(int i = 0; i < 4; i++)
    {
      d->sub[i] = (float)p->raw_black_level_separate[i] / (float)UINT16_MAX;
      d->div[i] = (white - d->sub[i]);
    }
  }
  else
  {
    float black = 0;
    for(int i = 0; i < 4; i++)
    {
      black += p->raw_black_level_separate[i] / (float)UINT16_MAX;
    }
    black /= 4.0f;

    for(int i = 0; i < 4; i++)
    {
      d->sub[i] = black;
      d->div[i] = (white - black);
    }
  }

  if(!dt_image_is_raw(&piece->pipe->image) || piece->pipe->image.bpp == sizeof(float)) piece->enabled = 0;
}

void init_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = calloc(1, sizeof(dt_iop_rawprepare_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void reload_defaults(dt_iop_module_t *self)
{
  dt_iop_rawprepare_params_t tmp = { 0 };

  // we might be called from presets update infrastructure => there is no image
  if(!self->dev) goto end;

  const dt_image_t *const image = &(self->dev->image_storage);

  tmp = (dt_iop_rawprepare_params_t){.raw_black_level_separate[0] = image->raw_black_level_separate[0],
                                     .raw_black_level_separate[1] = image->raw_black_level_separate[1],
                                     .raw_black_level_separate[2] = image->raw_black_level_separate[2],
                                     .raw_black_level_separate[3] = image->raw_black_level_separate[3],
                                     .raw_white_point = image->raw_white_point };

  self->default_enabled = dt_image_is_raw(image) && image->bpp != sizeof(float);

end:
  memcpy(self->params, &tmp, sizeof(dt_iop_rawprepare_params_t));
  memcpy(self->default_params, &tmp, sizeof(dt_iop_rawprepare_params_t));
}

void init(dt_iop_module_t *self)
{
  const dt_image_t *const image = &(self->dev->image_storage);

  self->params = calloc(1, sizeof(dt_iop_rawprepare_params_t));
  self->default_params = calloc(1, sizeof(dt_iop_rawprepare_params_t));
  self->default_enabled = dt_image_is_raw(image) && image->bpp != sizeof(float);
  self->priority = 10; // module order created by iop_dependencies.py, do not edit!
  self->params_size = sizeof(dt_iop_rawprepare_params_t);
  self->gui_data = NULL;
}

void cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
  free(self->params);
  self->params = NULL;
}

void gui_update(dt_iop_module_t *self)
{
  dt_iop_rawprepare_gui_data_t *g = (dt_iop_rawprepare_gui_data_t *)self->gui_data;
  dt_iop_rawprepare_params_t *p = (dt_iop_rawprepare_params_t *)self->params;

  for(int i = 0; i < 4; i++)
  {
    dt_bauhaus_slider_set_soft(g->black_level_separate[i], p->raw_black_level_separate[i]);
    dt_bauhaus_slider_set_default(g->black_level_separate[i], p->raw_black_level_separate[i]);
  }

  dt_bauhaus_slider_set_soft(g->white_point, p->raw_white_point);
  dt_bauhaus_slider_set_default(g->white_point, p->raw_white_point);

  if(self->default_enabled)
  {
    gtk_widget_show(g->box_raw);
    gtk_widget_hide(g->label_non_raw);
  }
  else
  {
    gtk_widget_hide(g->box_raw);
    gtk_widget_show(g->label_non_raw);
  }
}

static void callback(GtkWidget *widget, gpointer *user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;

  dt_iop_rawprepare_gui_data_t *g = (dt_iop_rawprepare_gui_data_t *)self->gui_data;
  dt_iop_rawprepare_params_t *p = (dt_iop_rawprepare_params_t *)self->params;

  for(int i = 0; i < 4; i++)
    p->raw_black_level_separate[i] = dt_bauhaus_slider_get(g->black_level_separate[i]);
  p->raw_white_point = dt_bauhaus_slider_get(g->white_point);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void gui_init(dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_rawprepare_gui_data_t));

  dt_iop_rawprepare_gui_data_t *g = (dt_iop_rawprepare_gui_data_t *)self->gui_data;
  dt_iop_rawprepare_params_t *p = (dt_iop_rawprepare_params_t *)self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  g->box_raw = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  for(int i = 0; i < 4; i++)
  {
    gchar *label = g_strdup_printf(_("black level %i"), i);

    g->black_level_separate[i]
        = dt_bauhaus_slider_new_with_range(self, 0, 16384, 1, p->raw_black_level_separate[i], 0);
    dt_bauhaus_widget_set_label(g->black_level_separate[i], NULL, label);
    g_object_set(G_OBJECT(g->black_level_separate[i]), "tooltip-text", label, (char *)NULL);
    gtk_box_pack_start(GTK_BOX(g->box_raw), g->black_level_separate[i], FALSE, FALSE, 0);
    dt_bauhaus_slider_enable_soft_boundaries(g->black_level_separate[i], 0, UINT16_MAX);
    g_signal_connect(G_OBJECT(g->black_level_separate[i]), "value-changed", G_CALLBACK(callback), self);

    g_free(label);
  }

  g->white_point = dt_bauhaus_slider_new_with_range(self, 0, 16384, 1, p->raw_white_point, 0);
  dt_bauhaus_widget_set_label(g->white_point, NULL, _("white point"));
  g_object_set(G_OBJECT(g->white_point), "tooltip-text", _("white point"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(g->box_raw), g->white_point, FALSE, FALSE, 0);
  dt_bauhaus_slider_enable_soft_boundaries(g->white_point, 0, UINT16_MAX);
  g_signal_connect(G_OBJECT(g->white_point), "value-changed", G_CALLBACK(callback), self);

  gtk_box_pack_start(GTK_BOX(self->widget), g->box_raw, FALSE, FALSE, 0);

  g->label_non_raw
      = gtk_label_new(_("raw black/white point correction\nonly works for the sensors that need it."));
  gtk_widget_set_halign(g->label_non_raw, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(self->widget), g->label_non_raw, FALSE, FALSE, 0);
}

void gui_cleanup(dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
