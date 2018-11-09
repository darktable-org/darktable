/*
    This file is part of darktable,
    copyright (c) 2010 Henrik Andersson.

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

#include "bauhaus/bauhaus.h"
#include "common/tags.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "dtgtk/button.h"
#include "dtgtk/resetlabel.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include "common/iop_group.h"
#include <assert.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <librsvg/rsvg.h>
// ugh, ugly hack. why do people break stuff all the time?
#ifndef RSVG_CAIRO_H
#include <librsvg/rsvg-cairo.h>
#endif

#include "common/file_location.h"
#include "common/metadata.h"
#include "common/utility.h"

#define CLIP(x) ((x < 0) ? 0.0 : (x > 1.0) ? 1.0 : x)
DT_MODULE_INTROSPECTION(4, dt_iop_watermark_params_t)

// gchar *checksum = g_compute_checksum_for_data(G_CHECKSUM_MD5,data,length);

typedef enum dt_iop_watermark_base_scale_t
{
  DT_SCALE_IMAGE = 0,
  DT_SCALE_LARGER_BORDER = 1,
  DT_SCALE_SMALLER_BORDER = 2
} dt_iop_watermark_base_scale_t;

typedef struct dt_iop_watermark_params_t
{
  /** opacity value of rendering watermark */
  float opacity;
  /** scale value of rendering watermark */
  float scale;
  /** Pixel independent xoffset, 0 to 1 */
  float xoffset;
  /** Pixel independent yoffset, 0 to 1 */
  float yoffset;
  /** Alignment value 0-8 3x3 */
  int alignment;
  /** Rotation **/
  float rotate;
  dt_iop_watermark_base_scale_t sizeto;
  char filename[64];
  /* simple text */
  char text[64];
  /* text color */
  float color[3];
  /* text font */
  char font[64];
} dt_iop_watermark_params_t;

typedef struct dt_iop_watermark_data_t
{
  float opacity;
  float scale;
  float xoffset;
  float yoffset;
  int alignment;
  float rotate;
  dt_iop_watermark_base_scale_t sizeto;
  char filename[64];
  char text[64];
  float color[3];
  char font[64];
} dt_iop_watermark_data_t;

typedef struct dt_iop_watermark_gui_data_t
{
  GtkWidget *watermarks;                             // watermark
  GList     *watermarks_filenames;                   // the actual filenames. the dropdown lacks file extensions
  GtkWidget *refresh;                                // refresh watermarks...
  GtkWidget *align[9];                               // Alignment buttons
  GtkWidget *opacity, *scale, *x_offset, *y_offset;  // opacity, scale, xoffs, yoffs
  GtkWidget *sizeto;                                 // relative size to
  GtkWidget *rotate;
  GtkWidget *text;
  GtkWidget *colorpick;
  GtkWidget *fontsel;
} dt_iop_watermark_gui_data_t;

int legacy_params(dt_iop_module_t *self, const void *const old_params, const int old_version,
                  void *new_params, const int new_version)
{
  if(old_version == 1 && new_version == 4)
  {
    typedef struct dt_iop_watermark_params_v1_t
    {
      /** opacity value of rendering watermark */
      float opacity;
      /** scale value of rendering watermark */
      float scale;
      /** Pixel independent xoffset, 0 to 1 */
      float xoffset;
      /** Pixel independent yoffset, 0 to 1 */
      float yoffset;
      /** Alignment value 0-8 3x3 */
      int alignment;
      char filename[64];
    } dt_iop_watermark_params_v1_t;

    dt_iop_watermark_params_v1_t *o = (dt_iop_watermark_params_v1_t *)old_params;
    dt_iop_watermark_params_t *n = (dt_iop_watermark_params_t *)new_params;
    dt_iop_watermark_params_t *d = (dt_iop_watermark_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->opacity = o->opacity;
    n->scale = o->scale;
    n->xoffset = o->xoffset;
    n->yoffset = o->yoffset;
    n->alignment = o->alignment;
    n->rotate = 0.0;
    n->sizeto = DT_SCALE_IMAGE;
    g_strlcpy(n->filename, o->filename, sizeof(n->filename));
    g_strlcpy(n->text, "", sizeof(n->text));
    g_strlcpy(n->font, "DejaVu Sans 10", sizeof(n->font));
    n->color[0] = n->color[1] = n->color[2] = 0;

    return 0;
  }
  else if(old_version == 2 && new_version == 4)
  {
    typedef struct dt_iop_watermark_params_v2_t
    {
      /** opacity value of rendering watermark */
      float opacity;
      /** scale value of rendering watermark */
      float scale;
      /** Pixel independent xoffset, 0 to 1 */
      float xoffset;
      /** Pixel independent yoffset, 0 to 1 */
      float yoffset;
      /** Alignment value 0-8 3x3 */
      int alignment;
      dt_iop_watermark_base_scale_t sizeto;
      char filename[64];
    } dt_iop_watermark_params_v2_t;

    dt_iop_watermark_params_v2_t *o = (dt_iop_watermark_params_v2_t *)old_params;
    dt_iop_watermark_params_t *n = (dt_iop_watermark_params_t *)new_params;
    dt_iop_watermark_params_t *d = (dt_iop_watermark_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->opacity = o->opacity;
    n->scale = o->scale;
    n->xoffset = o->xoffset;
    n->yoffset = o->yoffset;
    n->alignment = o->alignment;
    n->rotate = 0.0;
    n->sizeto = DT_SCALE_IMAGE;
    g_strlcpy(n->filename, o->filename, sizeof(n->filename));
    g_strlcpy(n->text, "", sizeof(n->text));
    g_strlcpy(n->font, "DejaVu Sans 10", sizeof(n->font));
    n->color[0] = n->color[1] = n->color[2] = 0;
    return 0;
  }
  else if(old_version == 3 && new_version == 4)
  {
    typedef struct dt_iop_watermark_params_v3_t
    {
      /** opacity value of rendering watermark */
      float opacity;
      /** scale value of rendering watermark */
      float scale;
      /** Pixel independent xoffset, 0 to 1 */
      float xoffset;
      /** Pixel independent yoffset, 0 to 1 */
      float yoffset;
      /** Alignment value 0-8 3x3 */
      int alignment;
      /** Rotation **/
      float rotate;
      dt_iop_watermark_base_scale_t sizeto;
      char filename[64];
    } dt_iop_watermark_params_v3_t;

    dt_iop_watermark_params_v3_t *o = (dt_iop_watermark_params_v3_t *)old_params;
    dt_iop_watermark_params_t *n = (dt_iop_watermark_params_t *)new_params;
    dt_iop_watermark_params_t *d = (dt_iop_watermark_params_t *)self->default_params;

    *n = *d; // start with a fresh copy of default parameters

    n->opacity = o->opacity;
    n->scale = o->scale;
    n->xoffset = o->xoffset;
    n->yoffset = o->yoffset;
    n->alignment = o->alignment;
    n->rotate = o->rotate;
    n->sizeto = o->sizeto;
    g_strlcpy(n->filename, o->filename, sizeof(n->filename));
    g_strlcpy(n->text, "", sizeof(n->text));
    g_strlcpy(n->font, "DejaVu Sans 10", sizeof(n->font));
    n->color[0] = n->color[1] = n->color[2] = 0;
    return 0;
  }
  return 1;
}


const char *name()
{
  return _("watermark");
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int groups()
{
  return dt_iop_get_group("watermark", IOP_GROUP_EFFECT);
}

int operation_tags()
{
  return IOP_TAG_DECORATION;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_iop(self, FALSE, NC_("accel", "refresh"), 0, 0);
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "opacity"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "scale"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "rotation"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "x offset"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "y offset"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_watermark_gui_data_t *g = (dt_iop_watermark_gui_data_t *)self->gui_data;

  dt_accel_connect_button_iop(self, "refresh", GTK_WIDGET(g->refresh));
  dt_accel_connect_slider_iop(self, "opacity", GTK_WIDGET(g->opacity));
  dt_accel_connect_slider_iop(self, "scale", GTK_WIDGET(g->scale));
  dt_accel_connect_slider_iop(self, "rotation", GTK_WIDGET(g->rotate));
  dt_accel_connect_slider_iop(self, "x offset", GTK_WIDGET(g->x_offset));
  dt_accel_connect_slider_iop(self, "y offset", GTK_WIDGET(g->y_offset));
}

static void _combo_box_set_active_text(dt_iop_watermark_gui_data_t *g, gchar *text)
{
  int i = 0;
  for(const GList *iter = g->watermarks_filenames; iter; iter = g_list_next(iter))
  {
    if(!g_strcmp0((gchar *)iter->data, text))
    {
      dt_bauhaus_combobox_set(g->watermarks, i);
      return;
    }
    i++;
  }
}

// replace < and > with &lt; and &gt;. any more? Yes! & -> &amp;
static gchar *_string_escape(const gchar *string)
{
  gchar *result;
  result = dt_util_str_replace(string, "&", "&amp;");
  result = dt_util_str_replace(result, "<", "&lt;");
  result = dt_util_str_replace(result, ">", "&gt;");
  return result;
}

static gchar *_string_substitute(gchar *string, const gchar *search, const gchar *replace)
{
  gchar *_replace = _string_escape(replace);
  gchar *result = dt_util_str_replace(string, search, _replace);
  g_free(_replace);
  return result;
}

static gchar *_watermark_get_svgdoc(dt_iop_module_t *self, dt_iop_watermark_data_t *data,
                                    const dt_image_t *image)
{
  gsize length;

  gchar *svgdoc = NULL;
  gchar configdir[PATH_MAX] = { 0 };
  gchar datadir[PATH_MAX] = { 0 };
  gchar *filename;
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));
  g_strlcat(datadir, "/watermarks/", sizeof(datadir));
  g_strlcat(configdir, "/watermarks/", sizeof(configdir));
  g_strlcat(datadir, data->filename, sizeof(datadir));
  g_strlcat(configdir, data->filename, sizeof(configdir));

  if(g_file_test(configdir, G_FILE_TEST_EXISTS))
    filename = configdir;
  else if(g_file_test(datadir, G_FILE_TEST_EXISTS))
    filename = datadir;
  else
    return NULL;

  gchar *svgdata = NULL;
  char datetime[200];

  // EXIF datetime
  struct tm tt_exif = { 0 };
  if(sscanf(image->exif_datetime_taken, "%d:%d:%d %d:%d:%d", &tt_exif.tm_year, &tt_exif.tm_mon,
            &tt_exif.tm_mday, &tt_exif.tm_hour, &tt_exif.tm_min, &tt_exif.tm_sec) == 6)
  {
    tt_exif.tm_year -= 1900;
    tt_exif.tm_mon--;
  }

  // Current datetime
  struct tm tt_cur = { 0 };
  time_t t = time(NULL);
  (void)localtime_r(&t, &tt_cur);

  if(g_file_get_contents(filename, &svgdata, &length, NULL))
  {
    // File is loaded lets substitute strings if found...

    // Darktable internal
    svgdoc = _string_substitute(svgdata, "$(DARKTABLE.NAME)", PACKAGE_NAME);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    svgdoc = _string_substitute(svgdata, "$(DARKTABLE.VERSION)", darktable_package_version);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }

    // Simple text from watermark module
    gchar buffer[1024];

    if (data->font[0] && data->text[0])
    {
      g_snprintf(buffer, sizeof(buffer), "%s", data->text);
      svgdoc = _string_substitute(svgdata, "$(WATERMARK_TEXT)", buffer);
      if(svgdoc != svgdata)
      {
        g_free(svgdata);
        svgdata = svgdoc;
      }

      PangoFontDescription *font = pango_font_description_from_string(data->font);
      const PangoStyle font_style = pango_font_description_get_style(font);
      const int font_weight = (int)pango_font_description_get_weight(font);

      g_snprintf(buffer, sizeof(buffer), "%s", pango_font_description_get_family(font));
      svgdoc = _string_substitute(svgdata, "$(WATERMARK_FONT_FAMILY)", buffer);
      if(svgdoc != svgdata)
      {
        g_free(svgdata);
        svgdata = svgdoc;
      }

      switch (font_style)
      {
      case PANGO_STYLE_OBLIQUE:
        g_strlcpy(buffer, "oblique", sizeof(buffer));
        break;
      case PANGO_STYLE_ITALIC:
        g_strlcpy(buffer, "italic", sizeof(buffer));
        break;
      default:
        g_strlcpy(buffer, "normal", sizeof(buffer));
        break;
      }
      svgdoc = _string_substitute(svgdata, "$(WATERMARK_FONT_STYLE)", buffer);
      if(svgdoc != svgdata)
      {
        g_free(svgdata);
        svgdata = svgdoc;
      }

      g_snprintf(buffer, sizeof(buffer), "%d", font_weight);
      svgdoc = _string_substitute(svgdata, "$(WATERMARK_FONT_WEIGHT)", buffer);
      if(svgdoc != svgdata)
      {
        g_free(svgdata);
        svgdata = svgdoc;
      }

      pango_font_description_free(font);
    }

    // watermark color
    GdkRGBA c = { data->color[0], data->color[1], data->color[2], 1.0f };
    g_snprintf(buffer, sizeof(buffer), "%s", gdk_rgba_to_string(&c));
    svgdoc = _string_substitute(svgdata, "$(WATERMARK_COLOR)", buffer);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }

    // Current image ID
    g_snprintf(buffer, sizeof(buffer), "%d", image->id);
    svgdoc = _string_substitute(svgdata, "$(IMAGE.ID)", buffer);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }

    // Current image
    dt_image_print_exif(image, buffer, sizeof(buffer));
    svgdoc = _string_substitute(svgdata, "$(IMAGE.EXIF)", buffer);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }

    // Image exif
    // EXIF date
    svgdoc = _string_substitute(svgdata, "$(EXIF.DATE)", image->exif_datetime_taken);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(EXIF.DATE.SECOND) -- 00..60
    strftime(datetime, sizeof(datetime), "%S", &tt_exif);
    svgdoc = _string_substitute(svgdata, "$(EXIF.DATE.SECOND)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(EXIF.DATE.MINUTE) -- 00..59
    strftime(datetime, sizeof(datetime), "%M", &tt_exif);
    svgdoc = _string_substitute(svgdata, "$(EXIF.DATE.MINUTE)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(EXIF.DATE.HOUR) -- 00..23
    strftime(datetime, sizeof(datetime), "%H", &tt_exif);
    svgdoc = _string_substitute(svgdata, "$(EXIF.DATE.HOUR)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(EXIF.DATE.HOUR_AMPM) -- 01..12
    strftime(datetime, sizeof(datetime), "%I %p", &tt_exif);
    svgdoc = _string_substitute(svgdata, "$(EXIF.DATE.HOUR_AMPM)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(EXIF.DATE.DAY) -- 01..31
    strftime(datetime, sizeof(datetime), "%d", &tt_exif);
    svgdoc = _string_substitute(svgdata, "$(EXIF.DATE.DAY)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(EXIF.DATE.MONTH) -- 01..12
    strftime(datetime, sizeof(datetime), "%m", &tt_exif);
    svgdoc = _string_substitute(svgdata, "$(EXIF.DATE.MONTH)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(EXIF.DATE.SHORT_MONTH) -- Jan, Feb, .., Dec, localized
    strftime(datetime, sizeof(datetime), "%b", &tt_exif);
    svgdoc = _string_substitute(svgdata, "$(EXIF.DATE.SHORT_MONTH)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(EXIF.DATE.LONG_MONTH) -- January, February, .., December, localized
    strftime(datetime, sizeof(datetime), "%B", &tt_exif);
    svgdoc = _string_substitute(svgdata, "$(EXIF.DATE.LONG_MONTH)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(EXIF.DATE.SHORT_YEAR) -- 12
    strftime(datetime, sizeof(datetime), "%y", &tt_exif);
    svgdoc = _string_substitute(svgdata, "$(EXIF.DATE.SHORT_YEAR)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(EXIF.DATE.LONG_YEAR) -- 2012
    strftime(datetime, sizeof(datetime), "%Y", &tt_exif);
    svgdoc = _string_substitute(svgdata, "$(EXIF.DATE.LONG_YEAR)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }

    // Current date
    // $(DATE) -- YYYY:
    dt_gettime_t(datetime, sizeof(datetime), t);
    svgdoc = _string_substitute(svgdata, "$(DATE)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(DATE.SECOND) -- 00..60
    strftime(datetime, sizeof(datetime), "%S", &tt_cur);
    svgdoc = _string_substitute(svgdata, "$(DATE.SECOND)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(DATE.MINUTE) -- 00..59
    strftime(datetime, sizeof(datetime), "%M", &tt_cur);
    svgdoc = _string_substitute(svgdata, "$(DATE.MINUTE)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(DATE.HOUR) -- 00..23
    strftime(datetime, sizeof(datetime), "%H", &tt_cur);
    svgdoc = _string_substitute(svgdata, "$(DATE.HOUR)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(DATE.HOUR_AMPM) -- 01..12
    strftime(datetime, sizeof(datetime), "%I %p", &tt_cur);
    svgdoc = _string_substitute(svgdata, "$(DATE.HOUR_AMPM)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(DATE.DAY) -- 01..31
    strftime(datetime, sizeof(datetime), "%d", &tt_cur);
    svgdoc = _string_substitute(svgdata, "$(DATE.DAY)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(DATE.MONTH) -- 01..12
    strftime(datetime, sizeof(datetime), "%m", &tt_cur);
    svgdoc = _string_substitute(svgdata, "$(DATE.MONTH)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(DATE.SHORT_MONTH) -- Jan, Feb, .., Dec, localized
    strftime(datetime, sizeof(datetime), "%b", &tt_cur);
    svgdoc = _string_substitute(svgdata, "$(DATE.SHORT_MONTH)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(DATE.LONG_MONTH) -- January, February, .., December, localized
    strftime(datetime, sizeof(datetime), "%B", &tt_cur);
    svgdoc = _string_substitute(svgdata, "$(DATE.LONG_MONTH)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(DATE.SHORT_YEAR) -- 12
    strftime(datetime, sizeof(datetime), "%y", &tt_cur);
    svgdoc = _string_substitute(svgdata, "$(DATE.SHORT_YEAR)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    // $(DATE.LONG_YEAR) -- 2012
    strftime(datetime, sizeof(datetime), "%Y", &tt_cur);
    svgdoc = _string_substitute(svgdata, "$(DATE.LONG_YEAR)", datetime);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }

    svgdoc = _string_substitute(svgdata, "$(EXIF.MAKER)", image->camera_maker);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    svgdoc = _string_substitute(svgdata, "$(EXIF.MODEL)", image->camera_model);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    svgdoc = _string_substitute(svgdata, "$(EXIF.LENS)", image->exif_lens);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }

    svgdoc = _string_substitute(svgdata, "$(IMAGE.FILENAME)", image->filename);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }

    gchar *basename = g_path_get_basename(image->filename);
    if(g_strrstr(basename, ".")) *(g_strrstr(basename, ".")) = '\0';
    svgdoc = _string_substitute(svgdata, "$(IMAGE.BASENAME)", basename);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    g_free(basename);

    // TODO: auto generate that code?
    GList *res;
    res = dt_metadata_get(image->id, "Xmp.dc.creator", NULL);
    svgdoc = _string_substitute(svgdata, "$(Xmp.dc.creator)", (res ? res->data : ""));
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    if(res)
    {
      g_list_free_full(res, &g_free);
    }

    res = dt_metadata_get(image->id, "Xmp.dc.publisher", NULL);
    svgdoc = _string_substitute(svgdata, "$(Xmp.dc.publisher)", (res ? res->data : ""));
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    if(res)
    {
      g_list_free_full(res, &g_free);
    }

    res = dt_metadata_get(image->id, "Xmp.dc.title", NULL);
    svgdoc = _string_substitute(svgdata, "$(Xmp.dc.title)", (res ? res->data : ""));
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    if(res)
    {
      g_list_free_full(res, &g_free);
    }

    res = dt_metadata_get(image->id, "Xmp.dc.description", NULL);
    svgdoc = _string_substitute(svgdata, "$(Xmp.dc.description)", (res ? res->data : ""));
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    if(res)
    {
      g_list_free_full(res, &g_free);
    }

    res = dt_metadata_get(image->id, "Xmp.dc.rights", NULL);
    svgdoc = _string_substitute(svgdata, "$(Xmp.dc.rights)", (res ? res->data : ""));
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    if(res)
    {
      g_list_free_full(res, &g_free);
    }

    res = dt_tag_get_list(image->id);
    gchar *keywords = dt_util_glist_to_str(", ", res);
    svgdoc = _string_substitute(svgdata, "$(IMAGE.TAGS)", (keywords ? keywords : ""));
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    g_free(keywords);
    if(res)
    {
      g_list_free_full(res, &g_free);
    }

    const int stars = image->flags & 0x7;
    const char *const rating_str[] = { "☆☆☆☆☆", "★☆☆☆☆", "★★☆☆☆", "★★★☆☆", "★★★★☆", "★★★★★", "❌", "" };
    svgdoc = _string_substitute(svgdata, "$(Xmp.xmp.Rating)", rating_str[stars]);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }

    // geolocation
    gchar *latitude = NULL, *longitude = NULL, *elevation = NULL;
    if(dt_conf_get_bool("plugins/lighttable/metadata_view/pretty_location"))
    {
      latitude = dt_util_latitude_str(image->latitude);
      longitude = dt_util_longitude_str(image->longitude);
      elevation = dt_util_elevation_str(image->elevation);
    }
    else
    {
      const gchar NS = image->latitude < 0 ? 'S' : 'N';
      const gchar EW = image->longitude < 0 ? 'W' : 'E';
      if(image->latitude) latitude = g_strdup_printf("%c %09.6f", NS, fabs(image->latitude));
      if(image->longitude) longitude = g_strdup_printf("%c %010.6f", EW, fabs(image->longitude));
      if(image->elevation) elevation = g_strdup_printf("%.2f %s", image->elevation, _("m"));
    }
    gchar *parts[4] = { 0 };
    int i = 0;
    if(latitude) parts[i++] = latitude;
    if(longitude) parts[i++] = longitude;
    if(elevation) parts[i++] = elevation;
    gchar *location = g_strjoinv(", ", parts);
    svgdoc = _string_substitute(svgdata, "$(GPS.LATITUDE)", (latitude ? latitude : "-"));
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    svgdoc = _string_substitute(svgdata, "$(GPS.LONGITUDE)", (longitude ? longitude : "-"));
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    svgdoc = _string_substitute(svgdata, "$(GPS.ELEVATION)", (elevation ? elevation : "-"));
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    svgdoc = _string_substitute(svgdata, "$(GPS.LOCATION)", location);
    if(svgdoc != svgdata)
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    g_free(latitude);
    g_free(longitude);
    g_free(elevation);
    g_free(location);

  }
  return svgdoc;
}

void process(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const void *const ivoid,
             void *const ovoid, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  dt_iop_watermark_data_t *data = (dt_iop_watermark_data_t *)piece->data;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;
  double angle = (M_PI / 180) * -data->rotate;

  /* Load svg if not loaded */
  gchar *svgdoc = _watermark_get_svgdoc(self, data, &piece->pipe->image);
  if(!svgdoc)
  {
    memcpy(ovoid, ivoid, (size_t)sizeof(float) * ch * roi_out->width * roi_out->height);
    return;
  }

  /* setup stride for performance */
  int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, roi_out->width);

  /* create cairo memory surface */
  guint8 *image = (guint8 *)g_malloc0_n(roi_out->height, stride);
  cairo_surface_t *surface = cairo_image_surface_create_for_data(image, CAIRO_FORMAT_ARGB32, roi_out->width,
                                                                 roi_out->height, stride);
  if(cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS)
  {
    //   fprintf(stderr,"Cairo surface error: %s\n",cairo_status_to_string(cairo_surface_status(surface)));
    g_free(image);
    memcpy(ovoid, ivoid, (size_t)sizeof(float) * ch * roi_out->width * roi_out->height);
    return;
  }

  /* create cairo context and setup transformation/scale */
  cairo_t *cr = cairo_create(surface);

  // rsvg (or some part of cairo which is used underneath) isn't thread safe, for example when handling fonts
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);

  /* create the rsvghandle from parsed svg data */
  GError *error = NULL;
  RsvgHandle *svg = rsvg_handle_new_from_data((const guint8 *)svgdoc, strlen(svgdoc), &error);
  g_free(svgdoc);
  if(!svg || error)
  {
    g_free(image);
    memcpy(ovoid, ivoid, (size_t)sizeof(float) * ch * roi_out->width * roi_out->height);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    fprintf(stderr, "[watermark] error processing svg file: %s\n", error->message);
    g_error_free(error);
    return;
  }

  /* get the dimension of svg */
  RsvgDimensionData dimension;
  rsvg_handle_get_dimensions(svg, &dimension);

  //  width/height of current (possibly cropped) image
  const float iw = piece->buf_in.width;
  const float ih = piece->buf_in.height;
  const float uscale = data->scale / 100.0; // user scale, from GUI in percent

  // wbase, hbase are the base width and height, this is the multiplicator used for the offset computing
  // scale is the scale of the watermark itself and is used only to render it.

  float wbase, hbase, scale;

  if(data->sizeto == DT_SCALE_IMAGE)
  {
    // in image mode, the wbase and hbase are just the image width and height
    wbase = iw;
    hbase = ih;
    if(dimension.width > dimension.height)
      scale = (iw * roi_out->scale) / dimension.width;
    else
      scale = (ih * roi_out->scale) / dimension.height;
  }
  else
  {
    // in larger/smaller side mode, set wbase and hbase to the largest or smallest side of the image
    float larger;
    if(dimension.width > dimension.height)
      larger = (float)dimension.width;
    else
      larger = (float)dimension.height;

    if(iw > ih)
    {
      wbase = hbase = (data->sizeto == DT_SCALE_LARGER_BORDER) ? iw : ih;
      scale = (data->sizeto == DT_SCALE_LARGER_BORDER) ? (iw / larger) : (ih / larger);
    }
    else
    {
      wbase = hbase = (data->sizeto == DT_SCALE_SMALLER_BORDER) ? iw : ih;
      scale = (data->sizeto == DT_SCALE_SMALLER_BORDER) ? (iw / larger) : (ih / larger);
    }
    scale *= roi_out->scale;
  }

  scale *= uscale;

  // compute the width and height of the SVG object in image dimension. This is only used to properly
  // layout the watermark based on the alignment.

  float svg_width, svg_height;

  if(dimension.width > dimension.height)
  {
    if(data->sizeto == DT_SCALE_IMAGE || (iw > ih && data->sizeto == DT_SCALE_LARGER_BORDER)
       || (iw < ih && data->sizeto == DT_SCALE_SMALLER_BORDER))
    {
      svg_width = iw * uscale;
      svg_height = dimension.height * (svg_width / dimension.width);
    }
    else
    {
      svg_width = ih * uscale;
      svg_height = dimension.height * (svg_width / dimension.width);
    }
  }
  else
  {
    if(data->sizeto == DT_SCALE_IMAGE || (ih > iw && data->sizeto == DT_SCALE_LARGER_BORDER)
       || (ih < iw && data->sizeto == DT_SCALE_SMALLER_BORDER))
    {
      svg_height = ih * uscale;
      svg_width = dimension.width * (svg_height / dimension.height);
    }
    else
    {
      svg_height = iw * uscale;
      svg_width = dimension.width * (svg_height / dimension.height);
    }
  }

  // compute bounding box of rotated watermark
  float bb_width, bb_height;
  bb_width = fabs(svg_width * cos(angle)) + fabs(svg_height * sin(angle));
  bb_height = fabs(svg_width * sin(angle)) + fabs(svg_height * cos(angle));
  float bX = bb_width / 2.0 - svg_width / 2.0;
  float bY = bb_height / 2.0 - svg_height / 2.0;

  // compute translation for the given alignment in image dimension

  float ty = 0, tx = 0;
  if(data->alignment >= 0 && data->alignment < 3) // Align to verttop
    ty = bY;
  else if(data->alignment >= 3 && data->alignment < 6) // Align to vertcenter
    ty = (ih / 2.0) - (svg_height / 2.0);
  else if(data->alignment >= 6 && data->alignment < 9) // Align to vertbottom
    ty = ih - svg_height - bY;

  if(data->alignment == 0 || data->alignment == 3 || data->alignment == 6)
    tx = bX;
  else if(data->alignment == 1 || data->alignment == 4 || data->alignment == 7)
    tx = (iw / 2.0) - (svg_width / 2.0);
  else if(data->alignment == 2 || data->alignment == 5 || data->alignment == 8)
    tx = iw - svg_width - bX;

  // translate to position
  cairo_translate(cr, -roi_in->x, -roi_in->y);

  // add translation for the given value in GUI (xoffset,yoffset)
  tx += data->xoffset * wbase;
  ty += data->yoffset * hbase;

  cairo_translate(cr, tx * roi_out->scale, ty * roi_out->scale);

  // compute the center of the svg to rotate from the center
  float cX = svg_width / 2.0 * roi_out->scale;
  float cY = svg_height / 2.0 * roi_out->scale;

  cairo_translate(cr, cX, cY);
  cairo_rotate(cr, angle);
  cairo_translate(cr, -cX, -cY);

  // now set proper scale for the watermark itself
  cairo_scale(cr, scale, scale);

  /* render svg into surface*/
  rsvg_handle_render_cairo(svg, cr);

  // no more non-thread safe rsvg usage
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  cairo_destroy(cr);

  /* ensure that all operations on surface finishing up */
  cairo_surface_flush(surface);

  /* render surface on output */
  guint8 *sd = image;
  float opacity = data->opacity / 100.0;
  /*
  #ifdef _OPENMP
    #pragma omp parallel for default(none) shared(in, out,sd,opacity) schedule(static)
  #endif
  */
  for(int j = 0; j < roi_out->height; j++)
    for(int i = 0; i < roi_out->width; i++)
    {
      float alpha = (sd[3] / 255.0) * opacity;
      /* svg uses a premultiplied alpha, so only use opacity for the blending */
      out[0] = ((1.0 - alpha) * in[0]) + (opacity * (sd[2] / 255.0));
      out[1] = ((1.0 - alpha) * in[1]) + (opacity * (sd[1] / 255.0));
      out[2] = ((1.0 - alpha) * in[2]) + (opacity * (sd[0] / 255.0));
      out[3] = in[3];

      out += ch;
      in += ch;
      sd += 4;
    }


  /* clean up */
  cairo_surface_destroy(surface);
  g_object_unref(svg);
  g_free(image);
}

static void watermark_callback(GtkWidget *tb, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_watermark_gui_data_t *g = (dt_iop_watermark_gui_data_t *)self->gui_data;

  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  memset(p->filename, 0, sizeof(p->filename));
  int n = dt_bauhaus_combobox_get(g->watermarks);
  snprintf(p->filename, sizeof(p->filename), "%s", (char *)g_list_nth_data(g->watermarks_filenames, n));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void load_watermarks(const char *basedir, dt_iop_watermark_gui_data_t *g)
{
  GList *files = NULL;
  char *watermarks_dir = g_build_filename(basedir, "watermarks", NULL);
  GDir *dir = g_dir_open(watermarks_dir, 0, NULL);
  if(dir)
  {
    const gchar *d_name;
    while((d_name = g_dir_read_name(dir)))
      files = g_list_append(files, g_strdup(d_name));
    g_dir_close(dir);
  }

  files = g_list_sort(files, (GCompareFunc)g_strcmp0);
  for(GList *iter = files; iter; iter = g_list_next(iter))
  {
    char *filename = iter->data;
    // remember the whole filename for later
    g->watermarks_filenames = g_list_append(g->watermarks_filenames, g_strdup(filename));
    // ... and remove the file extension from the string shown in the gui
    char *c = filename + strlen(filename);
    while(c >= filename && *c != '.') *c-- = '\0';
    if(*c == '.') *c = '\0';
    dt_bauhaus_combobox_add(g->watermarks, filename);
  }

  g_list_free_full(files, g_free);
  g_free(watermarks_dir);
}

static void refresh_watermarks(dt_iop_module_t *self)
{
  dt_iop_watermark_gui_data_t *g = (dt_iop_watermark_gui_data_t *)self->gui_data;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;

  g_signal_handlers_block_by_func(g->watermarks, watermark_callback, self);

  // Clear combobox...
  dt_bauhaus_combobox_clear(g->watermarks);
  g_list_free_full(g->watermarks_filenames, g_free);
  g->watermarks_filenames = NULL;

  // check watermarkdir and update combo with entries...
  gchar configdir[PATH_MAX] = { 0 };
  gchar datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));

  load_watermarks(datadir, g);
  load_watermarks(configdir, g);

  _combo_box_set_active_text(g, p->filename);

  g_signal_handlers_unblock_by_func(g->watermarks, watermark_callback, self);
}

static void refresh_callback(GtkWidget *tb, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  refresh_watermarks(self);
}



static void alignment_callback(GtkWidget *tb, gpointer user_data)
{
  int index = -1;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_watermark_gui_data_t *g = (dt_iop_watermark_gui_data_t *)self->gui_data;

  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;


  for(int i = 0; i < 9; i++)
  {
    /* block signal handler */
    g_signal_handlers_block_by_func(g->align[i], alignment_callback, user_data);

    if(GTK_WIDGET(g->align[i]) == tb)
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->align[i]), TRUE);
      index = i;
    }
    else
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->align[i]), FALSE);

    /* unblock signal handler */
    g_signal_handlers_unblock_by_func(g->align[i], alignment_callback, user_data);
  }
  p->alignment = index;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void opacity_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  p->opacity = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void text_callback(GtkWidget *entry, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  snprintf(p->text, sizeof(p->text), "%s", gtk_entry_get_text(GTK_ENTRY(entry)));
  dt_conf_set_string("plugins/darkroom/watermark/text", p->text);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void colorpick_color_set(GtkColorButton *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;

  GdkRGBA c;
  gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(widget), &c);
  p->color[0] = c.red;
  p->color[1] = c.green;
  p->color[2] = c.blue;

  dt_conf_set_float("plugins/darkroom/watermark/color_red", p->color[0]);
  dt_conf_set_float("plugins/darkroom/watermark/color_green", p->color[1]);
  dt_conf_set_float("plugins/darkroom/watermark/color_blue", p->color[2]);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void fontsel_callback(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;

  snprintf(p->font, sizeof(p->font), "%s", gtk_font_button_get_font_name(GTK_FONT_BUTTON(button)));
  dt_conf_set_string("plugins/darkroom/watermark/font", p->font);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void xoffset_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  p->xoffset = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void yoffset_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  p->yoffset = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void scale_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  p->scale = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void rotate_callback(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  p->rotate = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void sizeto_callback(GtkWidget *tb, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;

  if(darktable.gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  p->sizeto = dt_bauhaus_combobox_get(tb);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)p1;
  dt_iop_watermark_data_t *d = (dt_iop_watermark_data_t *)piece->data;

  d->opacity = p->opacity;
  d->scale = p->scale;
  d->rotate = p->rotate;
  d->xoffset = p->xoffset;
  d->yoffset = p->yoffset;
  d->alignment = p->alignment;
  d->sizeto = p->sizeto;
  memset(d->filename, 0, sizeof(d->filename));
  snprintf(d->filename, sizeof(d->filename), "%s", p->filename);
  memset(d->text, 0, sizeof(d->text));
  snprintf(d->text, sizeof(d->text), "%s", p->text);
  for (int k=0; k<3; k++)
    d->color[k] = p->color[k];
  memset(d->font, 0, sizeof(d->font));
  snprintf(d->font, sizeof(d->font), "%s", p->font);

// fprintf(stderr,"Commit params: %s...\n",d->filename);
}

void init_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_watermark_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe(struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}


void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_watermark_gui_data_t *g = (dt_iop_watermark_gui_data_t *)self->gui_data;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)module->params;
  dt_bauhaus_slider_set(g->opacity, p->opacity);
  dt_bauhaus_slider_set_soft(g->scale, p->scale);
  dt_bauhaus_slider_set(g->rotate, p->rotate);
  dt_bauhaus_slider_set(g->x_offset, p->xoffset);
  dt_bauhaus_slider_set(g->y_offset, p->yoffset);
  for(int i = 0; i < 9; i++)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->align[i]), FALSE);
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->align[p->alignment]), TRUE);
  _combo_box_set_active_text(g, p->filename);
  dt_bauhaus_combobox_set(g->sizeto, p->sizeto);
  gtk_entry_set_text(GTK_ENTRY(g->text), p->text);
  GdkRGBA color = (GdkRGBA){.red = p->color[0], .green = p->color[1], .blue = p->color[2], .alpha = 1.0 };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->colorpick), &color);
  gtk_font_button_set_font_name(GTK_FONT_BUTTON(g->fontsel), p->font);
}

void init(dt_iop_module_t *module)
{
  module->params = calloc(1, sizeof(dt_iop_watermark_params_t));
  module->params_size = sizeof(dt_iop_watermark_params_t);
  module->default_params = calloc(1, sizeof(dt_iop_watermark_params_t));
  module->default_enabled = 0;
  module->priority = 971; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_watermark_params_t);
  module->gui_data = NULL;
  dt_iop_watermark_params_t tmp = (dt_iop_watermark_params_t){
    100.0, 100.0, 0.0, 0.0, 4, 0.0, DT_SCALE_IMAGE, { "darktable.svg" }, { "" }, {0.0, 0.0, 0.0}, {"DejaVu Sans 10"}
  }; // opacity,scale,xoffs,yoffs,alignment
  memcpy(module->params, &tmp, sizeof(dt_iop_watermark_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_watermark_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = calloc(1, sizeof(dt_iop_watermark_gui_data_t));
  dt_iop_watermark_gui_data_t *g = (dt_iop_watermark_gui_data_t *)self->gui_data;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;

  int line = 0;
  self->widget = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(self->widget), DT_BAUHAUS_SPACE);
  gtk_grid_set_column_spacing(GTK_GRID(self->widget), DT_PIXEL_APPLY_DPI(10));
  dt_gui_add_help_link(self->widget, dt_get_help_url(self->op));

  gtk_grid_attach(GTK_GRID(self->widget), dt_ui_section_label_new(_("content")), 0, line++, 3, 1);

  // Add the marker combobox
  gchar configdir[PATH_MAX] = { 0 };
  gchar datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));
  GtkWidget *label = dtgtk_reset_label_new(_("marker"), self, &p->filename, sizeof(p->filename));
  g->watermarks = dt_bauhaus_combobox_new(self);
  gtk_widget_set_hexpand(GTK_WIDGET(g->watermarks), TRUE);
  char *tooltip = g_strdup_printf(_("SVG watermarks in %s/watermarks or %s/watermarks"), configdir, datadir);
  gtk_widget_set_tooltip_text(g->watermarks, tooltip);
  g_free(tooltip);
  g->refresh = dtgtk_button_new(dtgtk_cairo_paint_refresh, CPF_STYLE_FLAT | CPF_DO_NOT_USE_BORDER, NULL);
  gtk_grid_attach(GTK_GRID(self->widget), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(self->widget), g->watermarks, label, GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(self->widget), g->refresh, g->watermarks, GTK_POS_RIGHT, 1, 1);


  // Watermark color
  float red = dt_conf_get_float("plugins/darkroom/watermark/color_red");
  float green = dt_conf_get_float("plugins/darkroom/watermark/color_green");
  float blue = dt_conf_get_float("plugins/darkroom/watermark/color_blue");
  GdkRGBA color = (GdkRGBA){.red = red, .green = green, .blue = blue, .alpha = 1.0 };

  label = dtgtk_reset_label_new(_("color"), self, &p->color, 3 * sizeof(float));
  g->colorpick = gtk_color_button_new_with_rgba(&color);
  gtk_widget_set_tooltip_text(g->colorpick, _("watermark color, tag:\n$(WATERMARK_COLOR)"));
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->colorpick), FALSE);
  gtk_widget_set_size_request(GTK_WIDGET(g->colorpick), DT_PIXEL_APPLY_DPI(24), DT_PIXEL_APPLY_DPI(24));
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->colorpick), _("select watermark color"));

  gtk_grid_attach(GTK_GRID(self->widget), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(self->widget), g->colorpick, label, GTK_POS_RIGHT, 2, 1);

  // Simple text
  label = gtk_label_new(_("text"));
  gtk_widget_set_halign(label, GTK_ALIGN_START);
  g->text = gtk_entry_new();
  gtk_entry_set_width_chars(GTK_ENTRY(g->text), 1);
  gtk_widget_set_tooltip_text(g->text, _("text string, tag:\n$(WATERMARK_TEXT)"));
  dt_gui_key_accel_block_on_focus_connect(g->text);
  gtk_grid_attach(GTK_GRID(self->widget), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(self->widget), g->text, label, GTK_POS_RIGHT, 2, 1);

  gchar *str = dt_conf_get_string("plugins/darkroom/watermark/text");
  gtk_entry_set_text(GTK_ENTRY(g->text), str);
  g_free(str);

  // Text font
  label = dtgtk_reset_label_new(_("font"), self, &p->font, sizeof(p->font));
  str = dt_conf_get_string("plugins/darkroom/watermark/font");
  g->fontsel = gtk_font_button_new_with_font(str==NULL?"DejaVu Sans 10":str);
  GList *childs = gtk_container_get_children(GTK_CONTAINER(gtk_bin_get_child(GTK_BIN(g->fontsel))));
  gtk_label_set_ellipsize(GTK_LABEL(childs->data), PANGO_ELLIPSIZE_MIDDLE);
  g_list_free(childs);
  gtk_widget_set_tooltip_text(g->fontsel, _("text font, tags:\n$(WATERMARK_FONT_FAMILY)\n"
                                            "$(WATERMARK_FONT_STYLE)\n$(WATERMARK_FONT_WEIGHT)"));
  gtk_font_button_set_show_size (GTK_FONT_BUTTON(g->fontsel), FALSE);
  g_free(str);
  gtk_grid_attach(GTK_GRID(self->widget), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(self->widget), g->fontsel, label, GTK_POS_RIGHT, 2, 1);

  gtk_grid_attach(GTK_GRID(self->widget), dt_ui_section_label_new(_("properties")), 0, line++, 3, 1);

  // Add opacity/scale sliders to table
  g->opacity = dt_bauhaus_slider_new_with_range(self, 0.0, 100.0, 1.0, p->opacity, 0);
  dt_bauhaus_slider_set_format(g->opacity, "%.f%%");
  dt_bauhaus_widget_set_label(g->opacity, NULL, _("opacity"));
  g->scale = dt_bauhaus_slider_new_with_range(self, 1.0, 100.0, 1.0, p->scale, 0);
  dt_bauhaus_slider_enable_soft_boundaries(g->scale, 1.0, 500.0);
  dt_bauhaus_slider_set_format(g->scale, "%.f%%");
  dt_bauhaus_widget_set_label(g->scale, NULL, _("scale"));
  g->rotate = dt_bauhaus_slider_new_with_range(self, -180.0, 180.0, 1.0, p->rotate, 2);
  dt_bauhaus_slider_set_format(g->rotate, "%.02f°");
  dt_bauhaus_widget_set_label(g->rotate, NULL, _("rotation"));
  gtk_grid_attach(GTK_GRID(self->widget), g->opacity, 0, line++, 3, 1);
  gtk_grid_attach(GTK_GRID(self->widget), g->scale, 0, line++, 3, 1);
  gtk_grid_attach(GTK_GRID(self->widget), g->rotate, 0, line++, 3, 1);

  g->sizeto = dt_bauhaus_combobox_new(self);
  dt_bauhaus_combobox_add(g->sizeto, C_("size", "image"));
  dt_bauhaus_combobox_add(g->sizeto, _("larger border"));
  dt_bauhaus_combobox_add(g->sizeto, _("smaller border"));
  dt_bauhaus_combobox_set(g->sizeto, p->sizeto);
  dt_bauhaus_widget_set_label(g->sizeto, NULL, _("scale on"));
  gtk_widget_set_tooltip_text(g->sizeto, _("size is relative to"));
  gtk_grid_attach(GTK_GRID(self->widget), g->sizeto, 0, line++, 3, 1);

  gtk_grid_attach(GTK_GRID(self->widget), dt_ui_section_label_new(_("position")), 0, line++, 3, 1);

  // Create the 3x3 gtk table toggle button table...
  label = dtgtk_reset_label_new(_("alignment"), self, &p->alignment, sizeof(p->alignment));
  GtkWidget *bat = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(bat), DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(GTK_GRID(bat), DT_PIXEL_APPLY_DPI(3));
  for(int i = 0; i < 9; i++)
  {
    g->align[i] = dtgtk_togglebutton_new(dtgtk_cairo_paint_alignment, CPF_STYLE_FLAT | (CPF_SPECIAL_FLAG << i), NULL);
    gtk_widget_set_size_request(GTK_WIDGET(g->align[i]), DT_PIXEL_APPLY_DPI(16), DT_PIXEL_APPLY_DPI(16));
    gtk_grid_attach(GTK_GRID(bat), GTK_WIDGET(g->align[i]), i%3, i/3, 1, 1);
    g_signal_connect(G_OBJECT(g->align[i]), "toggled", G_CALLBACK(alignment_callback), self);
  }
  gtk_grid_attach(GTK_GRID(self->widget), label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(GTK_GRID(self->widget), bat, label, GTK_POS_RIGHT, 2, 1);

  // x/y offset
  g->x_offset = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.001, p->xoffset, 3);
  dt_bauhaus_slider_set_format(g->x_offset, "%.3f");
  dt_bauhaus_widget_set_label(g->x_offset, NULL, _("x offset"));
  g->y_offset = dt_bauhaus_slider_new_with_range(self, -1.0, 1.0, 0.001, p->yoffset, 3);
  dt_bauhaus_slider_set_format(g->y_offset, "%.3f");
  dt_bauhaus_widget_set_label(g->y_offset, NULL, _("y offset"));
  gtk_grid_attach(GTK_GRID(self->widget), g->x_offset, 0, line++, 3, 1);
  gtk_grid_attach(GTK_GRID(self->widget), g->y_offset, 0, line++, 3, 1);

  // Let's add some tooltips and hook up some signals...
  gtk_widget_set_tooltip_text(g->opacity, _("the opacity of the watermark"));
  gtk_widget_set_tooltip_text(g->scale, _("the scale of the watermark"));
  gtk_widget_set_tooltip_text(g->rotate, _("the rotation of the watermark"));

  g_signal_connect(G_OBJECT(g->opacity), "value-changed", G_CALLBACK(opacity_callback), self);
  g_signal_connect(G_OBJECT(g->scale), "value-changed", G_CALLBACK(scale_callback), self);
  g_signal_connect(G_OBJECT(g->rotate), "value-changed", G_CALLBACK(rotate_callback), self);

  g_signal_connect(G_OBJECT(g->x_offset), "value-changed", G_CALLBACK(xoffset_callback), self);

  g_signal_connect(G_OBJECT(g->y_offset), "value-changed", G_CALLBACK(yoffset_callback), self);


  g_signal_connect(G_OBJECT(g->refresh), "clicked", G_CALLBACK(refresh_callback), self);

  refresh_watermarks(self);


  g_signal_connect(G_OBJECT(g->watermarks), "value-changed", G_CALLBACK(watermark_callback), self);
  g_signal_connect(G_OBJECT(g->sizeto), "value-changed", G_CALLBACK(sizeto_callback), self);

  g_signal_connect(G_OBJECT(g->text), "changed", G_CALLBACK(text_callback), self);
  g_signal_connect(G_OBJECT(g->colorpick), "color-set", G_CALLBACK(colorpick_color_set), self);
  g_signal_connect(G_OBJECT(g->fontsel), "font-set", G_CALLBACK(fontsel_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{

  dt_iop_watermark_gui_data_t *g = (dt_iop_watermark_gui_data_t *)self->gui_data;
  g_list_free_full(g->watermarks_filenames, g_free);
  g->watermarks_filenames = NULL;
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
