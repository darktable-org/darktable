
/*
 *
 * This file is an image processing operation for GEGL
 *
 * GEGL is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * GEGL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GEGL; if not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright 2006 Øyvind Kolås <pippin@gimp.org>
 *
 */
#include "config.h"
#include <glib/gi18n-lib.h>


#ifdef GEGL_CHANT_PROPERTIES

gegl_chant_double (gamma, _("Gamma"), -G_MAXDOUBLE, G_MAXDOUBLE, .45, _("gamma value"))
gegl_chant_double (linear, _("Linear"), -G_MAXDOUBLE, G_MAXDOUBLE, .1, _("linear value"))

#else

#define GEGL_CHANT_TYPE_POINT_COMPOSER
#define GEGL_CHANT_C_FILE       "dt-gamma.c"

#include "gegl-chant.h"

#include <math.h>
#ifdef _MSC_VER
#define powf(a,b) ((gfloat)pow(a,b))
#endif


static void prepare (GeglOperation *operation)
{
  Babl *format = babl_format ("RGB float");

  gegl_operation_set_format (operation, "input", format);
  gegl_operation_set_format (operation, "output", format);
}

static gboolean
process (GeglOperation        *op,
          void                *in_buf,
          void                *out_buf,
          glong                n_pixels,
          const GeglRectangle *roi)
{
  gfloat *in = in_buf;
  gfloat *out = out_buf;
  gint    i;

  gfloat gamma  = GEGL_CHANT_PROPERTIES (op)->gamma;
  gfloat linear = GEGL_CHANT_PROPERTIES (op)->linear;
  for (i=0; i<n_pixels; i++)
  {
    gfloat a, b, c, g;
    if(linear<1.0)
    {
      g = gamma*(1.0-linear)/(1.0-gamma*linear);
      a = 1.0/(1.0+linear*(g-1));
      b = linear*(g-1)*a;
      c = powf(a*linear+b, g)/linear;
    }
    else
    {
      a = b = g = 0.0;
      c = 1.0;
    }

    gint   j;
    gfloat col;
    for (j=0; j<3; j++)
    {
      col = in[j];
      col = fminf(powf(a*col+b, g), 1.0);
      out[j] = col;
    }
    in += 3;
    out+= 3;
  }
  return TRUE;
}

static void
gegl_chant_class_init (GeglChantClass *klass)
{
  GeglOperationClass              *operation_class;
  GeglOperationPointComposerClass *point_composer_class;

  operation_class  = GEGL_OPERATION_CLASS (klass);
  point_composer_class     = GEGL_OPERATION_POINT_COMPOSER_CLASS (klass);

  point_composer_class->process = process;
  operation_class->prepare = prepare;

  operation_class->name        = "gegl:dt-gamma";
  operation_class->categories  = "compositors:math";
  operation_class->description =
       _("Linear/Gamma conversion curve");
}
#endif
