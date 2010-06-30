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
#ifdef HAVE_GEGL
  #include <gegl.h>
#endif
#include "develop/develop.h"
#include "develop/imageop.h"
#include "control/control.h"
#include "dtgtk/slider.h"
#include "dtgtk/button.h"
#include "dtgtk/togglebutton.h"
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>
#include <librsvg/rsvg.h>

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
  GtkVBox   *vbox1, *vbox2;
  GtkLabel  *label1, *label2,*label3,*label4,*label5,*label6;	 	 // watermark, opacity, scale, alignment, xoffset,yoffset
  GtkComboBox *combobox1;		                                             // watermark
  GtkDarktableButton *dtbutton1;	                                         // refresh watermarks...
  GtkDarktableToggleButton *dtba[9];	                                   // Alignment buttons
  GtkDarktableSlider *scale1,*scale2,*scale3,*scale4;      	     // opacity, scale, xoffs, yoffs
}
dt_iop_watermark_gui_data_t;

typedef struct dt_iop_watermark_global_data_t {
  float scale;
  int roi_width;
  int roi_height;
  gchar *svgdata;
} dt_iop_watermark_global_data_t;


const char *name()
{
  return _("watermark");
}


guint _string_occurence(const gchar *haystack,const gchar *needle) 
{
  guint o=0;
  const gchar *p=haystack;
  if( (p=g_strstr_len(p,strlen(p),needle)) != NULL) 
  {
    do
    {
      o++;
    } while((p=g_strstr_len((p+1),strlen(p+1),needle)) != NULL);
  }
  return o;
}

gchar *_string_substitute(gchar *string,const gchar *search,const gchar *replace)
{
  gint occurences = _string_occurence(string,search);
  if( occurences )
  {
    gint sl=-(strlen(search)-strlen(replace));
    gchar *pend=string+strlen(string);
    gchar *nstring=g_malloc(strlen(string)+(sl*occurences)+1);
    gchar *np=nstring;
    gchar *s=string,*p=string;
    if( (s=g_strstr_len(s,strlen(s),search)) != NULL) 
    {
      do
      {
        memcpy(np,p,s-p);
        np+=(s-p);
        memcpy(np,replace,strlen(replace));
        np+=strlen(replace);
        p=s+strlen(search);
      } while((s=g_strstr_len((s+1),strlen(s+1),search)) != NULL);
    }
    memcpy(np,p,pend-p);
    np[pend-p]='\0';
    string=nstring;
  } 
  return string;
}

static void _rsvg_size_func(gint *width,gint *height,gpointer user_data) {
    dt_iop_watermark_global_data_t *gd=(dt_iop_watermark_global_data_t *)user_data;
    float ratio=(float)*width / (float)*height;
    fprintf(stderr,"rsvg: size %dx%d scaling %f, ratio %f\n",*width,*height,gd->scale,ratio);
    *width = gd->roi_width * (gd->scale/100.0); 
    *height = (float)*width/ratio;
    fprintf(stderr,"rsvg: new size %dx%d\n",*width,*height);
}

void _load_svg( dt_iop_module_t *self ) {
  gsize length;
  dt_iop_watermark_global_data_t *gd = (dt_iop_watermark_global_data_t *)self->data;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  
  if( gd->svgdata == NULL ) {      
    gchar datadir[1024], filename[2048];
    dt_get_datadir(datadir, 1024);
    snprintf(filename, 2048, "%s/watermarks/%s", datadir,  p->filename );
    gchar *svgdata=NULL;
    if( g_file_get_contents( filename, &svgdata, &length, NULL) ) {
      // File is loaded lets substitute strings if found...
      
      // Darktable internal 
      gd->svgdata = _string_substitute(svgdata,"$(DARKTABLE.NAME)",PACKAGE_NAME);
      if( gd->svgdata != svgdata ) { g_free(svgdata); svgdata = gd->svgdata; }
      gd->svgdata = _string_substitute(svgdata,"$(DARKTABLE.VERSION)",PACKAGE_VERSION);
      if( gd->svgdata != svgdata ) { g_free(svgdata); svgdata = gd->svgdata; }
    
      // Current image
      gchar buffer[1024];
      dt_image_print_exif(darktable.develop->image,buffer,1024);
      gd->svgdata = _string_substitute(svgdata,"$(IMAGE.EXIF)",buffer);
      if( gd->svgdata != svgdata ) { g_free(svgdata); svgdata = gd->svgdata; }

      // Image exid
      gd->svgdata = _string_substitute(svgdata,"$(EXIF.DATE)",darktable.develop->image->exif_datetime_taken);
      if( gd->svgdata != svgdata ) { g_free(svgdata); svgdata = gd->svgdata; }
      gd->svgdata = _string_substitute(svgdata,"$(EXIF.MAKER)",darktable.develop->image->exif_maker);
      if( gd->svgdata != svgdata ) { g_free(svgdata); svgdata = gd->svgdata; }
      gd->svgdata = _string_substitute(svgdata,"$(EXIF.MODEL)",darktable.develop->image->exif_model);
      if( gd->svgdata != svgdata ) { g_free(svgdata); svgdata = gd->svgdata; }
      gd->svgdata = _string_substitute(svgdata,"$(EXIF.LENS)",darktable.develop->image->exif_lens);
      if( gd->svgdata != svgdata ) { g_free(svgdata); svgdata = gd->svgdata; }

      gd->svgdata = _string_substitute(svgdata,"$(IMAGE.FILENAME)",PACKAGE_VERSION);
      if( gd->svgdata != svgdata ) { g_free(svgdata); svgdata = gd->svgdata; }
    }
  }
    
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_watermark_data_t *data = (dt_iop_watermark_data_t *)piece->data;
  dt_iop_watermark_global_data_t *gd = (dt_iop_watermark_global_data_t*)self->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;

  // Load svg if not loaded
  _load_svg( self );
  
  gd->scale = data->scale;
  gd->roi_width=roi_out->width;
  gd->roi_height=roi_out->height;
  
  // Open and read svg xml file
  RsvgHandle *svg = rsvg_handle_new();
  
  // Parse and replace keywords in svg xml
  
  // Create rsvg context set size callback and dpi
  rsvg_handle_set_size_callback(svg, _rsvg_size_func, gd, NULL);
  
  // Write data to handle
  GError *err=NULL;
  if( gd->svgdata && rsvg_handle_write(svg,(guchar *)gd->svgdata,strlen(gd->svgdata),&err) == FALSE )
    fprintf(stderr,"rsvg: error %s\n",err->message);
  if( rsvg_handle_close(svg,&err) == FALSE )
    fprintf(stderr,"rsvg: error %s\n",err->message);
  
  // Get GdkPixbuf
  GdkPixbuf *watermark = rsvg_handle_get_pixbuf( svg );
  int sw = gdk_pixbuf_get_width(watermark);
  int sh = gdk_pixbuf_get_height(watermark);
  int dw = roi_out->width;
  int dh = roi_out->height;
  
  int dx=0,dy=0;
  
  // Let check and initialize y alignment
  if( data->alignment >=0 && data->alignment <3) // Align to verttop
    dy=0;
  else if( data->alignment >=3 && data->alignment <6) // Align to vertcenter
    dy=(dh/2.0)-(sh/2.0);
  else if( data->alignment >=6 && data->alignment <9) // Align to vertbottom
    dy=dh-sh;
  
  // Let check and initialize x alignment
  if( data->alignment == 0 ||  data->alignment == 3 || data->alignment==6 )
    dx=0;
  else if( data->alignment == 1 ||  data->alignment == 4 || data->alignment==7 )
    dx=(dw/2.0)-(sw/2.0);
  else if( data->alignment == 2 ||  data->alignment == 5 || data->alignment==8 )
    dx=dw-sw;

  // Apply x and y offset to alignment for finetuning position
  dx+=(data->xoffset*(dw*0.6666));
  dy+=(data->yoffset*(dh*0.6666));
  
  // Lets run thru pixels
  float opacity=data->opacity/100.0;
  guint8 *sd = gdk_pixbuf_get_pixels(watermark);
  for(int j=0;j<roi_out->height;j++) for(int i=0;i<roi_out->width;i++)
  {
    if( ( j >= dy && j < dy+sh ) && ( i>=dx && i<dx+sw ) ) { // Blit the overlay
      int index=(sw*(j-dy)+(i-dx))*4;
      float alpha = (sd[index+3]/255.0)*opacity;
      out[0] = ((1.0-alpha)*in[0]) + (alpha*(sd[index]/255.0));
      out[1] = ((1.0-alpha)*in[1]) + (alpha*(sd[index+1]/255.0));
      out[2] = ((1.0-alpha)*in[2]) + (alpha*(sd[index+2]/255.0));
    } else { // Copy src...
      out[0]=in[0];
      out[1]=in[1];
      out[2]=in[2];
    }
    out+=3;in+=3;
  }
  
  if(watermark)
    g_object_unref(watermark);
  
  rsvg_handle_free(svg);
}



static void 
watermark_callback(GtkWidget *tb, gpointer user_data) 
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_watermark_gui_data_t *g = (dt_iop_watermark_gui_data_t *)self->gui_data;
  dt_iop_watermark_global_data_t *gd = (dt_iop_watermark_global_data_t *)self->data;
  
  if(self->dt->gui->reset) return;
  if( gd->svgdata ) { g_free(gd->svgdata); gd->svgdata=NULL; }
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  snprintf(p->filename,32,"%s",gtk_combo_box_get_active_text(g->combobox1));
  dt_dev_add_history_item(darktable.develop, self);
}

void refresh_watermarks( dt_iop_module_t *self ) {
  dt_iop_watermark_gui_data_t *g = (dt_iop_watermark_gui_data_t *)self->gui_data;

  // Clear combobox...
  GtkTreeModel *model=gtk_combo_box_get_model(g->combobox1);
  gtk_list_store_clear (GTK_LIST_STORE(model));
  
  // check watermarkdir and update combo with entries...
  int count=0;
  const gchar *d_name = NULL;
  gchar datadir[1024], dirname[1024], filename[2048];
  dt_get_datadir(datadir, 1024);
  snprintf(dirname, 1024, "%s/watermarks", datadir);
  GDir *dir = g_dir_open(dirname, 0, NULL);
  if(dir)
  {
    while((d_name = g_dir_read_name(dir)))
    {
      snprintf(filename, 1024, "%s/%s", dirname, d_name);
      gtk_combo_box_append_text( g->combobox1, d_name );
      count++;
    }
  }
  
  if( count != 0)
    gtk_combo_box_set_active(g->combobox1,0);
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
  
  for(int i=0; i<9; i++) {
    if( GTK_WIDGET(g->dtba[i]) == tb ) index=i;
    else gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g->dtba[i]),FALSE);
  }
  p->alignment= index;
  dt_dev_add_history_item(darktable.develop, self);
}

static void
opacity_callback(GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  p->opacity= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
xoffset_callback(GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  p->xoffset= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
yoffset_callback(GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  p->yoffset= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
scale_callback(GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  p->scale= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
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
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_watermark_data_t));
  memset(piece->data,0,sizeof(dt_iop_watermark_data_t));
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
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_watermark_params_t));
  module->params_size = sizeof(dt_iop_watermark_params_t);
  module->default_params = malloc(sizeof(dt_iop_watermark_params_t));
  module->default_enabled = 0;
  module->priority = 999;
  module->params_size = sizeof(dt_iop_watermark_params_t);
  module->gui_data = NULL;
  module->data = malloc( sizeof(dt_iop_watermark_global_data_t));
  memset(module->data,0,sizeof(dt_iop_watermark_global_data_t));
  dt_iop_watermark_params_t tmp = (dt_iop_watermark_params_t){100.0,100.0,0.0,0.0,5}; // opacity,scale,xoffs,yoffs,aligment
  memcpy(module->params, &tmp, sizeof(dt_iop_watermark_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_watermark_params_t));
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->data);
  module->data= NULL;
  free(module->params);
  module->params = NULL;
}

void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_watermark_gui_data_t));
  dt_iop_watermark_gui_data_t *g = (dt_iop_watermark_gui_data_t *)self->gui_data;
  dt_iop_watermark_params_t *p = (dt_iop_watermark_params_t *)self->params;
  
  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new(_("marker")));
  g->label2 = GTK_LABEL(gtk_label_new(_("opacity")));
  g->label3 = GTK_LABEL(gtk_label_new(_("scale")));
  g->label4 = GTK_LABEL(gtk_label_new(_("alignment")));
  g->label5 = GTK_LABEL(gtk_label_new(_("x offset")));
  g->label6 = GTK_LABEL(gtk_label_new(_("y offset")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label4), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label5), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label6), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label4), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label5), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label6), TRUE, TRUE, 0);
  
  GtkWidget *hbox= GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->combobox1 = GTK_COMBO_BOX(gtk_combo_box_new_text());
  g->dtbutton1  = DTGTK_BUTTON(dtgtk_button_new(dtgtk_cairo_paint_refresh, 0));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->combobox1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->dtbutton1), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(hbox), TRUE, TRUE, 0);
 
  
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.5, p->opacity, 0.5));
  dtgtk_slider_set_format_type(g->scale1,DARKTABLE_SLIDER_FORMAT_PERCENT);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
 
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,1.0, 100.0, 1.0, p->scale, 0.5));
  dtgtk_slider_set_format_type(g->scale2,DARKTABLE_SLIDER_FORMAT_PERCENT);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
 
  GtkBox *br = GTK_BOX(gtk_hbox_new(FALSE,1));
  GtkBox *bb = GTK_BOX(gtk_vbox_new(FALSE,1));
  g->dtba[0] = DTGTK_TOGGLEBUTTON( dtgtk_togglebutton_new(dtgtk_cairo_paint_color,CPF_IGNORE_FG_STATE) );
  g->dtba[1] = DTGTK_TOGGLEBUTTON( dtgtk_togglebutton_new(dtgtk_cairo_paint_color,CPF_IGNORE_FG_STATE) );
  g->dtba[2] = DTGTK_TOGGLEBUTTON( dtgtk_togglebutton_new(dtgtk_cairo_paint_color,CPF_IGNORE_FG_STATE) );
  gtk_box_pack_start(GTK_BOX(br), GTK_WIDGET(g->dtba[0]), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(br), GTK_WIDGET(g->dtba[1]), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(br), GTK_WIDGET(g->dtba[2]), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bb), GTK_WIDGET(br), FALSE, FALSE, 0);
 
  br = GTK_BOX( gtk_hbox_new(FALSE,1) );
  g->dtba[3] = DTGTK_TOGGLEBUTTON( dtgtk_togglebutton_new(dtgtk_cairo_paint_color,CPF_IGNORE_FG_STATE) );
  g->dtba[4] = DTGTK_TOGGLEBUTTON( dtgtk_togglebutton_new(dtgtk_cairo_paint_color,CPF_IGNORE_FG_STATE) );
  g->dtba[5] = DTGTK_TOGGLEBUTTON( dtgtk_togglebutton_new(dtgtk_cairo_paint_color,CPF_IGNORE_FG_STATE) );
  gtk_box_pack_start(GTK_BOX(br), GTK_WIDGET(g->dtba[3]), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(br), GTK_WIDGET(g->dtba[4]), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(br), GTK_WIDGET(g->dtba[5]), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bb), GTK_WIDGET(br), FALSE, FALSE, 0);
  
  br = GTK_BOX( gtk_hbox_new(FALSE,1) );
  g->dtba[6] = DTGTK_TOGGLEBUTTON( dtgtk_togglebutton_new(dtgtk_cairo_paint_color,CPF_IGNORE_FG_STATE) );
  g->dtba[7] = DTGTK_TOGGLEBUTTON( dtgtk_togglebutton_new(dtgtk_cairo_paint_color,CPF_IGNORE_FG_STATE) );
  g->dtba[8] = DTGTK_TOGGLEBUTTON( dtgtk_togglebutton_new(dtgtk_cairo_paint_color,CPF_IGNORE_FG_STATE) );
  gtk_box_pack_start(GTK_BOX(br), GTK_WIDGET(g->dtba[6]), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(br), GTK_WIDGET(g->dtba[7]), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(br), GTK_WIDGET(g->dtba[8]), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(bb), GTK_WIDGET(br), FALSE, FALSE, 0);
 
 gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(bb), TRUE, TRUE, 0);
 
  g->scale3 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_VALUE,-1.0, 1.0,0.001, p->xoffset,3));
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale3), TRUE, TRUE, 0);

  g->scale4 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_VALUE,-1.0, 1.0,0.001, p->yoffset, 3));
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale4), TRUE, TRUE, 0);
 
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("the opacity of the watermark"), NULL);
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("the scale of the watermark"), NULL);

g_signal_connect (G_OBJECT (g->combobox1), "changed",
                    G_CALLBACK (watermark_callback), self);     
  
  g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (opacity_callback), self);     
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
                    G_CALLBACK (scale_callback), self);      
                    
  g_signal_connect (G_OBJECT (g->scale3), "value-changed",
                    G_CALLBACK (xoffset_callback), self); 
                    
  g_signal_connect (G_OBJECT (g->scale4), "value-changed",
                    G_CALLBACK (yoffset_callback), self);          
                    
  for(int i=0;i<9;i++)
    g_signal_connect (G_OBJECT (g->dtba[i]), "toggled",G_CALLBACK (alignment_callback), self);          
  
  g_signal_connect (G_OBJECT (g->dtbutton1), "clicked",G_CALLBACK (refresh_callback), self);        
  
  refresh_watermarks( self );
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

