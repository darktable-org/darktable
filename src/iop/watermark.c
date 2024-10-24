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

#include "bauhaus/bauhaus.h"
#include "common/imagebuf.h"
#include "common/tags.h"
#include "common/variables.h"
#include "common/datetime.h"
#include "control/control.h"
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "dtgtk/button.h"
#include "dtgtk/resetlabel.h"
#include "dtgtk/togglebutton.h"
#include "gui/accelerators.h"
#include "gui/color_picker_proxy.h"
#include "gui/gtk.h"
#include "gui/gtkentry.h"
#include "iop/iop_api.h"
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

DT_MODULE_INTROSPECTION(6, dt_iop_watermark_params_t)

// gchar *checksum = g_compute_checksum_for_data(G_CHECKSUM_MD5,data,length);

typedef enum dt_iop_watermark_base_scale_t
{
  DT_SCALE_MAINMENU_IMAGE            = 0,  // $DESCRIPTION: "image"
  DT_SCALE_MAINMENU_LARGER_BORDER    = 1,  // $DESCRIPTION: "larger border"
  DT_SCALE_MAINMENU_SMALLER_BORDER   = 2,  // $DESCRIPTION: "smaller border"
  DT_SCALE_MAINMENU_MARKERHEIGHT     = 3,  // $DESCRIPTION: "height"
  DT_SCALE_MAINMENU_ADVANCED         = 4   // $DESCRIPTION: "advanced options"
} dt_iop_watermark_base_scale_t;           // this is the first drop-down menu, always visible

typedef enum dt_iop_watermark_img_scale_t
{
  DT_SCALE_IMG_WIDTH                 = 1,  // $DESCRIPTION: "image width"
  DT_SCALE_IMG_HEIGHT                = 2,  // $DESCRIPTION: "image height"
  DT_SCALE_IMG_LARGER                = 3,  // $DESCRIPTION: "larger image border"
  DT_SCALE_IMG_SMALLER               = 4,  // $DESCRIPTION: "smaller image border"
} dt_iop_watermark_img_scale_t;            // advanced drop-down no. 1

typedef enum dt_iop_watermark_svg_scale_t
{
  DT_SCALE_SVG_WIDTH                 = 0,  // $DESCRIPTION: "marker width"
  DT_SCALE_SVG_HEIGHT                = 1,  // $DESCRIPTION: "marker height"
} dt_iop_watermark_svg_scale_t;            // advanced drop-down no. 1

typedef enum dt_iop_watermark_type_t
{
  DT_WTM_SVG = 0,         // $DESCRIPTION: "vector .svg"
  DT_WTM_PNG = 1          // $DESCRIPTION: "raster .png"
} dt_iop_watermark_type_t;

typedef struct dt_iop_watermark_params_t
{
  /** opacity value of rendering watermark */
  float opacity; // $MIN: 0.0 $MAX: 100.0 $DEFAULT: 100.0
  /** scale value of rendering watermark */
  float scale;   // $MIN: 1.0 $MAX: 500.0 $DEFAULT: 100.0
  /** Pixel independent xoffset, 0 to 1 */
  float xoffset; // $MIN: -1.0 $MAX: 1.0, 0.001 $DEFAULT: 0.0 $DESCRIPTION: "x offset"
  /** Pixel independent yoffset, 0 to 1 */
  float yoffset; // $MIN: -1.0 $MAX: 1.0, 0.001 $DEFAULT: 0.0 $DESCRIPTION: "y offset"
  /** Alignment value 0-8 3x3 */
  int alignment; // $DEFAULT: 4
  /** Rotation **/
  float rotate;  // $MIN: -180.0 $MAX: 180.0 $DEFAULT: 0.0 $DESCRIPTION: "rotation"
  dt_iop_watermark_base_scale_t scale_base; // $DEFAULT: DT_SCALE_MAINMENU_IMAGE $DESCRIPTION: "scale on"
  dt_iop_watermark_img_scale_t scale_img; // $DEFAULT: DT_SCALE_IMG_LARGER $DESCRIPTION: "scale marker to"
  dt_iop_watermark_svg_scale_t scale_svg; // $DEFAULT: DT_SCALE_SVG_WIDTH $DESCRIPTION: "scale marker reference"
  char filename[64];
  /* simple text */
  char text[512];
  /* text color */
  float color[3]; // $DEFAULT: 0.0
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
  dt_iop_watermark_base_scale_t scale_base;
  dt_iop_watermark_svg_scale_t scale_svg;
  dt_iop_watermark_img_scale_t scale_img;
  char filename[64];
  char text[512];
  float color[3];
  char font[64];
} dt_iop_watermark_data_t;

typedef struct dt_iop_watermark_gui_data_t
{
  GtkWidget *watermarks;                             // watermark
  GList     *watermarks_filenames;                   // the actual filenames
  GtkWidget *refresh;                                // refresh watermarks...
  GtkWidget *align[9];                               // Alignment buttons
  GtkWidget *opacity, *scale, *x_offset, *y_offset;  // opacity, scale, xoffs, yoffs
  GtkWidget *scale_base;                             // "scale on"
  GtkWidget *scale_img;                              // scale reference of image
  GtkWidget *scale_svg;                              // scale reference of marker
  GtkWidget *rotate;
  GtkWidget *text;
  GtkWidget *colorpick;
  GtkWidget *fontsel;
  GtkWidget *color_picker_button;
} dt_iop_watermark_gui_data_t;

// option selection from module version 2 through 5
typedef enum dt_iop_watermark_base_scale_v2_t
{
  DT_SCALE_IMAGE = 0,         // $DESCRIPTION: "image"
  DT_SCALE_LARGER_BORDER = 1, // $DESCRIPTION: "larger border"
  DT_SCALE_SMALLER_BORDER = 2 // $DESCRIPTION: "smaller border"
} dt_iop_watermark_base_scale_v2_t;

int legacy_params(dt_iop_module_t *self,
                  const void *const old_params,
                  const int old_version,
                  void **new_params,
                  int32_t *new_params_size,
                  int *new_version)
{
  typedef struct dt_iop_watermark_params_v6_t
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
    dt_iop_watermark_base_scale_t scale_base;
    dt_iop_watermark_img_scale_t scale_img;
    dt_iop_watermark_svg_scale_t scale_svg;
    char filename[64];
    /* simple text */
    char text[512];
    /* text color */
    float color[3];
    /* text font */
    char font[64];
  } dt_iop_watermark_params_v6_t;

  if(old_version == 1)
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

    const dt_iop_watermark_params_v1_t *o = (dt_iop_watermark_params_v1_t *)old_params;
    dt_iop_watermark_params_v6_t *n = malloc(sizeof(dt_iop_watermark_params_v6_t));

    n->opacity = o->opacity;
    n->scale = o->scale;
    n->xoffset = o->xoffset;
    n->yoffset = o->yoffset;
    n->alignment = o->alignment;
    n->rotate = 0.0;
    n->scale_base = DT_SCALE_MAINMENU_IMAGE;
    g_strlcpy(n->filename, o->filename, sizeof(n->filename));
    g_strlcpy(n->text, "", sizeof(n->text));
    g_strlcpy(n->font, "DejaVu Sans 10", sizeof(n->font));
    n->color[0] = n->color[1] = n->color[2] = 0;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_watermark_params_v6_t);
    *new_version = 6;
    return 0;
  }
  else if(old_version == 2)
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
      dt_iop_watermark_base_scale_v2_t sizeto;
      char filename[64];
    } dt_iop_watermark_params_v2_t;

    const dt_iop_watermark_params_v2_t *o = (dt_iop_watermark_params_v2_t *)old_params;
    dt_iop_watermark_params_v6_t *n = malloc(sizeof(dt_iop_watermark_params_v6_t));

    n->opacity = o->opacity;
    n->scale = o->scale;
    n->xoffset = o->xoffset;
    n->yoffset = o->yoffset;
    n->alignment = o->alignment;
    n->rotate = 0.0;
    n->scale_base = DT_SCALE_MAINMENU_IMAGE;
    g_strlcpy(n->filename, o->filename, sizeof(n->filename));
    g_strlcpy(n->text, "", sizeof(n->text));
    g_strlcpy(n->font, "DejaVu Sans 10", sizeof(n->font));
    n->color[0] = n->color[1] = n->color[2] = 0;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_watermark_params_v6_t);
    *new_version = 6;
    return 0;
  }
  else if(old_version == 3)
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
      dt_iop_watermark_base_scale_v2_t sizeto;
      char filename[64];
    } dt_iop_watermark_params_v3_t;

    const dt_iop_watermark_params_v3_t *o = (dt_iop_watermark_params_v3_t *)old_params;
    dt_iop_watermark_params_v6_t *n = malloc(sizeof(dt_iop_watermark_params_v6_t));

    n->opacity = o->opacity;
    n->scale = o->scale;
    n->xoffset = o->xoffset;
    n->yoffset = o->yoffset;
    n->alignment = o->alignment;
    n->rotate = o->rotate;
    n->scale_base = (dt_iop_watermark_base_scale_t)o->sizeto;
    // let scale_img and scale_svg at the default values
    g_strlcpy(n->filename, o->filename, sizeof(n->filename));
    g_strlcpy(n->text, "", sizeof(n->text));
    g_strlcpy(n->font, "DejaVu Sans 10", sizeof(n->font));
    n->color[0] = n->color[1] = n->color[2] = 0;

    *new_params = n;
    *new_params_size = sizeof(dt_iop_watermark_params_v6_t);
    *new_version = 6;
    return 0;
  }
  else if(old_version == 4)
  {
    typedef struct dt_iop_watermark_params_v4_t
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
      dt_iop_watermark_base_scale_v2_t sizeto;
      char filename[64];
      /* simple text */
      char text[64];
      /* text color */
      float color[3];
      /* text font */
      char font[64];
    } dt_iop_watermark_params_v4_t;

    const dt_iop_watermark_params_v4_t *o = (dt_iop_watermark_params_v4_t *)old_params;
    dt_iop_watermark_params_v6_t *n =
      malloc(sizeof(dt_iop_watermark_params_v6_t));

    n->opacity = o->opacity;
    n->scale = o->scale;
    n->xoffset = o->xoffset;
    n->yoffset = o->yoffset;
    n->alignment = o->alignment;
    n->rotate = o->rotate;
    n->scale_base = (dt_iop_watermark_base_scale_t)o->sizeto;
    // let scale_img and scale_svg at the default values
    g_strlcpy(n->filename, o->filename, sizeof(n->filename));
    g_strlcpy(n->text, o->text, sizeof(n->text));
    g_strlcpy(n->font, o->font, sizeof(n->font));
    n->color[0] = o->color[0];
    n->color[1] = o->color[1];
    n->color[2] = o->color[2];

    *new_params = n;
    *new_params_size = sizeof(dt_iop_watermark_params_v6_t);
    *new_version = 6;
    return 0;
  }
  else if(old_version == 5)
  {
    typedef struct dt_iop_watermark_params_v5_t
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
      dt_iop_watermark_base_scale_v2_t sizeto;
      char filename[64];
      /* simple text */
      char text[512];
      /* text color */
      float color[3];
      /* text font */
      char font[64];
    } dt_iop_watermark_params_v5_t;

    const dt_iop_watermark_params_v5_t *o = (dt_iop_watermark_params_v5_t *)old_params;
    dt_iop_watermark_params_v6_t *n = malloc(sizeof(dt_iop_watermark_params_v6_t));

    n->opacity = o->opacity;
    n->scale = o->scale;
    n->xoffset = o->xoffset;
    n->yoffset = o->yoffset;
    n->alignment = o->alignment;
    n->rotate = o->rotate;
    n->scale_base = (dt_iop_watermark_base_scale_t)o->sizeto;
    // let scale_img and scale_svg at the default values
    g_strlcpy(n->filename, o->filename, sizeof(n->filename));
    g_strlcpy(n->text, o->text, sizeof(n->text));
    g_strlcpy(n->font, o->font, sizeof(n->font));
    n->color[0] = o->color[0];
    n->color[1] = o->color[1];
    n->color[2] = o->color[2];

    *new_params = n;
    *new_params_size = sizeof(dt_iop_watermark_params_v6_t);
    *new_version = 6;
    return 0;
  }
  return 1;
}


const char *name()
{
  return _("watermark");
}

const char **description(dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("overlay an SVG watermark like a signature on the image"),
                                      _("creative"),
                                      _("non-linear, RGB, display-referred"),
                                      _("non-linear, RGB"),
                                      _("non-linear, RGB, display-referred"));
}

int flags()
{
  return IOP_FLAGS_INCLUDE_IN_STYLES | IOP_FLAGS_SUPPORTS_BLENDING;
}

int default_group()
{
  return IOP_GROUP_EFFECT | IOP_GROUP_EFFECTS;
}

int operation_tags()
{
  return IOP_TAG_DECORATION;
}

dt_iop_colorspace_type_t default_colorspace(dt_iop_module_t *self,
                                            dt_dev_pixelpipe_t *pipe,
                                            dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

// sets text / color / font widgets sensitive based on watermark file type
static void _text_color_font_set_sensitive(dt_iop_watermark_gui_data_t *g, gchar *filename)
{
  const gchar *extension = strrchr(filename, '.');
  if(extension)
  {
    const gboolean active = !g_ascii_strcasecmp(extension, ".svg");
    gtk_widget_set_sensitive(GTK_WIDGET(g->colorpick), active);
    gtk_widget_set_sensitive(GTK_WIDGET(g->color_picker_button), active);
    gtk_widget_set_sensitive(GTK_WIDGET(g->text), active);
    gtk_widget_set_sensitive(GTK_WIDGET(g->fontsel), active);
  }
}

static void _combo_box_set_active_text(dt_iop_watermark_gui_data_t *g, gchar *text)
{
  int i = 0;
  for(const GList *iter = g->watermarks_filenames; iter; iter = g_list_next(iter))
  {
    if(!g_strcmp0((gchar *)iter->data, text))
    {
      dt_bauhaus_combobox_set(g->watermarks, i);
      _text_color_font_set_sensitive(g, text);
      return;
    }
    i++;
  }
}

// replace < and > with &lt; and &gt;. any more? Yes! & -> &amp;
static gchar *_string_escape(const gchar *string)
{
  gchar *result, *result_old;
  result = dt_util_str_replace(string, "&", "&amp;");

  result_old = result;
  result = dt_util_str_replace(result_old, "<", "&lt;");
  g_free(result_old);

  result_old = result;
  result = dt_util_str_replace(result_old, ">", "&gt;");
  g_free(result_old);

  return result;
}

static gchar *_string_substitute(gchar *string, const gchar *search, const gchar *replace)
{
  gchar *_replace = _string_escape(replace);
  gchar *result = dt_util_str_replace(string, search, _replace);
  g_free(_replace);
  g_free(string);  // dt_util_str_replace always returns a new string, and we don't need the original after this func
  return result;
}

static gchar *_watermark_get_svgdoc(dt_iop_module_t *self, dt_iop_watermark_data_t *data,
                                    const dt_image_t *image, const gchar *filename)
{
  gchar *svgdata = NULL;
  gsize length = 0;
  if(g_file_get_contents(filename, &svgdata, &length, NULL))
  {
    // File is loaded lets substitute strings if found...
    // Simple text from watermark module
    gchar buffer[1024];

    // substitute $(WATERMARK_TEXT)
    if(data->text[0])
    {
      g_strlcpy(buffer, data->text, sizeof(buffer));
      svgdata = _string_substitute(svgdata, "$(WATERMARK_TEXT)", buffer);
    }
    // apply font style substitutions
    PangoFontDescription *font = pango_font_description_from_string(data->font);
    const PangoStyle font_style = pango_font_description_get_style(font);
    const int font_weight = (int)pango_font_description_get_weight(font);

    g_strlcpy(buffer, pango_font_description_get_family(font), sizeof(buffer));
    svgdata = _string_substitute(svgdata, "$(WATERMARK_FONT_FAMILY)", buffer);

    switch(font_style)
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
    svgdata = _string_substitute(svgdata, "$(WATERMARK_FONT_STYLE)", buffer);

    g_snprintf(buffer, sizeof(buffer), "%d", font_weight);
    svgdata = _string_substitute(svgdata, "$(WATERMARK_FONT_WEIGHT)", buffer);

    pango_font_description_free(font);

    // watermark color
    GdkRGBA c = { data->color[0], data->color[1], data->color[2], 1.0f };
    g_strlcpy(buffer, gdk_rgba_to_string(&c), sizeof(buffer));
    svgdata = _string_substitute(svgdata, "$(WATERMARK_COLOR)", buffer);

    // standard calculation on the remaining variables
    const int32_t flags = dt_lib_export_metadata_get_conf_flags();
    dt_variables_params_t *params;
    dt_variables_params_init(&params);
    char image_path[PATH_MAX] = { 0 };
    dt_image_full_path(image->id, image_path, sizeof(image_path), NULL);
    params->filename = image_path;
    params->jobcode = "infos";
    params->use_html_newline = TRUE;
    params->sequence = 0;
    params->imgid = image->id;
    dt_variables_set_tags_flags(params, flags);
    gchar *svgdoc = dt_variables_expand(params, svgdata, FALSE);  // returns a new string
    dt_variables_params_destroy(params);
    g_free(svgdata);  // free the old one
    svgdata = svgdoc; // and make the expanded string our result
  }
  return svgdata;
}

void process(dt_iop_module_t *self,
             dt_dev_pixelpipe_iop_t *piece,
             const void *const ivoid,
             void *const ovoid,
             const dt_iop_roi_t *const roi_in,
             const dt_iop_roi_t *const roi_out)
{
  dt_iop_watermark_data_t *data = piece->data;
  float *in = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;
  const float angle = (M_PI / 180) * (-data->rotate);

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
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
    return;
  }

  // find out the watermark type
  dt_iop_watermark_type_t type;
  const gchar *extension = strrchr(data->filename, '.');
  if(extension)
  {
    if(!g_ascii_strcasecmp(extension, ".svg"))
      type = DT_WTM_SVG;
    else if(!g_ascii_strcasecmp(extension, ".png"))
      type = DT_WTM_PNG;
    else // this should not happen
    {
      dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
      return;
    }
  }
  else
  {
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
    return;
  }

  /* Load svg if not loaded */
  gchar *svgdoc = NULL;
  if(type == DT_WTM_SVG)
  {
    svgdoc = _watermark_get_svgdoc(self, data, &piece->pipe->image, filename);
    if(!svgdoc)
    {
      dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
      return;
    }
  }

  /* setup stride for performance */
  const int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, roi_out->width);
  if(stride == -1)
  {
    dt_print(DT_DEBUG_ALWAYS, "[watermark] cairo stride error");
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
    return;
  }

  /* create a cairo memory surface that is later used for reading watermark overlay data */
  guint8 *image = (guint8 *)g_malloc0_n(roi_out->height, stride);
  cairo_surface_t *surface = cairo_image_surface_create_for_data(image, CAIRO_FORMAT_ARGB32, roi_out->width,
                                                                 roi_out->height, stride);
  if((cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) || (image == NULL))
  {
    dt_print(DT_DEBUG_ALWAYS, "[watermark] cairo surface error: %s",
             cairo_status_to_string(cairo_surface_status(surface)));
    g_free(image);
    dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
    return;
  }

  // rsvg (or some part of cairo which is used underneath) isn't thread safe, for example when handling fonts
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);

  RsvgHandle *svg = NULL;
  if(type == DT_WTM_SVG)
  {
    /* create the rsvghandle from parsed svg data */
    GError *error = NULL;
    svg = rsvg_handle_new_from_data((const guint8 *)svgdoc, strlen(svgdoc), &error);
    g_free(svgdoc);
    if(!svg || error)
    {
      cairo_surface_destroy(surface);
      g_free(image);
      dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
      dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
      dt_print(DT_DEBUG_ALWAYS, "[watermark] error processing svg file: %s", error->message);
      g_error_free(error);
      return;
    }
  }

  // we use a second surface
  guint8 *image_two = NULL;
  cairo_surface_t *surface_two = NULL;

  /* get the dimension of svg or png */
  RsvgDimensionData dimension;
  switch(type)
  {
    case DT_WTM_SVG:
      dimension = dt_get_svg_dimension(svg);
      break;
    case DT_WTM_PNG:
      // load png into surface 2
      surface_two = cairo_image_surface_create_from_png(filename);
      if((cairo_surface_status(surface_two) != CAIRO_STATUS_SUCCESS))
      {
        dt_print(DT_DEBUG_ALWAYS, "[watermark] cairo png surface 2 error: %s",
                 cairo_status_to_string(cairo_surface_status(surface_two)));
        cairo_surface_destroy(surface);
        g_free(image);
        dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
        dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
        return;
      }
      dimension.width = cairo_image_surface_get_width(surface_two);
      dimension.height = cairo_image_surface_get_height(surface_two);
      break;
  }

  // if no text is given dimensions are null
  if(!dimension.width) dimension.width = 1;
  if(!dimension.height) dimension.height = 1;

  //  width/height of current (possibly cropped) image
  const float iw = piece->buf_in.width;
  const float ih = piece->buf_in.height;
  const float uscale = data->scale / 100.0f; // user scale, from GUI in percent

  // wbase, hbase are the base width and height, this is the
  // multiplicator used for the offset computing scale is the scale of
  // the watermark itself and is used only to render it.
  // sbase is used for scale calculation in the larger/smaller modes
  float wbase, hbase, scale, sbase;

  // in larger/smaller (legacy) side mode, set wbase and hbase to the largest
  // or smallest side of the image
  const float larger = dimension.width > dimension.height
    ? (float)dimension.width
    : (float)dimension.height;

  // set the base width and height to either large or smaller
  // border of current image and calculate scale using either
  // marker (SVG object) width or height
  switch (data->scale_base)
  {
    case DT_SCALE_MAINMENU_LARGER_BORDER:
      sbase = wbase = hbase = (iw > ih) ? iw : ih;
      scale = sbase / larger;
      break;
    case DT_SCALE_MAINMENU_SMALLER_BORDER:
      sbase = wbase = hbase = (iw < ih) ? iw : ih;
      scale = sbase / larger;
      break;
    case DT_SCALE_MAINMENU_MARKERHEIGHT:
      wbase = iw;
      sbase = hbase = ih;
      scale = sbase / dimension.height;
      break;
    case DT_SCALE_MAINMENU_ADVANCED:
      wbase = iw;
      hbase = ih;
      if (data->scale_img == DT_SCALE_IMG_WIDTH)
      {
        sbase = iw;
        scale = (data->scale_svg == DT_SCALE_SVG_WIDTH) ? sbase / dimension.width : sbase / dimension.height;
      }
      else if (data->scale_img == DT_SCALE_IMG_HEIGHT)
      {
        sbase = ih;
        scale = (data->scale_svg == DT_SCALE_SVG_WIDTH) ? sbase / dimension.width : sbase / dimension.height;
      }
      else if (data->scale_img == DT_SCALE_IMG_LARGER)
      {
        sbase = (iw > ih) ? iw : ih;
        scale = (data->scale_svg == DT_SCALE_SVG_WIDTH) ? sbase / dimension.width : sbase / dimension.height;
      }
      else // data->scale_img == DT_SCALE_IMG_SMALLER
      {
        sbase = (iw < ih) ? iw : ih;
        scale = (data->scale_svg == DT_SCALE_SVG_WIDTH) ? sbase / dimension.width : sbase / dimension.height;
      }
      break;

    // default to "image" mode
    case DT_SCALE_MAINMENU_IMAGE:
    default:
      // in image mode, the wbase and hbase are just the image width and height
      wbase = iw;
      hbase = ih;
      if(dimension.width > dimension.height)
        scale = iw / dimension.width;
      else
        scale = ih / dimension.height;
  }

  scale *= roi_out->scale;
  scale *= uscale;

  // compute the width and height of the SVG object in image
  // dimension. This is only used to properly layout the watermark
  // based on the alignment.

  float svg_width, svg_height;

  // help to reduce the number of if clauses
  gboolean svg_calc_heightfromwidth;   // calculate svg_height from svg_width if TRUE
                                       // calculate svg_width from svg_height if FALSE
  float svg_calc_base;                 // this value is used as svg_width or svg_height,
                                       // depending on svg_calc_heightfromwidth

  switch (data->scale_base)
  {
    case DT_SCALE_MAINMENU_LARGER_BORDER:
      svg_calc_base = ((iw > ih) ? iw : ih) * uscale;
      svg_calc_heightfromwidth = (dimension.width > dimension.height) ? TRUE : FALSE;
      break;
    case DT_SCALE_MAINMENU_SMALLER_BORDER:
      svg_calc_base = ((iw < ih) ? iw : ih) * uscale;
      svg_calc_heightfromwidth = (dimension.width > dimension.height) ? TRUE : FALSE;
      break;
    case DT_SCALE_MAINMENU_MARKERHEIGHT:
      svg_calc_base = ih * uscale;
      svg_calc_heightfromwidth = FALSE;
      break;
    case DT_SCALE_MAINMENU_ADVANCED:
      if (data->scale_img == DT_SCALE_IMG_WIDTH)
      {
        svg_calc_base = iw * uscale;
        svg_calc_heightfromwidth = (data->scale_svg == DT_SCALE_SVG_WIDTH) ? TRUE : FALSE;
      }
      else if (data->scale_img == DT_SCALE_IMG_HEIGHT)
      {
        svg_calc_base = ih * uscale;
        svg_calc_heightfromwidth = (data->scale_svg == DT_SCALE_SVG_WIDTH) ? TRUE : FALSE;
      }
      else if (data->scale_img == DT_SCALE_IMG_LARGER)
      {
        svg_calc_base = ((iw > ih) ? iw : ih) * uscale;
        svg_calc_heightfromwidth = (data->scale_svg == DT_SCALE_SVG_WIDTH) ? TRUE : FALSE;
      }
      else // data->scale_img == DT_SCALE_IMG_SMALLER
      {
        svg_calc_base = ((iw < ih) ? iw : ih) * uscale;
        svg_calc_heightfromwidth = (data->scale_svg == DT_SCALE_SVG_WIDTH) ? TRUE : FALSE;
      }
      break;

    // default to "image" mode
    case DT_SCALE_MAINMENU_IMAGE:
    default:
      if(dimension.width > dimension.height)
      {
        svg_calc_base = iw * uscale;
        svg_calc_heightfromwidth = TRUE;
      }
      else
      {
        svg_calc_base = ih * uscale;
        svg_calc_heightfromwidth = FALSE;
      }
  }

  if(svg_calc_heightfromwidth)
  {
    // calculate svg_height from svg_width
    svg_width = svg_calc_base;
    svg_height = dimension.height * (svg_width / dimension.width);
  }
  else
  {
    // calculate svg_width from svg_height
    svg_height = svg_calc_base;
    svg_width = dimension.width * (svg_height / dimension.height);
  }


  /* For the rotation we need an extra cairo image as rotations are
     buggy via rsvg_handle_render_cairo.  distortions and blurred
     images are obvious but you also can easily have crashes.
  */

  float svg_offset_x = 0;
  float svg_offset_y = 0;
  if(type == DT_WTM_SVG)
  {
    /* the svg_offsets allow safe text boxes as they might render out of the dimensions */
    svg_offset_x = ceilf(3.0f * scale);
    svg_offset_y = ceilf(3.0f * scale);

    const int watermark_width  = (int)((dimension.width  * scale) + 3* svg_offset_x);
    const int watermark_height = (int)((dimension.height * scale) + 3* svg_offset_y) ;

    const int stride_two = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, watermark_width);
    image_two = (guint8 *)g_malloc0_n(watermark_height, stride_two);
    surface_two = cairo_image_surface_create_for_data(image_two, CAIRO_FORMAT_ARGB32, watermark_width,
                                                                   watermark_height, stride_two);
    if((cairo_surface_status(surface_two) != CAIRO_STATUS_SUCCESS) || (image_two == NULL))
    {
      dt_print(DT_DEBUG_ALWAYS, "[watermark] cairo surface 2 error: %s",
               cairo_status_to_string(cairo_surface_status(surface_two)));
      cairo_surface_destroy(surface);
      g_object_unref(svg);
      g_free(image);
      g_free(image_two);
      dt_iop_image_copy_by_size(ovoid, ivoid, roi_out->width, roi_out->height, ch);
      dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
      return;
    }
  }

  /* create cairo context and setup transformation/scale */
  cairo_t *cr = cairo_create(surface);
  /* create cairo context for the scaled watermark */
  cairo_t *cr_two = cairo_create(surface_two);

  // compute bounding box of rotated watermark
  const float bb_width = fabsf(svg_width * cosf(angle)) + fabsf(svg_height * sinf(angle));
  const float bb_height = fabsf(svg_width * sinf(angle)) + fabsf(svg_height * cosf(angle));
  const float bX = bb_width / 2.0f - svg_width / 2.0f;
  const float bY = bb_height / 2.0f - svg_height / 2.0f;

  // compute translation for the given alignment in image dimension

  float ty = 0, tx = 0;
  if(data->alignment >= 0 && data->alignment < 3) // Align to verttop
    ty = bY;
  else if(data->alignment >= 3 && data->alignment < 6) // Align to vertcenter
    ty = (ih / 2.0f) - (svg_height / 2.0f);
  else if(data->alignment >= 6 && data->alignment < 9) // Align to vertbottom
    ty = ih - svg_height - bY;

  if(data->alignment == 0 || data->alignment == 3 || data->alignment == 6)
    tx = bX;
  else if(data->alignment == 1 || data->alignment == 4 || data->alignment == 7)
    tx = (iw / 2.0f) - (svg_width / 2.0f);
  else if(data->alignment == 2 || data->alignment == 5 || data->alignment == 8)
    tx = iw - svg_width - bX;

  // translate to position
  cairo_translate(cr, -roi_in->x, -roi_in->y);

  // add translation for the given value in GUI (xoffset,yoffset)
  tx += data->xoffset * wbase;
  ty += data->yoffset * hbase;

  cairo_translate(cr, tx * roi_out->scale, ty * roi_out->scale);

  // compute the center of the svg to rotate from the center
  const float cX = svg_width / 2.0f * roi_out->scale;
  const float cY = svg_height / 2.0f * roi_out->scale;

  cairo_translate(cr, cX, cY);
  cairo_rotate(cr, angle);
  cairo_translate(cr, -cX, -cY);

  // now set proper scale and translationfor the watermark itself
  cairo_translate(cr_two, svg_offset_x, svg_offset_y);

  switch(type)
  {
    case DT_WTM_SVG:
      cairo_scale(cr_two, scale, scale);
      /* render svg into surface*/
      dt_render_svg(svg, cr_two, dimension.width, dimension.height, 0, 0);
      break;
    case DT_WTM_PNG:
      cairo_scale(cr, scale, scale);
      break;
  }
  cairo_surface_flush(surface_two);

  // paint the watermark
  cairo_set_source_surface(cr, surface_two, -svg_offset_x, -svg_offset_y);
  cairo_paint(cr);

  // no more non-thread safe rsvg usage
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  cairo_destroy(cr);
  cairo_destroy(cr_two);

  /* ensure that all operations on surface finishing up */
  cairo_surface_flush(surface);

  /* render surface on output */
  guint8 *sd = image;
  const float opacity = data->opacity / 100.0f;
  DT_OMP_FOR()
  for(int j = 0; j < roi_out->height * roi_out->width; j++)
  {
    float *const i = in + ch*j;
    float *const o = out + ch*j;
    guint8 *const s = sd + 4*j;
    const float alpha = (s[3] / 255.0f) * opacity;
    /* svg uses a premultiplied alpha, so only use opacity for the blending */
    o[0] = ((1.0f - alpha) * i[0]) + (opacity * (s[2] / 255.0f));
    o[1] = ((1.0f - alpha) * i[1]) + (opacity * (s[1] / 255.0f));
    o[2] = ((1.0f - alpha) * i[2]) + (opacity * (s[0] / 255.0f));
    o[3] = in[3];
    }

  /* clean up */
  cairo_surface_destroy(surface);
  cairo_surface_destroy(surface_two);
  g_free(image);
  if(type == DT_WTM_SVG)
  {
    g_free(image_two);
    g_object_unref(svg);
  }

}

static void _watermark_callback(GtkWidget *tb, dt_iop_module_t *self)
{
  dt_iop_watermark_gui_data_t *g = self->gui_data;

  if(darktable.gui->reset) return;
  dt_iop_watermark_params_t *p = self->params;
  memset(p->filename, 0, sizeof(p->filename));
  int n = dt_bauhaus_combobox_get(g->watermarks);
  g_strlcpy(p->filename, (char *)g_list_nth_data(g->watermarks_filenames, n), sizeof(p->filename));
  _text_color_font_set_sensitive(g, p->filename);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void color_picker_apply(dt_iop_module_t *self, GtkWidget *picker,
                        dt_dev_pixelpipe_t *pipe)
{
  dt_iop_watermark_gui_data_t *g = self->gui_data;
  dt_iop_watermark_params_t *p = self->params;

  if(fabsf(p->color[0] - self->picked_color[0]) < 0.0001f
     && fabsf(p->color[1] - self->picked_color[1]) < 0.0001f
     && fabsf(p->color[2] - self->picked_color[2]) < 0.0001f)
  {
    // interrupt infinite loops
    return;
  }

  GdkRGBA c = {.red   = self->picked_color[0],
               .green = self->picked_color[1],
               .blue  = self->picked_color[2],
               .alpha = 1.0 };

  p->color[0] = self->picked_color[0];
  p->color[1] = self->picked_color[1];
  p->color[2] = self->picked_color[2];
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->colorpick), &c);

  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _load_watermarks(const char *basedir, dt_iop_watermark_gui_data_t *g)
{
  GList *files = NULL;
  char *watermarks_dir = g_build_filename(basedir, "watermarks", NULL);
  GDir *dir = g_dir_open(watermarks_dir, 0, NULL);
  if(dir)
  {
    const gchar *d_name;
    while((d_name = g_dir_read_name(dir)))
      files = g_list_prepend(files, g_strdup(d_name));
    g_dir_close(dir);
  }

  files = g_list_sort(files, (GCompareFunc)g_strcmp0);
  for(GList *iter = files; iter; iter = g_list_next(iter))
  {
    char *filename = iter->data;
    gchar *extension = strrchr(filename, '.');
    if(extension)
    {
      // we add only supported file formats to the list
      if(!g_ascii_strcasecmp(extension, ".svg") || !g_ascii_strcasecmp(extension, ".png"))
      {
        // remember the whole filename for later
        g->watermarks_filenames = g_list_append(g->watermarks_filenames, g_strdup(filename));
        // ... and build string shown in the gui
        *extension = '\0';
        extension++;
        gchar *text = g_strdup_printf("%s (%s)", filename, extension);
        dt_bauhaus_combobox_add(g->watermarks, text);
        g_free(text);
      }
    }
  }

  g_list_free_full(files, g_free);
  g_free(watermarks_dir);
}

static void _refresh_watermarks(dt_iop_module_t *self)
{
  dt_iop_watermark_gui_data_t *g = self->gui_data;
  dt_iop_watermark_params_t *p = self->params;

  g_signal_handlers_block_by_func(g->watermarks, _watermark_callback, self);

  // Clear combobox...
  dt_bauhaus_combobox_clear(g->watermarks);
  g_list_free_full(g->watermarks_filenames, g_free);
  g->watermarks_filenames = NULL;

  // check watermarkdir and update combo with entries...
  gchar configdir[PATH_MAX] = { 0 };
  gchar datadir[PATH_MAX] = { 0 };
  dt_loc_get_datadir(datadir, sizeof(datadir));
  dt_loc_get_user_config_dir(configdir, sizeof(configdir));

  _load_watermarks(datadir, g);
  _load_watermarks(configdir, g);

  _combo_box_set_active_text(g, p->filename);

  g_signal_handlers_unblock_by_func(g->watermarks, _watermark_callback, self);
}

static void _refresh_callback(GtkWidget *tb, dt_iop_module_t *self)
{
  _refresh_watermarks(self);
}

static void _alignment_callback(GtkWidget *tb, dt_iop_module_t *self)
{
  int index = -1;
  dt_iop_watermark_gui_data_t *g = self->gui_data;

  if(darktable.gui->reset) return;
  dt_iop_watermark_params_t *p = self->params;


  for(int i = 0; i < 9; i++)
  {
    /* block signal handler */
    g_signal_handlers_block_by_func(g->align[i], _alignment_callback, self);

    if(GTK_WIDGET(g->align[i]) == tb)
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->align[i]), TRUE);
      index = i;
    }
    else
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->align[i]), FALSE);

    /* unblock signal handler */
    g_signal_handlers_unblock_by_func(g->align[i], _alignment_callback, self);
  }
  p->alignment = index;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _text_callback(GtkWidget *entry, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_watermark_params_t *p = self->params;
  g_strlcpy(p->text, gtk_entry_get_text(GTK_ENTRY(entry)), sizeof(p->text));
  dt_conf_set_string("plugins/darkroom/watermark/text", p->text);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void _colorpick_color_set(GtkColorButton *widget, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_watermark_params_t *p = self->params;

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

static void _fontsel_callback(GtkWidget *button, dt_iop_module_t *self)
{
  if(darktable.gui->reset) return;
  dt_iop_watermark_params_t *p = self->params;

  gchar *fontname = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(button));
  g_strlcpy(p->font, fontname, sizeof(p->font));
  g_free(fontname);
  dt_conf_set_string("plugins/darkroom/watermark/font", p->font);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params(dt_iop_module_t *self,
                   dt_iop_params_t *p1,
                   dt_dev_pixelpipe_t *pipe,
                   dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)p1;
  dt_iop_watermark_data_t *d = piece->data;

  d->opacity = p->opacity;
  d->scale = p->scale;
  d->rotate = p->rotate;
  d->xoffset = p->xoffset;
  d->yoffset = p->yoffset;
  d->alignment = p->alignment;
  d->scale_base = p->scale_base;
  d->scale_img = p->scale_img;
  d->scale_svg = p->scale_svg;
  memset(d->filename, 0, sizeof(d->filename));
  g_strlcpy(d->filename, p->filename, sizeof(d->filename));
  memset(d->text, 0, sizeof(d->text));
  g_strlcpy(d->text, p->text, sizeof(d->text));
  for(int k=0; k<3; k++)
    d->color[k] = p->color[k];
  memset(d->font, 0, sizeof(d->font));
  g_strlcpy(d->font, p->font, sizeof(d->font));

// dt_print(DT_DEBUG_ALWAYS, "Commit params: %s...",d->filename);
}

void init_pipe(dt_iop_module_t *self,
               dt_dev_pixelpipe_t *pipe,
               dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_watermark_data_t));
}

void cleanup_pipe(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}


void gui_update(dt_iop_module_t *self)
{
  dt_iop_watermark_gui_data_t *g = self->gui_data;
  dt_iop_watermark_params_t *p = self->params;
  for(int i = 0; i < 9; i++)
  {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->align[i]), FALSE);
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->align[p->alignment]), TRUE);
  _combo_box_set_active_text(g, p->filename);
  gtk_entry_set_text(GTK_ENTRY(g->text), p->text);
  GdkRGBA color = (GdkRGBA){.red = p->color[0], .green = p->color[1], .blue = p->color[2], .alpha = 1.0 };
  gtk_color_chooser_set_rgba(GTK_COLOR_CHOOSER(g->colorpick), &color);
  gtk_font_chooser_set_font(GTK_FONT_CHOOSER(g->fontsel), p->font);

  if(p->scale_base == DT_SCALE_MAINMENU_ADVANCED)
  {
    gtk_widget_set_visible(GTK_WIDGET(g->scale_img), TRUE);
    gtk_widget_set_visible(GTK_WIDGET(g->scale_svg), TRUE);
  }
  else
  {
    gtk_widget_set_visible(GTK_WIDGET(g->scale_img), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(g->scale_svg), FALSE);
  }
}

void gui_changed(dt_iop_module_t *self,
                 GtkWidget *w,
                 void *previous)
{
  dt_iop_watermark_gui_data_t *g = self->gui_data;
  dt_iop_watermark_params_t *p = self->params;

  if(w == g->scale_base)
  {
    if(p->scale_base == DT_SCALE_MAINMENU_ADVANCED)
    {
      gtk_widget_set_visible(GTK_WIDGET(g->scale_img), TRUE);
      gtk_widget_set_visible(GTK_WIDGET(g->scale_svg), TRUE);
    }
    else
    {
      gtk_widget_set_visible(GTK_WIDGET(g->scale_img), FALSE);
      gtk_widget_set_visible(GTK_WIDGET(g->scale_svg), FALSE);
    }
  }
}

void init(dt_iop_module_t *self)
{
  dt_iop_default_init(self);

  dt_iop_watermark_params_t *d = self->default_params;

  g_strlcpy(d->filename, "darktable.svg", sizeof(d->filename));
  g_strlcpy(d->font, "DejaVu Sans 10", sizeof(d->font));
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_watermark_gui_data_t *g = IOP_GUI_ALLOC(watermark);
  dt_iop_watermark_params_t *p = self->params;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, DT_BAUHAUS_SPACE);

  GtkGrid *grid = GTK_GRID(gtk_grid_new());
  gtk_grid_set_row_spacing(grid, DT_BAUHAUS_SPACE);
  gtk_grid_set_column_spacing(grid, DT_PIXEL_APPLY_DPI(10));
  int line = 0;

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
  g->refresh = dtgtk_button_new(dtgtk_cairo_paint_refresh, 0, NULL);

  gtk_grid_attach(grid, label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(grid, g->watermarks, label, GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(grid, g->refresh, g->watermarks, GTK_POS_RIGHT, 1, 1);

  // Simple text
  label = dt_ui_label_new(_("text"));
  g->text = dt_action_entry_new(DT_ACTION(self), N_("text"), G_CALLBACK(_text_callback), self,
                                _("text string, tag: $(WATERMARK_TEXT)\n"
                                  "use $(NL) to insert a line break"),
                                dt_conf_get_string_const("plugins/darkroom/watermark/text"));
  dt_gtkentry_setup_completion(GTK_ENTRY(g->text), dt_gtkentry_get_default_path_compl_list());
  gtk_entry_set_placeholder_text(GTK_ENTRY(g->text), _("content"));
  gtk_grid_attach(grid, label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(grid, g->text, label, GTK_POS_RIGHT, 2, 1);

  // Text font
  label = dtgtk_reset_label_new(_("font"), self, &p->font, sizeof(p->font));
  const char *str = dt_conf_get_string_const("plugins/darkroom/watermark/font");
  g->fontsel = gtk_font_button_new_with_font(str==NULL?"DejaVu Sans 10":str);
  GtkWidget *child = dt_gui_container_first_child(GTK_CONTAINER(gtk_bin_get_child(GTK_BIN(g->fontsel))));
  gtk_label_set_ellipsize(GTK_LABEL(child), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_tooltip_text(g->fontsel, _("text font, tags:\n$(WATERMARK_FONT_FAMILY)\n"
                                            "$(WATERMARK_FONT_STYLE)\n$(WATERMARK_FONT_WEIGHT)"));
  gtk_font_button_set_show_size (GTK_FONT_BUTTON(g->fontsel), FALSE);

  gtk_grid_attach(grid, label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(grid, g->fontsel, label, GTK_POS_RIGHT, 2, 1);

  // Watermark color
  float red = dt_conf_get_float("plugins/darkroom/watermark/color_red");
  float green = dt_conf_get_float("plugins/darkroom/watermark/color_green");
  float blue = dt_conf_get_float("plugins/darkroom/watermark/color_blue");
  GdkRGBA color = (GdkRGBA){.red = red, .green = green, .blue = blue, .alpha = 1.0 };

  label = dtgtk_reset_label_new(_("color"), self, &p->color, 3 * sizeof(float));
  g->colorpick = gtk_color_button_new_with_rgba(&color);
  gtk_widget_set_tooltip_text(g->colorpick, _("watermark color, tag:\n$(WATERMARK_COLOR)"));
  gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(g->colorpick), FALSE);
  gtk_color_button_set_title(GTK_COLOR_BUTTON(g->colorpick), _("select watermark color"));
  g->color_picker_button = dt_color_picker_new(self, DT_COLOR_PICKER_POINT, NULL);
  gtk_widget_set_tooltip_text(GTK_WIDGET(g->color_picker_button), _("pick color from image"));
  dt_action_define_iop(self, NULL, N_("pick color"), g->color_picker_button, &dt_action_def_toggle);

  gtk_grid_attach(grid, label, 0, line++, 1, 1);
  gtk_grid_attach_next_to(grid, g->colorpick, label, GTK_POS_RIGHT, 1, 1);
  gtk_grid_attach_next_to(grid, g->color_picker_button, g->colorpick, GTK_POS_RIGHT, 1, 1);

  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(grid), TRUE, TRUE, 0);

  // Add opacity/scale sliders to table
  g->opacity = dt_bauhaus_slider_from_params(self, N_("opacity"));
  dt_bauhaus_slider_set_format(g->opacity, "%");

  gtk_box_pack_start(GTK_BOX(self->widget), dt_ui_section_label_new(C_("section", "placement")), TRUE, TRUE, 0);

  // rotate
  g->rotate = dt_bauhaus_slider_from_params(self, "rotate");
  dt_bauhaus_slider_set_format(g->rotate, "°");

  // scale
  g->scale = dt_bauhaus_slider_from_params(self, N_("scale"));
  dt_bauhaus_slider_set_soft_max(g->scale, 100.0);
  dt_bauhaus_slider_set_format(g->scale, "%");

  // legacy scale on drop-down
  g->scale_base = dt_bauhaus_combobox_from_params(self, "scale_base");
  gtk_widget_set_tooltip_text(g->scale_base, _("choose how to scale the watermark\n"
                                               "• image: scale watermark relative to whole image\n"
                                               "• larger border: scale larger watermark border relative to larger image border\n"
                                               "• smaller border: scale larger watermark border relative to smaller image border\n"
                                               "• height: scale watermark height to image height\n"
                                               "• advanced options: choose watermark and image dimensions independently"));

  // scale image reference
  g->scale_img = dt_bauhaus_combobox_from_params(self, "scale_img");
  gtk_widget_set_tooltip_text(g->scale_img, _("reference image dimension against which to scale the watermark"));

  // scale marker reference
  g->scale_svg = dt_bauhaus_combobox_from_params(self, "scale_svg");
  gtk_widget_set_tooltip_text(g->scale_svg, _("watermark dimension to scale"));

  // Create the 3x3 gtk table toggle button table...
  GtkWidget *bat = gtk_grid_new();
  label = dtgtk_reset_label_new(_("alignment"), self, &p->alignment, sizeof(p->alignment));
  gtk_grid_attach(GTK_GRID(bat), label, 0, 0, 1, 3);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_grid_set_row_spacing(GTK_GRID(bat), DT_PIXEL_APPLY_DPI(3));
  gtk_grid_set_column_spacing(GTK_GRID(bat), DT_PIXEL_APPLY_DPI(3));
  for(int i = 0; i < 9; i++)
  {
    g->align[i] = dtgtk_togglebutton_new(dtgtk_cairo_paint_alignment, (CPF_SPECIAL_FLAG << i), NULL);
    gtk_grid_attach(GTK_GRID(bat), GTK_WIDGET(g->align[i]), 1 + i%3, i/3, 1, 1);
    g_signal_connect(G_OBJECT(g->align[i]), "toggled", G_CALLBACK(_alignment_callback), self);
  }

  gtk_box_pack_start(GTK_BOX(self->widget), bat, FALSE, FALSE, 0);

  // x/y offset
  g->x_offset = dt_bauhaus_slider_from_params(self, "xoffset");
  dt_bauhaus_slider_set_digits(g->x_offset, 3);
  g->y_offset = dt_bauhaus_slider_from_params(self, "yoffset");
  dt_bauhaus_slider_set_digits(g->y_offset, 3);

  // Let's add some tooltips and hook up some signals...
  gtk_widget_set_tooltip_text(g->opacity, _("the opacity of the watermark"));
  gtk_widget_set_tooltip_text(g->scale, _("the scale of the watermark"));
  gtk_widget_set_tooltip_text(g->rotate, _("the rotation of the watermark"));

  _refresh_watermarks(self);

  g_signal_connect(G_OBJECT(g->watermarks), "value-changed", G_CALLBACK(_watermark_callback), self);
  g_signal_connect(G_OBJECT(g->refresh), "clicked", G_CALLBACK(_refresh_callback), self);
  g_signal_connect(G_OBJECT(g->colorpick), "color-set", G_CALLBACK(_colorpick_color_set), self);
  g_signal_connect(G_OBJECT(g->fontsel), "font-set", G_CALLBACK(_fontsel_callback), self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  dt_iop_watermark_gui_data_t *g = self->gui_data;
  g_list_free_full(g->watermarks_filenames, g_free);
  g->watermarks_filenames = NULL;

  IOP_GUI_FREE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
