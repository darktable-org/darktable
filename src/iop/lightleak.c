/*
    This file is part of darktable,
    Copyright (C) 2010-2024 darktable developers.

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
// our includes go first:
#include "bauhaus/bauhaus.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"

#include <gtk/gtk.h>
#include <stdlib.h>
#include <math.h>
#include <stddef.h>

// Silence the compiler during dev of new module as we often
// need to have temporary unfinished code that will hurt the
// compiler.
// THIS MUST be REMOVED before submitting a PR.
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-function"

DT_MODULE_INTROSPECTION(1, dt_iop_lightleak_params_t)

typedef struct dt_iop_lightleak_params_t
{
  float strength;     // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 50.0 $DESCRIPTION: "strength"
  float falloff;      // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 50.0 $DESCRIPTION: "falloff"
  float hue;          // $MIN: 0.0 $MAX: 360.0 $DEFAULT: 15 $DESCRIPTION: "hue"
  float sat;          // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 75 $DESCRIPTION: "saturation"
  float direction;    // $MIN: 0.0 $MAX: 360.0 $DEFAULT: 0 $DESCRIPTION: "direction (degrees)"
  float inset;        // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 0.0 $DESCRIPTION: "inset"
} dt_iop_lightleak_params_t;

typedef struct dt_iop_lightleak_gui_data_t
{
  GtkWidget *strength;
  GtkWidget *falloff;
  GtkWidget *hue;
  GtkWidget *sat;
  GtkWidget *direction;
  GtkWidget *inset;
} dt_iop_lightleak_gui_data_t;

const char *name()
{
  return _("light leak");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description
    (self,
     // first line is the general description for the module
     _("adds light leaks to the image"),
     // the goal:
     _("creative"),
     // the input:
     _("linear, RGB, scene-referred"),
     // the internal working:
     _("linear, RGB"),
     // the output:
     _("linear, RGB, scene-referred"));
}

// some additional flags (self explanatory i think):
int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_ALLOW_TILING;
}

// where does it appear in the gui?
int default_group()
{
  return IOP_GROUP_EFFECT;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  if(old_version == 1)
  {
    typedef struct dt_iop_lightleak_params_v1_t
    {
      float strength;
      float falloff;
      float hue;
      float sat;
      gboolean from_top;
      gboolean from_left;
    } dt_iop_lightleak_params_v1_t;

    const dt_iop_lightleak_params_v1_t *o = old_params;
    dt_iop_lightleak_params_t *n = malloc(sizeof(dt_iop_lightleak_params_t));
    *new_params = n;
    *new_params_size = sizeof(dt_iop_lightleak_params_t);
    *new_version = 2;

    n->strength = o->strength;
    n->falloff  = o->falloff;
    n->hue      = o->hue * 360.0f;   // convert 0–1 → 0–360
    n->sat      = o->sat * 100.0f;   // 0–1 → 0–100

    // Approximate old toggles → direction
    if(o->from_top && o->from_left)
      n->direction = 315.0f;  // top-left corner ≈ 315°
    else if(o->from_top)
      n->direction = 0.0f;    // top
    else if(o->from_left)
      n->direction = 270.0f;  // left
    else
      n->direction = 0.0f;    // default to top

    return 0;
  }

  return 1;  // unknown version
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  memcpy(piece->data, p1, self->params_size);
}

void hsv2rgb(const float *hsv, float *rgb)
{
  const float h = hsv[0] * 6.0f;
  const float s = hsv[1];
  const float v = hsv[2];

  if(s < 1e-6f)
  {
    rgb[0] = rgb[1] = rgb[2] = v;
    return;
  }

  const int i = (int)h;
  const float f = h - i;
  const float p = v * (1.0f - s);
  const float q = v * (1.0f - s * f);
  const float t = v * (1.0f - s * (1.0f - f));

  switch(i)
  {
    case 0: rgb[0] = v; rgb[1] = t; rgb[2] = p; break;
    case 1: rgb[0] = q; rgb[1] = v; rgb[2] = p; break;
    case 2: rgb[0] = p; rgb[1] = v; rgb[2] = t; break;
    case 3: rgb[0] = p; rgb[1] = q; rgb[2] = v; break;
    case 4: rgb[0] = t; rgb[1] = p; rgb[2] = v; break;
    default: rgb[0] = v; rgb[1] = p; rgb[2] = q; break;
  }
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  dt_iop_lightleak_params_t *d = (dt_iop_lightleak_params_t *)piece->data;

  if(!dt_iop_have_required_input_format(4, self, piece->colors, ivoid, ovoid, roi_in, roi_out))
    return;

  const int width  = roi_out->width;
  const int height = roi_out->height;

  // Direction in radians
  const float rad = d->direction * M_PI_F / 180.0f;
  const float cos_dir = cosf(rad);   // component along direction
  const float sin_dir = sinf(rad);

  // inset_norm: 0.0 = at edge, 1.0 = at center
  const float inset_norm = d->inset / 100.0f;

  // The "origin offset" — how far we push the zero-point inward
  // We push along the *opposite* of the direction vector (towards center from edge)
  const float offset = inset_norm * 0.5f;   // max offset = half image diagonal-ish, but 0.5 works well in normalized space

  DT_OMP_FOR()
  for(int y = 0; y < height; y++)
  {
    const float *in  = ((const float *)ivoid)  + (size_t)4 * width * y;
    float       *out = ((      float *)ovoid) + (size_t)4 * width * y;

    const float ny = (float)y / (float)(height - 1);   // 0..1

    for(int x = 0; x < width; x++)
    {
      const float nx = (float)x / (float)(width - 1);   // 0..1

      // Project every point onto the direction axis
      // → gives a signed position along the leak gradient axis
      float proj = (nx - 0.5f) * cos_dir + (ny - 0.5f) * sin_dir;

      // Shift the zero-point inward
      // When inset=0: zero at -0.5 → full strength starts at edge
      // When inset=100: zero at 0 → full strength through center
      proj += offset;

      // Now distance from the peak line (absolute value)
      float dist_from_peak = fabsf(proj);

      // contrib = how much leak — max=1 at the line, falls off on both sides
      float contrib = 1.0f - dist_from_peak / 0.707f;   // rough normalization (max proj distance ~0.707)
      contrib = fmaxf(0.0f, contrib);

      // Apply user-controlled falloff sharpness
      // falloff=0 → very soft / wide, falloff=100 → very sharp/narrow band
      float exponent = 1.0f + (d->falloff / 25.0f);     // 1.0 → linear-ish, up to ~5.0 → very sharp
      contrib = powf(contrib, exponent);

      const float opacity = contrib * (d->strength / 100.0f);

      float hsv[3] = { d->hue / 360.0f, d->sat / 100.0f, 1.25f };
      float leak[3];
      hsv2rgb(hsv, leak);

      for(int c = 0; c < 3; c++)
      {
        out[c] = in[c] + opacity * leak[c] * 0.7f;
      }

      out[3] = in[3];

      in  += 4;
      out += 4;
    }
  }
}

/** Optional init and cleanup */
void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);
}

void cleanup(dt_iop_module_t *self)
{
  dt_iop_default_cleanup(self);
  // Releases any memory allocated in init(module) Implement this
  // function explicitly if the module allocates additional memory
  // besides (default_)params.  this is rare.
}

#ifdef HAVE_OPENCL

typedef struct dt_iop_lightleak_global_data_t
{
  int kernel_lightleak;  // kernel ID
} dt_iop_lightleak_global_data_t;

void init_global(dt_iop_module_so_t *self)
{
  const int program = 40; // lightleak.cl, from programs.conf
  dt_iop_lightleak_global_data_t *gd = malloc(sizeof(dt_iop_lightleak_global_data_t));
  self->data = gd;
  gd->kernel_lightleak = dt_opencl_create_kernel(program, "lightleak");
}

void cleanup_global(dt_iop_module_so_t *self)
{
  dt_iop_lightleak_global_data_t *gd = self->data;
  if(gd)
  {
    dt_opencl_free_kernel(gd->kernel_lightleak);
    free(self->data);
    self->data = NULL;
  }
}

int process_cl(dt_iop_module_t *self,
               dt_dev_pixelpipe_iop_t *piece,
               cl_mem dev_in,
               cl_mem dev_out,
               const dt_iop_roi_t *const roi_in,
               const dt_iop_roi_t *const roi_out)
{
  if(!dt_iop_have_required_input_format(4, self, piece->colors, (void *)dev_in, (void *)dev_out, roi_in, roi_out))
  {
    return DT_OPENCL_PROCESS_CL;  // fallback to CPU
  }

  dt_iop_lightleak_global_data_t *gd = (dt_iop_lightleak_global_data_t *)self->global_data;
  dt_iop_lightleak_params_t *d = (dt_iop_lightleak_params_t *)piece->data;

  if(gd->kernel_lightleak < 0) return DT_OPENCL_PROCESS_CL;

  const int devid = piece->pipe->devid;
  const int width  = roi_out->width;
  const int height = roi_out->height;

  cl_int err = CL_SUCCESS;

  cl_mem dev_params = dt_opencl_copy_host_to_device_constant(devid, sizeof(dt_iop_lightleak_params_t), d);
  if(!dev_params)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_lightleak] failed to copy params to device\n");
    return DT_OPENCL_PROCESS_CL;
  }

  err = dt_opencl_enqueue_kernel_2d_args(
    devid, gd->kernel_lightleak,
    width, height,
    CLARG(dev_in),
    CLARG(dev_out),
    CLARG(dev_params),
    CLARG(width),
    CLARG(height)
  );

  dt_opencl_release_mem_object(dev_params);

  if(err != CL_SUCCESS)
  {
    dt_print(DT_DEBUG_OPENCL, "[opencl_lightleak] enqueue failed: %d\n", err);
    return DT_OPENCL_PROCESS_CL;
  }

  return err;  // return raw error code (0 = success)
}
#endif // HAVE_OPENCL

static void _paint_leak_hue_background(const dt_iop_module_t *self)
{
  dt_iop_lightleak_gui_data_t *g = self->gui_data;
  const dt_iop_lightleak_params_t *p = self->params;

  const int num_stops = DT_BAUHAUS_SLIDER_MAX_STOPS;

  for(int i = 0; i < num_stops; i++)
  {
    const float stop = (float)i / (float)(num_stops - 1);
    const float hue_norm = stop;  // 0.0 → 1.0 = full circle
    const float sat_norm = p->sat / 100.0f;
    const float val = 1.0f;

    float hsv[3] = { hue_norm, sat_norm, val };
    float rgb[3];
    hsv2rgb(hsv, rgb);

    dt_bauhaus_slider_set_stop(g->hue, stop, rgb[0], rgb[1], rgb[2]);
  }

  gtk_widget_queue_draw(g->hue);
}

void gui_changed(dt_iop_module_t *self, GtkWidget *w, void *previous)
{
  dt_iop_lightleak_gui_data_t *g = self->gui_data;

  // Repaint hue gradient when hue or sat changes
  if(!w || w == g->hue || w == g->sat)
  {
    _paint_leak_hue_background(self);
  }
}

/** gui setup and update, these are needed. */
void gui_update(dt_iop_module_t *self)
{
  dt_iop_lightleak_gui_data_t *g = self->gui_data;
  const dt_iop_lightleak_params_t *p = self->params;
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_lightleak_gui_data_t *g = IOP_GUI_ALLOC(lightleak);

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  // Strength
  g->strength = dt_bauhaus_slider_from_params(self, "strength");
  dt_bauhaus_slider_set_digits(g->strength, 1);
  dt_bauhaus_slider_set_format(g->strength, "%");
  gtk_widget_set_tooltip_text(g->strength, _("intensity of the light leak"));

  // Falloff
  g->falloff = dt_bauhaus_slider_from_params(self, "falloff");
  dt_bauhaus_slider_set_digits(g->falloff, 1);
  dt_bauhaus_slider_set_format(g->falloff, "%");
  gtk_widget_set_tooltip_text(g->falloff, _("higher = sharper fade from edge"));

  // Hue
  g->hue = dt_bauhaus_slider_from_params(self, "hue");
  dt_bauhaus_slider_set_digits(g->hue, 0);
  dt_bauhaus_slider_set_format(g->hue, "°");
  gtk_widget_set_tooltip_text(g->hue, _("hue of the light leak in degrees"));
  _paint_leak_hue_background(self);

  // Saturation
  g->sat = dt_bauhaus_slider_from_params(self, "sat");
  dt_bauhaus_slider_set_digits(g->sat, 1);
  dt_bauhaus_slider_set_format(g->sat, "%");
  gtk_widget_set_tooltip_text(g->sat, _("saturation of the light leak"));

  // Direction
  g->direction = dt_bauhaus_slider_from_params(self, "direction");
  dt_bauhaus_slider_set_digits(g->direction, 0);
  dt_bauhaus_slider_set_format(g->direction, "°");
  gtk_widget_set_tooltip_text(g->direction, _("central direction of the light leak (0° = top, 90° = right, etc.)"));

  // Inset (center offset)
  g->inset = dt_bauhaus_slider_from_params(self, "inset");
  dt_bauhaus_slider_set_digits(g->inset, 1);
  dt_bauhaus_slider_set_format(g->inset, "%");
  gtk_widget_set_tooltip_text(g->inset, _("how far from the edge the leak center is placed (0%% = at edge, 100%% = at image center)"));
}

void gui_cleanup(dt_iop_module_t *self)
{
  // This only needs to be provided if gui_init allocates any memory
  // or resources besides self->widget and gui_data_t.
}
