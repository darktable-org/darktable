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
#include "gui/gtk.h"
#include <gtk/gtk.h>
#include <inttypes.h>

#define GRAIN_LIGHTNESS_STRENGTH_SCALE 0.25
// (m_pi/2)/4 = half hue colorspan
#define GRAIN_HUE_STRENGTH_SCALE 0.392699082
#define GRAIN_SATURATION_STRENGTH_SCALE 0.25
#define GRAIN_RGB_STRENGTH_SCALE 0.25

#define CLIP(x) ((x<0)?0.0:(x>1.0)?1.0:x)
DT_MODULE(1)

typedef enum _dt_iop_grain_channel_t
{
  DT_GRAIN_CHANNEL_HUE=0,
  DT_GRAIN_CHANNEL_SATURATION,
  DT_GRAIN_CHANNEL_LIGHTNESS,
  DT_GRAIN_CHANNEL_RGB
}
_dt_iop_grain_channel_t;

typedef struct dt_iop_grain_params_t
{
  _dt_iop_grain_channel_t channel;
  float scale;
  float strength;
}
dt_iop_grain_params_t;

typedef struct dt_iop_grain_gui_data_t
{
  GtkVBox   *vbox1,  *vbox2;
  GtkLabel  *label1,*label2,*label3;	           // channel, scale, strength
  GtkComboBox *combo1;			                      // channel
  GtkDarktableSlider *scale1,*scale2;        // scale, strength
}
dt_iop_grain_gui_data_t;

typedef struct dt_iop_grain_data_t
{
  _dt_iop_grain_channel_t channel;
  float scale;
  float strength;
}
dt_iop_grain_data_t;


static int grad3[12][3] = {{1,1,0},{-1,1,0},{1,-1,0},{-1,-1,0},
                        {1,0,1},{-1,0,1},{1,0,-1},{-1,0,-1},
                        {0,1,1},{0,-1,1},{0,1,-1},{0,-1,-1}};

static int p[] = {151,160,137,91,90,15,
131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,8,99,37,240,21,10,23,
190, 6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,35,11,32,57,177,33,
88,237,149,56,87,174,20,125,136,171,168, 68,175,74,165,71,134,139,48,27,166,
77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,55,46,245,40,244,
102,143,54, 65,25,63,161, 1,216,80,73,209,76,132,187,208, 89,18,169,200,196,
135,130,116,188,159,86,164,100,109,198,173,186, 3,64,52,217,226,250,124,123,
5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,189,28,42,
223,183,170,213,119,248,152, 2,44,154,163, 70,221,153,101,155,167, 43,172,9,
129,22,39,253, 19,98,108,110,79,113,224,232,178,185, 112,104,218,246,97,228,
251,34,242,193,238,210,144,12,191,179,162,241, 81,51,145,235,249,14,239,107,
49,192,214, 31,181,199,106,157,184, 84,204,176,115,121,50,45,127, 4,150,254,
138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180};

static int perm[512];
void _simplex_noise_init() { for(int i=0; i<512; i++) perm[i] = p[i & 255]; }
double dot(int g[], double x, double y, double z) { return g[0]*x + g[1]*y + g[2]*z; }

#define FASTFLOOR(x) ( x>0 ? (int)(x) : (int)(x)-1 )

double _simplex_noise(double xin, double yin, double zin) {
 double n0, n1, n2, n3; // Noise contributions from the four corners
 // Skew the input space to determine which simplex cell we're in
 double F3 = 1.0/3.0;
 double s = (xin+yin+zin)*F3; // Very nice and simple skew factor for 3D
 int i = FASTFLOOR(xin+s);
 int j = FASTFLOOR(yin+s);
 int k = FASTFLOOR(zin+s);
 double G3 = 1.0/6.0; // Very nice and simple unskew factor, too
 double t = (i+j+k)*G3;
 double X0 = i-t; // Unskew the cell origin back to (x,y,z) space
 double Y0 = j-t;
 double Z0 = k-t;
 double x0 = xin-X0; // The x,y,z distances from the cell origin
 double y0 = yin-Y0;
 double z0 = zin-Z0;
 // For the 3D case, the simplex shape is a slightly irregular tetrahedron.
 // Determine which simplex we are in.
 int i1, j1, k1; // Offsets for second corner of simplex in (i,j,k) coords
 int i2, j2, k2; // Offsets for third corner of simplex in (i,j,k) coords
 if(x0>=y0) {
   if(y0>=z0)
     { i1=1; j1=0; k1=0; i2=1; j2=1; k2=0; } // X Y Z order
     else if(x0>=z0) { i1=1; j1=0; k1=0; i2=1; j2=0; k2=1; } // X Z Y order
     else { i1=0; j1=0; k1=1; i2=1; j2=0; k2=1; } // Z X Y order
   }
 else { // x0<y0
   if(y0<z0) { i1=0; j1=0; k1=1; i2=0; j2=1; k2=1; } // Z Y X order
   else if(x0<z0) { i1=0; j1=1; k1=0; i2=0; j2=1; k2=1; } // Y Z X order
   else { i1=0; j1=1; k1=0; i2=1; j2=1; k2=0; } // Y X Z order
 }
 //  A step of (1,0,0) in (i,j,k) means a step of (1-c,-c,-c) in (x,y,z),
 //  a step of (0,1,0) in (i,j,k) means a step of (-c,1-c,-c) in (x,y,z), and
 //  a step of (0,0,1) in (i,j,k) means a step of (-c,-c,1-c) in (x,y,z), where
 //  c = 1/6.
  double   x1 = x0 - i1 + G3; // Offsets for second corner in (x,y,z) coords
  double   y1 = y0 - j1 + G3;
  double   z1 = z0 - k1 + G3;
  double   x2 = x0 - i2 + 2.0*G3; // Offsets for third corner in (x,y,z) coords
  double   y2 = y0 - j2 + 2.0*G3;
  double   z2 = z0 - k2 + 2.0*G3;
  double   x3 = x0 - 1.0 + 3.0*G3; // Offsets for last corner in (x,y,z) coords
  double   y3 = y0 - 1.0 + 3.0*G3;
  double   z3 = z0 - 1.0 + 3.0*G3;
  // Work out the hashed gradient indices of the four simplex corners
  int ii = i & 255;
  int jj = j & 255;
  int kk = k & 255;
  int gi0 = perm[ii+perm[jj+perm[kk]]] % 12;
  int gi1 = perm[ii+i1+perm[jj+j1+perm[kk+k1]]] % 12;
  int gi2 = perm[ii+i2+perm[jj+j2+perm[kk+k2]]] % 12;
  int gi3 = perm[ii+1+perm[jj+1+perm[kk+1]]] % 12;
  // Calculate the contribution from the four corners
  double t0 = 0.6 - x0*x0 - y0*y0 - z0*z0;
  if(t0<0) n0 = 0.0;
  else {
    t0 *= t0;
    n0 = t0 * t0 * dot(grad3[gi0], x0, y0, z0);
  }
  double t1 = 0.6 - x1*x1 - y1*y1 - z1*z1;
  if(t1<0) n1 = 0.0;
  else {
    t1 *= t1;
    n1 = t1 * t1 * dot(grad3[gi1], x1, y1, z1);
  }
  double t2 = 0.6 - x2*x2 - y2*y2 - z2*z2;
  if(t2<0) n2 = 0.0;
  else {
    t2 *= t2;
    n2 = t2 * t2 * dot(grad3[gi2], x2, y2, z2);
  }
  double t3 = 0.6 - x3*x3 - y3*y3 - z3*z3;
  if(t3<0) n3 = 0.0;
  else {
    t3 *= t3;
    n3 = t3 * t3 * dot(grad3[gi3], x3, y3, z3);
  }
  // Add contributions from each corner to get the final noise value.
  // The result is scaled to stay just inside [-1,1]
  return 32.0*(n0 + n1 + n2 + n3);
}





#define PRIME_LEVELS 4
uint64_t _low_primes[PRIME_LEVELS] ={ 12503,14029,15649, 11369 };
//uint64_t _mid_primes[PRIME_LEVELS] ={ 784697,875783, 536461,639259};

double __value_noise(uint32_t level,uint32_t x,uint32_t y) 
{
  //uint32_t lvl=level%PRIME_LEVELS;
  uint32_t n = x + y * 57;
  n = (n<<13) ^ n;
  return ( 1.0 - (( (n * (n * n * 15731 + 789221) +1376312589) & 0x7fffffff) / 1073741824.0)); 
}

double __value_smooth_noise(uint32_t level,double x,double y) 
{
  double corners = ( __value_noise(level,x-1, y-1)+__value_noise(level,x+1, y-1)+__value_noise(level,x-1, y+1)+__value_noise(level,x+1, y+1) ) / 16;
  double sides   = ( __value_noise(level,x-1, y)  +__value_noise(level,x+1, y)  +__value_noise(level,x, y-1)  +__value_noise(level,x, y+1) ) /  8;
  double center  =  __value_noise(level,x, y) / 4;
  return corners + sides + center;
}

double __preline_cosine_interpolate(double a,double b,double x)
{
  double ft = x * 3.1415927;
	double f = (1 - cos(ft)) * .5;
	return  a*(1-f) + b*f;
}

double __value_interpolate(uint32_t level,double x,double y) 
{
  double fx = x - (uint32_t)x;
  double fy = y - (uint32_t)y;

  double v1 = __value_smooth_noise(level,(uint32_t)x,     (uint32_t)y);
  double v2 = __value_smooth_noise(level,(uint32_t)x + 1, (uint32_t)y);
  double v3 = __value_smooth_noise(level,(uint32_t)x,     (uint32_t)y + 1);
  double v4 = __value_smooth_noise(level,(uint32_t)x + 1, (uint32_t)y + 1);

  double i1 = __preline_cosine_interpolate(v1 , v2 , fx);
  double i2 = __preline_cosine_interpolate(v3 , v4 , fx);

  return __preline_cosine_interpolate(i1 , i2 , fy);
}
double _perlin_2d_noise(double x,double y,uint32_t octaves,double persistance,double z) 
{
  double f=1,a=1,total=0;
  
  for(int o=0;o<octaves;o++) {
    total+= (__value_interpolate(o,x*f/z,y*f/z)*a);
    f=2*o;
    a=persistance*o;
  }
  return total;
}

double _simplex_2d_noise(double x,double y,uint32_t octaves,double persistance,double z) 
{
  double f=1,a=1,total=0;
  
  for(int o=0;o<octaves;o++) {
    total+= (_simplex_noise(x*f/z,y*f/z,o)*a);
    f=2*o;
    a=persistance*o;
  }
  return total;
}

void rgb2hsl(float r,float g,float b,float *h,float *s,float *l) 
{
  float pmax=fmax(r,fmax(g,b));
  float pmin=fmin(r,fmin(g,b));
  float delta=(pmax-pmin);
  
  *h=*s=*l=0;
  *l=(pmin+pmax)/2.0;
 
  if(pmax!=pmin) 
  {
    *s=*l<0.5?delta/(pmax+pmin):delta/(2.0-pmax-pmin);
  
    if(pmax==r) *h=(g-b)/delta;
    if(pmax==g) *h=2.0+(b-r)/delta;
    if(pmax==b) *h=4.0+(r-g)/delta;
    *h/=6.0;
    if(*h<0.0) *h+=1.0;
    else if(*h>1.0) *h-=1.0;
  }
}
void hue2rgb(float m1,float m2,float hue,float *channel)
{
  if(hue<0.0) hue+=1.0;
  else if(hue>1.0) hue-=1.0;
  
  if( (6.0*hue) < 1.0) *channel=(m1+(m2-m1)*hue*6.0);
  else if((2.0*hue) < 1.0) *channel=m2;
  else if((3.0*hue) < 2.0) *channel=(m1+(m2-m1)*((2.0/3.0)-hue)*6.0);
  else *channel=m1;
}

void hsl2rgb(float *r,float *g,float *b,float h,float s,float l)
{
  float m1,m2;
  *r=*g=*b=l;
  if( s==0) return;
  m2=l<0.5?l*(1.0+s):l+s-l*s;
  m1=(2.0*l-m2);
  hue2rgb(m1,m2,h +(1.0/3.0), r);
  hue2rgb(m1,m2,h, g);
  hue2rgb(m1,m2,h - (1.0/3.0), b);
}


const char *name()
{
  return _("grain");
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_grain_data_t *data = (dt_iop_grain_data_t *)piece->data;
  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  
  // Apply grain to image
  in  = (float *)ivoid;
  out = (float *)ovoid;
  float h,s,l;
  double strength=(data->strength/100.0);
  for(int j=0;j<roi_out->height;j++) for(int i=0;i<roi_out->width;i++)
  {
    double xscale=piece->buf_in.width>piece->buf_in.height?(piece->buf_in.width/piece->buf_in.height):1.0;
    double yscale=piece->buf_in.width<piece->buf_in.height?(piece->buf_in.height/piece->buf_in.width):1.0;
	  
    double x=((roi_out->x+i)/(double)(roi_out->x+roi_out->width)) *512;	// * (32+(256*(data->scale/100.0)));
    double y=((roi_out->y+j)/(double)(roi_out->y+roi_out->height)) *512;	// * (32+(256*(data->scale/100.0)));
    x*=xscale;
    y*=yscale;
	  
    double octaves=2;
    double zoom=8*(data->scale/100.0);
  //  double noise=_perlin_2d_noise(x, y, octaves,0.25, zoom)*1.5;
    double noise=_simplex_2d_noise(x, y, octaves,0.5, zoom);
    if(data->channel==DT_GRAIN_CHANNEL_LIGHTNESS || data->channel==DT_GRAIN_CHANNEL_SATURATION) 
    {
      rgb2hsl(in[0],in[1],in[2],&h,&s,&l);
    
      h+=(data->channel==DT_GRAIN_CHANNEL_HUE)? noise*(strength*GRAIN_HUE_STRENGTH_SCALE) : 0;
      s+=(data->channel==DT_GRAIN_CHANNEL_SATURATION)? noise*(strength*GRAIN_SATURATION_STRENGTH_SCALE) : 0;
      l+=(data->channel==DT_GRAIN_CHANNEL_LIGHTNESS)? noise*(strength*GRAIN_LIGHTNESS_STRENGTH_SCALE) : 0;
      s=CLIP(s); 
      l=CLIP(l);
      if(h<0.0) h+=1.0;
      if(h>1.0) h-=1.0;
      hsl2rgb(&out[0],&out[1],&out[2],h,s,l);
    } 
    else if( data->channel==DT_GRAIN_CHANNEL_RGB )
    {
      out[0]=CLIP(in[0]+(noise*(strength*GRAIN_RGB_STRENGTH_SCALE)));
      out[1]=CLIP(in[1]+(noise*(strength*GRAIN_RGB_STRENGTH_SCALE)));
      out[2]=CLIP(in[2]+(noise*(strength*GRAIN_RGB_STRENGTH_SCALE)));
    } 
    else
    { // No noisemethod lets jsut copy source to dest
      out[0]=in[0];
      out[1]=in[1];
      out[2]=in[2];
    }  
    out += 3; in += 3;
  }
  
}

static void
channel_changed (GtkComboBox *combo, dt_iop_module_t *self)
{
  dt_iop_grain_gui_data_t *g = (dt_iop_grain_gui_data_t *)self->gui_data;
  if(self->dt->gui->reset) return;
  dt_iop_grain_params_t *p = (dt_iop_grain_params_t *)self->params;
  p->channel = gtk_combo_box_get_active(g->combo1);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
scale_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_grain_params_t *p = (dt_iop_grain_params_t *)self->params;
  p->scale= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}

static void
strength_callback (GtkDarktableSlider *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_grain_params_t *p = (dt_iop_grain_params_t *)self->params;
  p->strength= dtgtk_slider_get_value(slider);
  dt_dev_add_history_item(darktable.develop, self);
}


void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_grain_params_t *p = (dt_iop_grain_params_t *)p1;
#ifdef HAVE_GEGL
  fprintf(stderr, "[grain] TODO: implement gegl version!\n");
  // pull in new params to gegl
#else
  dt_iop_grain_data_t *d = (dt_iop_grain_data_t *)piece->data;
  d->channel = p->channel;
  d->scale = p->scale;
  d->strength = p->strength;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
  // create part of the gegl pipeline
  piece->data = NULL;
#else
  piece->data = malloc(sizeof(dt_iop_grain_data_t));
  memset(piece->data,0,sizeof(dt_iop_grain_data_t));
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
  dt_iop_grain_gui_data_t *g = (dt_iop_grain_gui_data_t *)self->gui_data;
  dt_iop_grain_params_t *p = (dt_iop_grain_params_t *)module->params;
  gtk_combo_box_set_active(g->combo1, p->channel);
  dtgtk_slider_set_value(g->scale1, p->scale);
  dtgtk_slider_set_value(g->scale2, p->strength);
}

void init(dt_iop_module_t *module)
{
  _simplex_noise_init();
  module->params = malloc(sizeof(dt_iop_grain_params_t));
  module->default_params = malloc(sizeof(dt_iop_grain_params_t));
  module->default_enabled = 0;
  module->priority = 965;
  module->params_size = sizeof(dt_iop_grain_params_t);
  module->gui_data = NULL;
  dt_iop_grain_params_t tmp = (dt_iop_grain_params_t){DT_GRAIN_CHANNEL_LIGHTNESS,0.0,50.0};
  memcpy(module->params, &tmp, sizeof(dt_iop_grain_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_grain_params_t));
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
  self->gui_data = malloc(sizeof(dt_iop_grain_gui_data_t));
  dt_iop_grain_gui_data_t *g = (dt_iop_grain_gui_data_t *)self->gui_data;
  dt_iop_grain_params_t *p = (dt_iop_grain_params_t *)self->params;

  self->widget = GTK_WIDGET(gtk_hbox_new(FALSE, 0));
  g->vbox1 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  g->vbox2 = GTK_VBOX(gtk_vbox_new(FALSE, 0));
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox1), FALSE, FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(g->vbox2), TRUE, TRUE, 5);
  g->label1 = GTK_LABEL(gtk_label_new(_("channel")));
  g->label2 = GTK_LABEL(gtk_label_new(_("scale")));
  g->label3 = GTK_LABEL(gtk_label_new(_("strength")));
  gtk_misc_set_alignment(GTK_MISC(g->label1), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label2), 0.0, 0.5);
  gtk_misc_set_alignment(GTK_MISC(g->label3), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label2), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox1), GTK_WIDGET(g->label3), TRUE, TRUE, 0);
  
  g->combo1=GTK_COMBO_BOX(gtk_combo_box_new_text());
  gtk_combo_box_append_text(g->combo1,_("hue"));
  gtk_combo_box_append_text(g->combo1,_("saturation"));
  gtk_combo_box_append_text(g->combo1,_("lightness"));
  //gtk_combo_box_append_text(g->combo1,_("rgb"));
  gtk_combo_box_set_active(g->combo1,p->channel);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->combo1), TRUE, TRUE, 0);
  
  g->scale1 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->scale, 2));
  dtgtk_slider_set_format_type(g->scale1,DARKTABLE_SLIDER_FORMAT_PERCENT);
  g->scale2 = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR,0.0, 100.0, 0.1, p->strength, 2));
  dtgtk_slider_set_format_type(g->scale2,DARKTABLE_SLIDER_FORMAT_PERCENT);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale1), TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(g->vbox2), GTK_WIDGET(g->scale2), TRUE, TRUE, 0);
  gtk_object_set(GTK_OBJECT(g->scale1), "tooltip-text", _("the scale of the noise"), NULL);
  gtk_object_set(GTK_OBJECT(g->scale2), "tooltip-text", _("the the strength of applied grain"), NULL);
  
 g_signal_connect (G_OBJECT (g->combo1), "changed",
            G_CALLBACK (channel_changed), self);
 g_signal_connect (G_OBJECT (g->scale1), "value-changed",
                    G_CALLBACK (scale_callback), self);
  g_signal_connect (G_OBJECT (g->scale2), "value-changed",
        G_CALLBACK (strength_callback), self);
 
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  free(self->gui_data);
  self->gui_data = NULL;
}

