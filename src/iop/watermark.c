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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "dtgtk/slider.h"
#include "dtgtk/button.h"
#include "dtgtk/togglebutton.h"
#include "dtgtk/resetlabel.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#include <librsvg/rsvg.h>
#include <librsvg/rsvg-cairo.h>

#include "common/metadata.h"
#include "common/utility.h"
#include "common/file_location.h"

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
DT_MODULE(1)

// gchar *checksum = g_compute_checksum_for_data(G_CHECKSUM_MD5,data,length);

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
  char filename[64];
}
dt_iop_watermark_params_t;

typedef struct dt_iop_watermark_data_t
{
  float opacity;
  float scale;
  float xoffset;
  float yoffset;
  int alignment;
  char filename[64];
}
dt_iop_watermark_data_t;

typedef struct dt_iop_watermark_gui_data_t
{
  GtkComboBox *combobox1;		                                             // watermark
  GtkDarktableButton *dtbutton1;	                                         // refresh watermarks...
  GtkDarktableToggleButton *dtba[9];	                                   // Alignment buttons
  GtkDarktableSlider *scale1,*scale2,*scale3,*scale4;      	     // opacity, scale, xoffs, yoffs
}
dt_iop_watermark_gui_data_t;


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
  return IOP_GROUP_EFFECT;
}

int
operation_tags ()
{
  return IOP_TAG_DECORATION;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_iop(self, FALSE, NC_("accel", "refresh"), 0, 0);
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "opacity"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "scale"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "x offset"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "y offset"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_watermark_gui_data_t *g =
      (dt_iop_watermark_gui_data_t*)self->gui_data;

  dt_accel_connect_button_iop(self, "refresh", GTK_WIDGET(g->dtbutton1));
  dt_accel_connect_slider_iop(self, "opacity",
                              GTK_WIDGET(g->scale1));
  dt_accel_connect_slider_iop(self, "scale",
                              GTK_WIDGET(g->scale2));
  dt_accel_connect_slider_iop(self, "x offset",
                              GTK_WIDGET(g->scale3));
  dt_accel_connect_slider_iop(self, "y offset",
                              GTK_WIDGET(g->scale4));
}

static gboolean _combo_box_set_active_text(GtkComboBox *cb,gchar *text)
{
  gboolean found=FALSE;
  gchar *sv=NULL;
  GtkTreeIter iter;
  GtkTreeModel *tm=gtk_combo_box_get_model(cb);
  if(  gtk_tree_model_get_iter_first (tm,&iter) )
  {
    do
    {
      GValue value = { 0, };
      gtk_tree_model_get_value(tm,&iter,0,&value);
      if (G_VALUE_HOLDS_STRING (&value))
        if( (sv=(gchar *)g_value_get_string(&value))!=NULL && strcmp(sv,text)==0)
        {
          gtk_combo_box_set_active_iter(cb, &iter);
          found=TRUE;
          break;
        }
    }
    while( gtk_tree_model_iter_next(tm,&iter) );
  }
  return found;
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

static gchar *_string_substitute(gchar *string,const gchar *search,const gchar *replace)
{
  gchar* _replace = _string_escape(replace);
  gchar* result = dt_util_str_replace(string, search, _replace);
  if(_replace)
    g_free(_replace);
  return result;
}

static gchar * _watermark_get_svgdoc( dt_iop_module_t *self, dt_iop_watermark_data_t *data, const dt_image_t *image)
{
  gsize length;

  gchar *svgdoc=NULL;
  gchar configdir[1024],datadir[1024], *filename;
  dt_loc_get_datadir(datadir, 1024);
  dt_loc_get_user_config_dir(configdir, 1024);
  g_strlcat(datadir,"/watermarks/",1024);
  g_strlcat(configdir,"/watermarks/",1024);
  g_strlcat(datadir,data->filename,1024);
  g_strlcat(configdir,data->filename,1024);

  if (g_file_test(configdir,G_FILE_TEST_EXISTS))
    filename=configdir;
  else if (g_file_test(datadir,G_FILE_TEST_EXISTS))
    filename=datadir;
  else return NULL;

  gchar *svgdata=NULL;
  char datetime[200];
  struct tm tt = {0};
  if(sscanf(image->exif_datetime_taken,"%d:%d:%d %d:%d:%d",
              &tt.tm_year,
              &tt.tm_mon,
              &tt.tm_mday,
              &tt.tm_hour,
              &tt.tm_min,
              &tt.tm_sec
             ) == 6
     )
  {
    tt.tm_year-=1900;
    tt.tm_mon--;
  }

  if( g_file_get_contents( filename, &svgdata, &length, NULL) )
  {
    // File is loaded lets substitute strings if found...

    // Darktable internal
    svgdoc = _string_substitute(svgdata,"$(DARKTABLE.NAME)",PACKAGE_NAME);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    svgdoc = _string_substitute(svgdata,"$(DARKTABLE.VERSION)",PACKAGE_VERSION);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }

    // Current image ID
    gchar buffer[1024];
    g_snprintf(buffer,1024,"%d",image->id);
    svgdoc = _string_substitute(svgdata,"$(IMAGE.ID)",buffer);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }

    // Current image
    dt_image_print_exif(image,buffer,1024);
    svgdoc = _string_substitute(svgdata,"$(IMAGE.EXIF)",buffer);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }

    // Image exif
    svgdoc = _string_substitute(svgdata,"$(EXIF.DATE)",image->exif_datetime_taken);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
// $(EXIF.DATE.SECOND) -- 00..60
    strftime(datetime, sizeof(datetime), "%S", &tt);
    svgdoc = _string_substitute(svgdata,"$(EXIF.DATE.SECOND)",datetime);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
// $(EXIF.DATE.MINUTE) -- 00..59
    strftime(datetime, sizeof(datetime), "%M", &tt);
    svgdoc = _string_substitute(svgdata,"$(EXIF.DATE.MINUTE)",datetime);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
// $(EXIF.DATE.HOUR) -- 00..23
    strftime(datetime, sizeof(datetime), "%H", &tt);
    svgdoc = _string_substitute(svgdata,"$(EXIF.DATE.HOUR)",datetime);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
// $(EXIF.DATE.HOUR_AMPM) -- 01..12
    strftime(datetime, sizeof(datetime), "%I %p", &tt);
    svgdoc = _string_substitute(svgdata,"$(EXIF.DATE.HOUR_AMPM)",datetime);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
// $(EXIF.DATE.DAY) -- 01..31
    strftime(datetime, sizeof(datetime), "%d", &tt);
    svgdoc = _string_substitute(svgdata,"$(EXIF.DATE.DAY)",datetime);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
// $(EXIF.DATE.MONTH) -- 01..12
    strftime(datetime, sizeof(datetime), "%m", &tt);
    svgdoc = _string_substitute(svgdata,"$(EXIF.DATE.MONTH)",datetime);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
// $(EXIF.DATE.SHORT_MONTH) -- Jan, Feb, .., Dec, localized
    strftime(datetime, sizeof(datetime), "%b", &tt);
    svgdoc = _string_substitute(svgdata,"$(EXIF.DATE.SHORT_MONTH)",datetime);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
// $(EXIF.DATE.LONG_MONTH) -- January, February, .., December, localized
    strftime(datetime, sizeof(datetime), "%B", &tt);
    svgdoc = _string_substitute(svgdata,"$(EXIF.DATE.LONG_MONTH)",datetime);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
// $(EXIF.DATE.SHORT_YEAR) -- 12
    strftime(datetime, sizeof(datetime), "%y", &tt);
    svgdoc = _string_substitute(svgdata,"$(EXIF.DATE.SHORT_YEAR)",datetime);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
// $(EXIF.DATE.LONG_YEAR) -- 2012
    strftime(datetime, sizeof(datetime), "%Y", &tt);
    svgdoc = _string_substitute(svgdata,"$(EXIF.DATE.LONG_YEAR)",datetime);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }

    svgdoc = _string_substitute(svgdata,"$(EXIF.MAKER)",image->exif_maker);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    svgdoc = _string_substitute(svgdata,"$(EXIF.MODEL)",image->exif_model);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    svgdoc = _string_substitute(svgdata,"$(EXIF.LENS)",image->exif_lens);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }

    svgdoc = _string_substitute(svgdata,"$(IMAGE.FILENAME)",image->filename);
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }

    // TODO: auto generate that code?
    GList * res;
    res = dt_metadata_get(image->id, "Xmp.dc.creator", NULL);
    svgdoc = _string_substitute(svgdata,"$(Xmp.dc.creator)",(res?res->data:""));
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    if( res )
    {
      g_free(res->data);
      g_list_free(res);
    }

    res = dt_metadata_get(image->id, "Xmp.dc.publisher", NULL);
    svgdoc = _string_substitute(svgdata,"$(Xmp.dc.publisher)",(res?res->data:""));
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    if( res )
    {
      g_free(res->data);
      g_list_free(res);
    }

    res = dt_metadata_get(image->id, "Xmp.dc.title", NULL);
    svgdoc = _string_substitute(svgdata,"$(Xmp.dc.title)",(res?res->data:""));
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    if( res )
    {
      g_free(res->data);
      g_list_free(res);
    }

    res = dt_metadata_get(image->id, "Xmp.dc.description", NULL);
    svgdoc = _string_substitute(svgdata,"$(Xmp.dc.description)",(res?res->data:""));
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    if( res )
    {
      g_free(res->data);
      g_list_free(res);
    }

    res = dt_metadata_get(image->id, "Xmp.dc.rights", NULL);
    svgdoc = _string_substitute(svgdata,"$(Xmp.dc.rights)",(res?res->data:""));
    if( svgdoc != svgdata )
    {
      g_free(svgdata);
      svgdata = svgdoc;
    }
    if( res )
    {
      g_free(res->data);
      g_list_free(res);
    }

  }
  return svgdoc;
}


void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_watermark_data_t *data = (dt_iop_watermark_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;

  /* Load svg if not loaded */
  gchar *svgdoc = _watermark_get_svgdoc (self, data, &piece->pipe->image);
  if (!svgdoc)
  {
    memcpy(ovoid, ivoid, sizeof(float)*ch*roi_out->width*roi_out->height);
    return;
  }

  /* create the rsvghandle from parsed svg data */
  GError *error = NULL;
  RsvgHandle *svg = rsvg_handle_new_from_data ((const guint8 *)svgdoc,strlen (svgdoc),&error);
  g_free (svgdoc);
  if (!svg || error)
  {
    memcpy(ovoid, ivoid, sizeof(float)*ch*roi_out->width*roi_out->height);
    return;
  }

  /* get the dimension of svg */
  RsvgDimensionData dimension;
  rsvg_handle_get_dimensions (svg,&dimension);

  /* calculate aligment of watermark */
  const float iw=piece->buf_in.width*roi_out->scale;
  const float ih=piece->buf_in.height*roi_out->scale;

  float scale=1.0;
  if ((dimension.width/dimension.height)>1.0)
    scale = iw/dimension.width;
  else
    scale = ih/dimension.height;

  scale *= (data->scale/100.0);

  /* setup stride for performance */
  int stride = cairo_format_stride_for_width (CAIRO_FORMAT_ARGB32,roi_out->width);

  /* create cairo memory surface */
  guint8 *image= (guint8 *)g_malloc (stride*roi_out->height);
  memset (image,0,stride*roi_out->height);
  cairo_surface_t *surface = cairo_image_surface_create_for_data (image,CAIRO_FORMAT_ARGB32,roi_out->width,roi_out->height,stride);
  if (cairo_surface_status(surface)!=	CAIRO_STATUS_SUCCESS)
  {
//   fprintf(stderr,"Cairo surface error: %s\n",cairo_status_to_string(cairo_surface_status(surface)));
    g_free (image);
    memcpy(ovoid, ivoid, sizeof(float)*ch*roi_out->width*roi_out->height);
    return;
  }

  /* create cairo context and setup transformation/scale */
  cairo_t *cr = cairo_create (surface);

  float ty=0,tx=0;
  if( data->alignment >=0 && data->alignment <3) // Align to verttop
    ty=0;
  else if( data->alignment >=3 && data->alignment <6) // Align to vertcenter
    ty=(ih/2.0)-((dimension.height*scale)/2.0);
  else if( data->alignment >=6 && data->alignment <9) // Align to vertbottom
    ty=ih-(dimension.height*scale);

  if( data->alignment == 0 ||  data->alignment == 3 || data->alignment==6 )
    tx=0;
  else if( data->alignment == 1 ||  data->alignment == 4 || data->alignment==7 )
    tx=(iw/2.0)-((dimension.width*scale)/2.0);
  else if( data->alignment == 2 ||  data->alignment == 5 || data->alignment==8 )
    tx=iw-(dimension.width*scale);

  /* translate to position */
  cairo_translate (cr,-roi_in->x,-roi_in->y);
  cairo_translate (cr,tx,ty);

  /* scale */
  cairo_scale (cr,scale,scale);

  /* translate x and y offset */
  cairo_translate (cr,data->xoffset*iw/roi_out->scale,data->yoffset*ih/roi_out->scale);

  /* render svg into surface*/
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  rsvg_handle_render_cairo (svg,cr);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  /* ensure that all operations on surface finishing up */
  cairo_surface_flush (surface);

  /* render surface on output */
  guint8 *sd = image;
  float opacity = data->opacity/100.0;
  /*
  #ifdef _OPENMP
  	#pragma omp parallel for default(none) shared(roi_out, in, out,sd,opacity) schedule(static)
  #endif
  */
  for(int j=0; j<roi_out->height; j++) for(int i=0; i<roi_out->width; i++)
    {
      float alpha = (sd[3]/255.0)*opacity;
      out[0] = ((1.0-alpha)*in[0]) + (alpha*(sd[2]/255.0));
      out[1] = ((1.0-alpha)*in[1]) + (alpha*(sd[1]/255.0));
      out[2] = ((1.0-alpha)*in[2]) + (alpha*(sd[0]/255.0));
      out[3] = in[3];

      out+=ch;
      in+=ch;
      sd+=4;
    }


  /* clean up */
  cairo_surface_destroy (surface);
  g_object_unref (svg);
  g_free (image);

}

static void
watermark_callback(GtkWidget *tb, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_watermark_gui_data_t *g = (dt_iop_watermark_gui_data_t *)self->gui_data;

  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  memset(p->filename,0,64);
  snprintf(p->filename,64,"%s",gtk_combo_box_get_active_text(g->combobox1));
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}


static void refresh_watermarks( dt_iop_module_t *self )
{
  dt_iop_watermark_gui_data_t *g = (dt_iop_watermark_gui_data_t *)self->gui_data;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;

  g_signal_handlers_block_by_func (g->combobox1,watermark_callback,self);

  // Clear combobox...
  GtkTreeModel *model=gtk_combo_box_get_model(g->combobox1);
  gtk_list_store_clear (GTK_LIST_STORE(model));

  // check watermarkdir and update combo with entries...
  int count=0;
  const gchar *d_name = NULL;
  gchar configdir[1024],datadir[1024],filename[2048];
  dt_loc_get_datadir(datadir, 1024);
  dt_loc_get_user_config_dir(configdir, 1024);
  g_strlcat(datadir,"/watermarks",1024);
  g_strlcat(configdir,"/watermarks",1024);

  /* read watermarks from datadir */
  GDir *dir = g_dir_open(datadir, 0, NULL);
  if(dir)
  {
    while((d_name = g_dir_read_name(dir)))
    {
      snprintf(filename, 1024, "%s/%s", datadir, d_name);
      gtk_combo_box_append_text( g->combobox1, d_name );
      count++;
    }
    g_dir_close(dir) ;
  }

  /* read watermarks from user config dir*/
  dir = g_dir_open(configdir, 0, NULL);
  if(dir)
  {
    while((d_name = g_dir_read_name(dir)))
    {
      snprintf(filename, 2048, "%s/%s", configdir, d_name);
      gtk_combo_box_append_text( g->combobox1, d_name );
      count++;
    } 
    g_dir_close(dir) ;
  }

  _combo_box_set_active_text( g->combobox1, p->filename );

  g_signal_handlers_unblock_by_func (g->combobox1,watermark_callback,self);

}

static void
refresh_callback(GtkWidget *tb, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  refresh_watermarks(self);
}




static void
alignment_callback(GtkWidget *tb, gpointer user_data)
{
  int index=-1;
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_watermark_gui_data_t *g = (dt_iop_watermark_gui_data_t *)self->gui_data;

  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;


  for(int i=0; i<9; i++)
  {
    /* block signal handler */
    g_signal_handlers_block_by_func (g->dtba[i],alignment_callback,user_data);

    if( GTK_WIDGET(g->dtba[i]) == tb )
    {
      gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->dtba[i]),TRUE);
      index=i;
    }
    else gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->dtba[i]),FALSE);

    /* unblock signal handler */
    g_signal_handlers_unblock_by_func (g->dtba[i],alignment_callback,user_data);
  }
  p->alignment= index;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
opacity_callback(GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  p->opacity= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
xoffset_callback(GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  p->xoffset= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
yoffset_callback(GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  p->yoffset= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
scale_callback(GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  p->scale= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[watermark] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_watermark_data_t *d = (dt_iop_watermark_data_t *)piece->data;
  d->opacity= p->opacity;
  d->scale= p->scale;
  d->xoffset= p->xoffset;
  d->yoffset= p->yoffset;
  d->alignment= p->alignment;
  memset(d->filename,0,64);
  sprintf(d->filename,"%s",p->filename);

  //fprintf(stderr,"Commit params: %s...\n",d->filename);
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_watermark_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // clean up everything again.
  (void)gegl_node_remove_child(pipe->gegl, piece->input);
  // no free necessary, no data is alloc'ed
#else
  free(piece->data);
#endif
}


void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_watermark_gui_data_t *g = (dt_iop_watermark_gui_data_t *)self->gui_data;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)module->params;
  dtgtk_slider_set_value(g->scale1, p->opacity);
  dtgtk_slider_set_value(g->scale2, p->scale);
  dtgtk_slider_set_value(g->scale3, p->xoffset);
  dtgtk_slider_set_value(g->scale4, p->yoffset);
  gtk_toggle_button_set_active( GTK_TOGGLE_BUTTON(g->dtba[ p->alignment ]), TRUE);
  _combo_box_set_active_text( g->combobox1, p->filename );
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_watermark_params_t));
  module->params_size = sizeof(dt_iop_watermark_params_t);
  module->default_params = malloc(sizeof(dt_iop_watermark_params_t));
  module->default_enabled = 0;
  module->priority = 980; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_watermark_params_t);
  module->gui_data = NULL;
  dt_iop_watermark_params_t tmp = (dt_iop_watermark_params_t)
  {
    100.0,100.0,0.0,0.0,4, {"darktable.svg"}
  }; // opacity,scale,xoffs,yoffs,alignment
  memcpy(module->params, &tmp, sizeof(dt_iop_watermark_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_watermark_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_watermark_gui_data_t));
  dt_iop_watermark_gui_data_t *g = (dt_iop_watermark_gui_data_t *)self->gui_data;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;

  self->widget = gtk_hbox_new(FALSE,0);
  GtkWidget *vbox = gtk_vbox_new(FALSE,DT_GUI_IOP_MODULE_CONTROL_SPACING);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(vbox), TRUE, TRUE, 5);

  GtkWidget *label1 = dtgtk_reset_label_new(_("marker"), self, &p->filename, sizeof(char)*64);
  GtkWidget *label4 = dtgtk_reset_label_new(_("alignment"), self, &p->alignment, sizeof(int));

  // Add the marker combobox
  GtkWidget *hbox= gtk_hbox_new(FALSE,0);
  g->combobox1 = GTK_COMBO_BOX(gtk_combo_box_new_text());
  g->dtbutton1  = DTGTK_BUTTON(dtgtk_button_new(dtgtk_cairo_paint_refresh, 0));
  gtk_box_pack_start(GTK_BOX(hbox),GTK_WIDGET(label1),TRUE,TRUE,0);
  gtk_box_pack_start(GTK_BOX(hbox),GTK_WIDGET(g->combobox1),TRUE,TRUE,0);
  gtk_box_pack_start(GTK_BOX(hbox),GTK_WIDGET(g->dtbutton1),FALSE,FALSE,0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(hbox), TRUE, TRUE, 0);

  // Add opacity/scale sliders to table
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 1.0, p->opacity, 0.5));
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,1.0, 100.0, 1.0, p->scale, 0.5));
  dtgtk_slider_set_format_type(g->scale1,DARKTABLE_SLIDER_FORMAT_PERCENT);
  dtgtk_slider_set_format_type(g->scale2,DARKTABLE_SLIDER_FORMAT_PERCENT);
  dtgtk_slider_set_label(g->scale1,_("opacity"));
  dtgtk_slider_set_unit(g->scale1,"%");
  dtgtk_slider_set_label(g->scale2,_("scale"));
  dtgtk_slider_set_unit(g->scale2,"%");
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);

  // Create the 3x3 gtk table toggle button table...
  GtkTable *bat = GTK_TABLE( gtk_table_new(3,3,TRUE));
  for(int i=0; i<9; i++)
  {
    g->dtba[i] = DTGTK_TOGGLEBUTTON (dtgtk_togglebutton_new (dtgtk_cairo_paint_alignment,CPF_STYLE_FLAT|(CPF_SPECIAL_FLAG<<(i+1))));
    gtk_widget_set_size_request (GTK_WIDGET (g->dtba[i]),16,16);
    gtk_table_attach (GTK_TABLE (bat), GTK_WIDGET (g->dtba[i]), (i%3),(i%3)+1,(i/3),(i/3)+1,0,0,0,0);
    g_signal_connect (G_OBJECT (g->dtba[i]), "toggled",G_CALLBACK (alignment_callback), self);
  }
  GtkWidget *hbox2 = gtk_hbox_new(FALSE,0);
  gtk_box_pack_start(GTK_BOX(hbox2),GTK_WIDGET(label4),TRUE,TRUE,0);
  gtk_box_pack_start(GTK_BOX(hbox2), GTK_WIDGET(bat), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(hbox2), TRUE, TRUE, 0);

  // x/y offset
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_VALUE,-1.0, 1.0,0.001, p->xoffset,3));
  g->scale4 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_VALUE,-1.0, 1.0,0.001, p->yoffset, 3));
  dtgtk_slider_set_label(g->scale3,_("x offset"));
  dtgtk_slider_set_label(g->scale4,_("y offset"));
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox), GTK_WIDGET(g->scale4), TRUE, TRUE, 0);


  // Let's add some tooltips and hook up some signals...
  g_object_set(G_OBJECT(g->scale1), "tooltip-text", _("the opacity of the watermark"), (char *)NULL);
  g_object_set(G_OBJECT(g->scale2), "tooltip-text", _("the scale of the watermark"), (char *)NULL);

  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (opacity_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (scale_callback), self);

  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (xoffset_callback), self);

  g_signal_connect (G_OBJECT (g->scale4), "value-changed",
                    G_CALLBACK (yoffset_callback), self);


  g_signal_connect (G_OBJECT (g->dtbutton1), "clicked",G_CALLBACK (refresh_callback), self);

  refresh_watermarks( self );


  g_signal_connect (G_OBJECT (g->combobox1), "changed",
                    G_CALLBACK (watermark_callback), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
