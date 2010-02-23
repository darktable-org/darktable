/** this is the view for the lighttable module.  */
#include "views/view.h"
#include "libs/lib.h"
#include "control/jobs.h"
#include "control/settings.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "common/darktable.h"
#include "gui/gtk.h"
#include "gui/draw.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <assert.h>

DT_MODULE(1)

#define DT_LIBRARY_MAX_ZOOM 13

typedef enum dt_library_image_over_t
{
  DT_LIB_DESERT = 0,
  DT_LIB_STAR_1 = 1,
  DT_LIB_STAR_2 = 2,
  DT_LIB_STAR_3 = 3,
  DT_LIB_STAR_4 = 4
}
dt_library_image_over_t;


/**
 * this organises the whole library:
 * previously imported film rolls..
 */
typedef struct dt_library_t
{
  // tmp mouse vars:
  float select_offset_x, select_offset_y;
  int32_t last_selected_id;
  int button;
  uint32_t modifiers;
  uint32_t center, pan;
  int32_t track, offset;
  float zoom_x, zoom_y;
  dt_library_image_over_t image_over;
}
dt_library_t;

const char *name(dt_view_t *self)
{
  return _("lighttable");
}

void init(dt_view_t *self)
{
  self->data = malloc(sizeof(dt_library_t));
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->select_offset_x = lib->select_offset_y = 0.5f;
  lib->last_selected_id = -1;
  lib->button = 0;
  lib->modifiers = 0;
  lib->center = lib->pan = lib->track = 0;
  lib->zoom_x = 0.0f;
  lib->zoom_y = 0.0f;
}


void cleanup(dt_view_t *self)
{
  free(self->data);
}


void dt_library_star(cairo_t *cr, float x, float y, float r1, float r2)
{
  const float d = 2.0*M_PI*0.1f;
  const float dx[10] = {sinf(0.0), sinf(d), sinf(2*d), sinf(3*d), sinf(4*d), sinf(5*d), sinf(6*d), sinf(7*d), sinf(8*d), sinf(9*d)};
  const float dy[10] = {cosf(0.0), cosf(d), cosf(2*d), cosf(3*d), cosf(4*d), cosf(5*d), cosf(6*d), cosf(7*d), cosf(8*d), cosf(9*d)};
  cairo_move_to(cr, x+r1*dx[0], y-r1*dy[0]);
  for(int k=1;k<10;k++)
    if(k&1) cairo_line_to(cr, x+r2*dx[k], y-r2*dy[k]);
    else    cairo_line_to(cr, x+r1*dx[k], y-r1*dy[k]);
  cairo_close_path(cr);
}


void dt_image_expose(dt_image_t *img, dt_library_t *lib, int32_t index, cairo_t *cr, int32_t width, int32_t height, int32_t zoom, int32_t px, int32_t py)
{
  float bgcol = 0.4, fontcol = 0.5, bordercol = 0.1, outlinecol = 0.2;
  int selected = 0, altered = 0, imgsel;
  DT_CTL_GET_GLOBAL(imgsel, lib_image_mouse_over_id);
  // if(img->flags & DT_IMAGE_SELECTED) selected = 1;
  sqlite3_stmt *stmt;
  int rc;
  rc = sqlite3_prepare_v2(darktable.db, "select * from selected_images where imgid = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, img->id);
  if(sqlite3_step(stmt) == SQLITE_ROW) selected = 1;
  sqlite3_finalize(stmt);
  rc = sqlite3_prepare_v2(darktable.db, "select num from history where imgid = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, img->id);
  if(sqlite3_step(stmt) == SQLITE_ROW) altered = 1;
  sqlite3_finalize(stmt);
  if(selected == 1)
  {
    outlinecol = 0.4;
    bgcol = 0.6; fontcol = 0.5;
  }
  if(imgsel == img->id) { bgcol = 0.8; fontcol = 0.7; outlinecol = 0.6; } // mouse over
  float imgwd = 0.8f;
  if(zoom == 1)
  {
    imgwd = .97f;
    // cairo_set_antialias(cr, CAIRO_ANTIALIAS_NONE);
  }
  else
  {
    double x0 = 0.007*width, y0 = 0.007*height, rect_width = 0.986*width, rect_height = 0.986*height, radius = 0.04*width;
    // double x0 = 0.*width, y0 = 0.*height, rect_width = 1.*width, rect_height = 1.*height, radius = 0.08*width;
    double x1, y1, off, off1;

    x1=x0+rect_width;
    y1=y0+rect_height;
    off=radius*0.666;
    off1 = radius-off;
    cairo_move_to  (cr, x0, y0 + radius);
    cairo_curve_to (cr, x0, y0+off1, x0+off1 , y0, x0 + radius, y0);
    cairo_line_to (cr, x1 - radius, y0);
    cairo_curve_to (cr, x1-off1, y0, x1, y0+off1, x1, y0 + radius);
    cairo_line_to (cr, x1 , y1 - radius);
    cairo_curve_to (cr, x1, y1-off1, x1-off1, y1, x1 - radius, y1);
    cairo_line_to (cr, x0 + radius, y1);
    cairo_curve_to (cr, x0+off1, y1, x0, y1-off1, x0, y1- radius);
    cairo_close_path (cr);
    cairo_set_source_rgb(cr, bgcol, bgcol, bgcol);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 0.005*width);
    cairo_set_source_rgb(cr, outlinecol, outlinecol, outlinecol);
    cairo_stroke(cr);

#if defined(__MACH__) || defined(__APPLE__) // dreggn
#else
    char num[10];
    cairo_set_source_rgb(cr, fontcol, fontcol, fontcol);
    cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size (cr, .25*width);

    cairo_move_to (cr, .0*width, .24*height);
    snprintf(num, 10, "%d", index);
    cairo_show_text (cr, num);
#endif
  }

#if 1
  int32_t iwd = width*imgwd, iht = height*imgwd, stride;
  float scale = 1.0;
  dt_image_buffer_t mip;
  mip = dt_image_get_matching_mip_size(img, imgwd*width, imgwd*height, &iwd, &iht);
  mip = dt_image_get(img, mip, 'r');
  dt_image_get_mip_size(img, mip, &iwd, &iht);
  float fwd, fht;
  dt_image_get_exact_mip_size(img, mip, &fwd, &fht);
  cairo_surface_t *surface = NULL;
  if(mip != DT_IMAGE_NONE)
  {
    stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, iwd);
    surface = cairo_image_surface_create_for_data (img->mip[mip], CAIRO_FORMAT_RGB24, iwd, iht, stride); 
    scale = fminf(width*imgwd/fwd, height*imgwd/fht);
  }

  // draw centered and fitted:
  cairo_save(cr);
  cairo_translate(cr, width/2.0, height/2.0f);
  cairo_scale(cr, scale, scale);
  cairo_translate(cr, -.5f*fwd, -.5f*fht);

  if(mip != DT_IMAGE_NONE)
  {
    cairo_set_source_surface (cr, surface, 0, 0);
    cairo_rectangle(cr, 0, 0, fwd, fht);
    cairo_fill(cr);
    cairo_surface_destroy (surface);
    dt_image_release(img, mip, 'r');

    if(zoom == 1) cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_rectangle(cr, 0, 0, fwd, fht);
  }

  // border around image
  const float border = zoom == 1 ? 16/scale : 2/scale;
  cairo_set_source_rgb(cr, bordercol, bordercol, bordercol);
  if(mip != DT_IMAGE_NONE && selected)
  {
    cairo_set_line_width(cr, 1./scale);
    if(zoom == 1)
    { // draw shadow around border
      cairo_set_source_rgb(cr, 0.2, 0.2, 0.2);
      cairo_stroke(cr);
      // cairo_new_path(cr);
      cairo_set_fill_rule (cr, CAIRO_FILL_RULE_EVEN_ODD);
      float alpha = 1.0f;
      for(int k=0;k<16;k++)
      {
        cairo_rectangle(cr, 0, 0, fwd, fht);
        cairo_new_sub_path(cr);
        cairo_rectangle(cr, -k/scale, -k/scale, fwd+2.*k/scale, fht+2.*k/scale);
        cairo_set_source_rgba(cr, 0, 0, 0, alpha);
        alpha *= 0.6f;
        cairo_fill(cr);
      }
    }
    else
    {
      cairo_set_fill_rule (cr, CAIRO_FILL_RULE_EVEN_ODD);
      cairo_new_sub_path(cr);
      cairo_rectangle(cr, -border, -border, fwd+2.*border, fht+2.*border);
      cairo_stroke_preserve(cr);
      cairo_set_source_rgb(cr, 1.0-bordercol, 1.0-bordercol, 1.0-bordercol);
      cairo_fill(cr);
    }
  }
  else if(mip != DT_IMAGE_NONE)
  {
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
  }
  cairo_restore(cr);

  const float fscale = fminf(width, height);
  if(imgsel == img->id)
  { // draw mouseover hover effects, set event hook for mouse button down!
    lib->image_over = DT_LIB_DESERT;
    if(zoom != 1 || (zoom == 1 && selected))
    {
      cairo_set_line_width(cr, 1.5);
      cairo_set_source_rgb(cr, outlinecol, outlinecol, outlinecol);
      cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
      float r1, r2;
      if(zoom != 1) 
      {
        r1 = 0.06*width;
        r2 = 0.025*width;
      }
      else
      {
        r1 = 0.02*fscale;
        r2 = 0.0083*fscale;
      }
      for(int k=0;k<5;k++)
      {
        float x, y;
        if(zoom != 1) 
        {
          x = (0.15+k*0.15)*width;
          y = 0.88*height;
        }
        else
        {
          x = (.04+k*0.04)*fscale;
          y = .12*fscale;
        }
        if(k == 4)
        {
          if(altered) dt_draw_altered(cr, x, y, (r1+r2)*.5);
        }
        else
        {
          dt_library_star(cr, x, y, r1, r2);
          if((px - x)*(px - x) + (py - y)*(py - y) < r1*r1)
          {
            lib->image_over = DT_LIB_STAR_1 + k;
            cairo_fill(cr);
          }
          else if((img->flags & 0x7) > k)
          {
            cairo_fill_preserve(cr);
            cairo_set_source_rgb(cr, 1.0-bordercol, 1.0-bordercol, 1.0-bordercol);
            cairo_stroke(cr);
            cairo_set_source_rgb(cr, outlinecol, outlinecol, outlinecol);
          }
          else cairo_stroke(cr);
        }
      }
    }
  }

  if(selected && (zoom == 1))
  { // some exif data
    cairo_set_source_rgb(cr, .7, .7, .7);
    cairo_select_font_face (cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size (cr, .025*fscale);

    cairo_move_to (cr, .02*fscale, .04*fscale);
    // cairo_show_text(cr, img->filename);
    cairo_text_path(cr, img->filename);
    char exifline[50];
    cairo_move_to (cr, .02*fscale, .08*fscale);
    dt_image_print_exif(img, exifline, 50);
    cairo_text_path(cr, exifline);
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_stroke(cr);
  }

  // if(zoom == 1) cairo_set_antialias(cr, CAIRO_ANTIALIAS_DEFAULT);
#endif
}

static void
dt_library_toggle_selection(int iid)
{
  int rc;
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(darktable.db, "select * from selected_images where imgid = ?1", -1, &stmt, NULL);
  rc = sqlite3_bind_int (stmt, 1, iid);
  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    sqlite3_finalize(stmt);
    rc = sqlite3_prepare_v2(darktable.db, "delete from selected_images where imgid = ?1", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, iid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  else
  {
    sqlite3_finalize(stmt);
    rc = sqlite3_prepare_v2(darktable.db, "insert into selected_images values (?1)", -1, &stmt, NULL);
    rc = sqlite3_bind_int (stmt, 1, iid);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

static void
expose_filemanager (dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  const int iir = dt_conf_get_int("plugins/lighttable/images_in_row");
  lib->image_over = DT_LIB_DESERT;
  int32_t mouse_over_id;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
  cairo_set_source_rgb (cr, .9, .9, .9);
  cairo_paint(cr);

  if(lib->track >  2) lib->offset += iir;
  if(lib->track < -2) lib->offset -= iir;
  lib->track = 0;
  if(lib->center) lib->offset = 0;
  lib->center = 0;
  int offset = lib->offset;
  static int oldpan = 0;
  const int pan = lib->pan;

  const float wd = width/(float)iir;
  const float ht = width/(float)iir;

  const int seli = pointerx / (float)wd;
  const int selj = pointery / (float)ht;

  const int img_pointerx = fmodf(pointerx, wd);
  const int img_pointery = fmodf(pointery, ht);

  const int max_rows = 1 + (int)((height)/ht + .5);
  const int max_cols = iir;
  sqlite3_stmt *stmt = NULL;
  int id, last_seli = 1<<30, last_selj = 1<<30;
  int clicked1 = (oldpan == 0 && pan == 1 && lib->button == 1);

  gchar *query = dt_conf_get_string ("plugins/lighttable/query");
  if(!query) return;
  if(query[0] == '\0')
  {
    g_free(query);
    return;
  }
  char newquery[1024];
  snprintf(newquery, 1024, "select count(id) %s", query + 8);
  sqlite3_prepare_v2(darktable.db, newquery, -1, &stmt, NULL);
  sqlite3_bind_int (stmt, 1, 0);
  sqlite3_bind_int (stmt, 2, -1);
  int count = 1;
  if(sqlite3_step(stmt) == SQLITE_ROW)
    count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  if(offset < 0)         lib->offset = offset = 0;
  if(offset > count-iir) lib->offset = offset = count-iir;
  dt_view_set_scrollbar(self, 0, 1, 1, offset, count, max_cols*iir);

  sqlite3_prepare_v2(darktable.db, query, -1, &stmt, NULL);
  sqlite3_bind_int (stmt, 1, offset);
  sqlite3_bind_int (stmt, 2, max_rows*iir);
  g_free(query);
  for(int row = 0; row < max_rows; row++)
  {
    for(int col = 0; col < max_cols; col++)
    {
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        id = sqlite3_column_int(stmt, 0);
        dt_image_t *image = dt_image_cache_get(id, 'r');
        if(image)
        {
          // set mouse over id
          if(seli == col && selj == row)
          {
            mouse_over_id = image->id;
            DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, mouse_over_id);
          }
          // add clicked image to selected table
          if(clicked1)
          {
            if((lib->modifiers & GDK_SHIFT_MASK) == 0 && (lib->modifiers & GDK_CONTROL_MASK) == 0 && seli == col && selj == row)
            { // clear selected if no modifier
              sqlite3_stmt *stmt2;
              sqlite3_prepare_v2(darktable.db, "delete from selected_images where imgid != ?1", -1, &stmt2, NULL);
              sqlite3_bind_int(stmt2, 1, image->id);
              sqlite3_step(stmt2);
              sqlite3_finalize(stmt2);
            }
            if((lib->modifiers & GDK_SHIFT_MASK) && image->id == lib->last_selected_id) { last_seli = col; last_selj = row; }
            if((last_seli < (1<<30) && ((lib->modifiers & GDK_SHIFT_MASK) && (col >= last_seli && row >= last_selj &&
                    col <= seli && row <= selj) && (col != last_seli || row != last_selj))) ||
               (seli == col && selj == row))
            { // insert all in range if shift, or only the one the mouse is over for ctrl or plain click.
              dt_library_toggle_selection(image->id);
              lib->last_selected_id = image->id;
            }
          }
          cairo_save(cr);
          dt_image_expose(image, lib, image->id, cr, wd, iir == 1 ? height : ht, iir, img_pointerx, img_pointery);
          cairo_restore(cr);
          dt_image_cache_release(image, 'r');
        }
      }
      else goto failure;
      cairo_translate(cr, wd, 0.0f);
    }
    cairo_translate(cr, -max_cols*wd, ht);
  }
failure:
  sqlite3_finalize(stmt);

  oldpan = pan;
#ifdef _DEBUG
  if(darktable.unmuted & DT_DEBUG_CACHE)
    dt_mipmap_cache_print(darktable.mipmap_cache);
#endif
}

static void
expose_zoomable (dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  static int last_offset = 0x7fffffff;
  float zoom, zoom_x, zoom_y;
  int32_t mouse_over_id, pan, track, center;
  DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
  zoom   = dt_conf_get_int("plugins/lighttable/images_in_row");
  zoom_x = lib->zoom_x;
  zoom_y = lib->zoom_y;
  pan    = lib->pan;
  center = lib->center;
  track  = lib->track;

  lib->image_over = DT_LIB_DESERT;

  if(zoom == 1) cairo_set_source_rgb (cr, .8, .8, .8);
  else cairo_set_source_rgb (cr, .9, .9, .9);
  cairo_paint(cr);

  const float wd = width/zoom;
  const float ht = width/zoom;

  static int oldpan = 0;
  static float oldzoom = -1;
  if(oldzoom < 0) oldzoom = zoom;

  // TODO: exaggerate mouse gestures to pan when zoom == 1
  if(pan)// && mouse_over_id >= 0)
  {
    zoom_x = lib->select_offset_x - /* (zoom == 1 ? 2. : 1.)*/pointerx;
    zoom_y = lib->select_offset_y - /* (zoom == 1 ? 2. : 1.)*/pointery;
  }

  gchar *query = dt_conf_get_string ("plugins/lighttable/query");
  if(!query) return;
  if(query[0] == '\0')
  {
    g_free(query);
    return;
  }

  if     (track == 0);
  else if(track >  1)  zoom_y += ht;
  else if(track >  0)  zoom_x += wd;
  else if(track > -2)  zoom_x -= wd;
  else                 zoom_y -= ht;
  if(zoom > DT_LIBRARY_MAX_ZOOM)
  { // double speed.
    if     (track == 0);
    else if(track >  1)  zoom_y += ht;
    else if(track >  0)  zoom_x += wd;
    else if(track > -2)  zoom_x -= wd;
    else                 zoom_y -= ht;
    if(zoom > 1.5*DT_LIBRARY_MAX_ZOOM)
    { // quad speed.
      if     (track == 0);
      else if(track >  1)  zoom_y += ht;
      else if(track >  0)  zoom_x += wd;
      else if(track > -2)  zoom_x -= wd;
      else                 zoom_y -= ht;
    }
  }

  if(oldzoom != zoom)
  {
    float oldx = (pointerx + zoom_x)*oldzoom/width;
    float oldy = (pointery + zoom_y)*oldzoom/width;
    if(zoom == 1)
    {
      zoom_x = (int)oldx*wd;
      zoom_y = (int)oldy*ht;
      last_offset = 0x7fffffff;
    }
    else
    {
      zoom_x = oldx*wd - pointerx;
      zoom_y = oldy*ht - pointery;
    }
  }
  oldzoom = zoom;

  if(center)
  {
    if(mouse_over_id >= 0)
    {
      zoom_x = wd*((int)(zoom_x)/(int)wd);
      zoom_y = ht*((int)(zoom_y)/(int)ht);
    }
    else zoom_x = zoom_y = 0.0;
    center = 0;
  }

  // mouse left the area, but we leave mouse over as it was, especially during panning
  // if(!pan && pointerx > 0 && pointerx < width && pointery > 0 && pointery < height) DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
  if(!pan && zoom != 1) DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);

  // set scrollbar positions, clamp zoom positions
  sqlite3_stmt *stmt = NULL;
  int rc;
  char newquery[1024];
  snprintf(newquery, 1024, "select count(id) %s", query + 8);
  sqlite3_prepare_v2(darktable.db, newquery, -1, &stmt, NULL);
  sqlite3_bind_int (stmt, 1, 0);
  sqlite3_bind_int (stmt, 2, -1);
  int count = 1;
  if(sqlite3_step(stmt) == SQLITE_ROW)
    count = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  if(zoom_x < -wd*DT_LIBRARY_MAX_ZOOM/2)  zoom_x = -wd*DT_LIBRARY_MAX_ZOOM/2;
  if(zoom_x >  wd*DT_LIBRARY_MAX_ZOOM-wd) zoom_x =  wd*DT_LIBRARY_MAX_ZOOM-wd;
  if(zoom_y < -height+ht)                 zoom_y = -height+ht;
  if(zoom_y >  ht*count/zoom-ht)          zoom_y =  ht*count/zoom-ht;

  int offset_i = (int)(zoom_x/wd);
  int offset_j = (int)(zoom_y/ht);
  // arbitrary 1000 to avoid bug due to round towards zero using (int)
  int seli = zoom == 1 ? 0 : (int)(1000 + (pointerx + zoom_x)/wd) - MAX(offset_i, 0) - 1000;
  int selj = zoom == 1 ? 0 : (int)(1000 + (pointery + zoom_y)/ht) - offset_j         - 1000;
  float offset_x = zoom == 1 ? 0.0 : zoom_x/wd - (int)(zoom_x/wd);
  float offset_y = zoom == 1 ? 0.0 : zoom_y/ht - (int)(zoom_y/ht);
  const int max_rows = zoom == 1 ? 1 : 2 + (int)((height)/ht + .5);
  const int max_cols = zoom == 1 ? 1 : MIN(DT_LIBRARY_MAX_ZOOM - MAX(0, offset_i), 1 + (int)(zoom+.5));

  int offset = MAX(0, offset_i) + DT_LIBRARY_MAX_ZOOM*offset_j;
  int img_pointerx = fmodf(pointerx + zoom_x, wd);
  int img_pointery = fmodf(pointery + zoom_y, ht);

  // assure 1:1 is not switching images on resize/tab events:
  if(!track && last_offset != 0x7fffffff && zoom == 1)
  {
    offset = last_offset;
    zoom_x = wd*(offset % DT_LIBRARY_MAX_ZOOM);
    zoom_y = ht*(offset / DT_LIBRARY_MAX_ZOOM);
  }
  else last_offset = offset;

  int id, clicked1, last_seli = 1<<30, last_selj = 1<<30;
  clicked1 = (oldpan == 0 && pan == 1 && lib->button == 1);

  dt_view_set_scrollbar(self, MAX(0, offset_i), DT_LIBRARY_MAX_ZOOM, zoom, DT_LIBRARY_MAX_ZOOM*offset_j, count, DT_LIBRARY_MAX_ZOOM*max_cols);

  sqlite3_prepare_v2(darktable.db, query, -1, &stmt, NULL);
  g_free(query);
  cairo_translate(cr, -offset_x*wd, -offset_y*ht);
  cairo_translate(cr, -MIN(offset_i*wd, 0.0), 0.0);
  for(int row = 0; row < max_rows; row++)
  {
    if(offset < 0)
    {
      cairo_translate(cr, 0, ht);
      offset += DT_LIBRARY_MAX_ZOOM;
      continue;
    }
    rc = sqlite3_bind_int  (stmt, 1, offset);
    rc = sqlite3_bind_int  (stmt, 2, max_cols);
    for(int col = 0; col < max_cols; col++)
    {
      if(sqlite3_step(stmt) == SQLITE_ROW)
      {
        id = sqlite3_column_int(stmt, 0);
        dt_image_t *image = dt_image_cache_get(id, 'r');
        if(image)
        {
          // printf("flags %d > k %d\n", image->flags, col);

          // set mouse over id
          if((zoom == 1 && mouse_over_id < 0) || ((!pan || track) && seli == col && selj == row))
          {
            mouse_over_id = image->id;
            DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, mouse_over_id);
          }
          // add clicked image to selected table
          if(clicked1)
          {
            if((lib->modifiers & GDK_SHIFT_MASK) == 0 && (lib->modifiers & GDK_CONTROL_MASK) == 0 && seli == col && selj == row)
            { // clear selected if no modifier
              sqlite3_stmt *stmt2;
              sqlite3_prepare_v2(darktable.db, "delete from selected_images where imgid != ?1", -1, &stmt2, NULL);
              sqlite3_bind_int(stmt2, 1, image->id);
              sqlite3_step(stmt2);
              sqlite3_finalize(stmt2);
            }
            // FIXME: whatever comes first assumtion is broken!
            // if((lib->modifiers & GDK_SHIFT_MASK) && (last_seli == (1<<30)) &&
            //    (image->id == lib->last_selected_id || image->id == mouse_over_id)) { last_seli = col; last_selj = row; }
            // if(last_seli < (1<<30) && ((lib->modifiers & GDK_SHIFT_MASK) && (col >= MIN(last_seli,seli) && row >= MIN(last_selj,selj) &&
            //         col <= MAX(last_seli,seli) && row <= MAX(last_selj,selj)) && (col != last_seli || row != last_selj)) ||
            if((lib->modifiers & GDK_SHIFT_MASK) && image->id == lib->last_selected_id) { last_seli = col; last_selj = row; }
            if((last_seli < (1<<30) && ((lib->modifiers & GDK_SHIFT_MASK) && (col >= last_seli && row >= last_selj &&
                    col <= seli && row <= selj) && (col != last_seli || row != last_selj))) ||
               (seli == col && selj == row))
            { // insert all in range if shift, or only the one the mouse is over for ctrl or plain click.
              dt_library_toggle_selection(image->id);
              lib->last_selected_id = image->id;
            }
          }
          cairo_save(cr);
          dt_image_expose(image, lib, image->id, cr, wd, zoom == 1 ? height : ht, zoom, img_pointerx, img_pointery);
          cairo_restore(cr);
          dt_image_cache_release(image, 'r');
        }
      }
      else goto failure;
      cairo_translate(cr, wd, 0.0f);
    }
    cairo_translate(cr, -max_cols*wd, ht);
    offset += DT_LIBRARY_MAX_ZOOM;
    rc = sqlite3_reset(stmt);
    rc = sqlite3_clear_bindings(stmt);
  }
failure:
  sqlite3_finalize(stmt);

  oldpan = pan;
  lib->zoom_x = zoom_x;
  lib->zoom_y = zoom_y;
  lib->track  = 0;
  lib->center = center;
  // dt_mipmap_cache_print(darktable.mipmap_cache);
  // dt_image_cache_print(darktable.image_cache);
#ifdef _DEBUG
  if(darktable.unmuted & DT_DEBUG_CACHE)
    dt_mipmap_cache_print(darktable.mipmap_cache);
#endif
}

void expose(dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx, int32_t pointery)
{
  const int i = dt_conf_get_int("plugins/lighttable/layout");
  switch(i)
  {
    case 1: // file manager
      expose_filemanager(self, cr, width, height, pointerx, pointery);
      break;
    default: // zoomable
      expose_zoomable(self, cr, width, height, pointerx, pointery);
      break;
  }
}

void enter(dt_view_t *self)
{
  // add expanders
  GtkBox *box = GTK_BOX(glade_xml_get_widget (darktable.gui->main_window, "plugins_vbox"));
  GList *modules = g_list_last(darktable.lib->plugins);
  while(modules)
  {
    dt_lib_module_t *module = (dt_lib_module_t *)(modules->data);
    module->gui_init(module);
    module->gui_reset(module);
    // add the widget created by gui_init to an expander and both to list.
    GtkWidget *expander = dt_lib_gui_get_expander(module);
    gtk_box_pack_start(box, expander, FALSE, FALSE, 0);
    modules = g_list_previous(modules);
  }

  // end marker widget:
  GtkWidget *endmarker = gtk_drawing_area_new();
  gtk_widget_set_size_request(GTK_WIDGET(endmarker), 250, 50);
  gtk_box_pack_start(box, endmarker, FALSE, FALSE, 0);
  g_signal_connect (G_OBJECT (endmarker), "expose-event",
                    G_CALLBACK (dt_control_expose_endmarker), 0);

  gtk_widget_show_all(GTK_WIDGET(box));

  // close expanders
  modules = darktable.lib->plugins;
  while(modules)
  {
    dt_lib_module_t *module = (dt_lib_module_t *)(modules->data);
    char var[1024];
    snprintf(var, 1024, "plugins/lighttable/%s/expanded", module->plugin_name);
    gboolean expanded = dt_conf_get_bool(var);
    gtk_expander_set_expanded (module->expander, expanded);
    if(expanded) gtk_widget_show_all(module->widget);
    else         gtk_widget_hide_all(module->widget);
    modules = g_list_next(modules);
  }
}

void dt_lib_remove_child(GtkWidget *widget, gpointer data)
{
  gtk_container_remove(GTK_CONTAINER(data), widget);
}

void leave(dt_view_t *self)
{
  GList *it = darktable.lib->plugins;
  while(it)
  {
    dt_lib_module_t *module = (dt_lib_module_t *)(it->data);
    module->gui_cleanup(module);
    it = g_list_next(it);
  }
  GtkBox *box = GTK_BOX(glade_xml_get_widget (darktable.gui->main_window, "plugins_vbox"));
  gtk_container_foreach(GTK_CONTAINER(box), (GtkCallback)dt_lib_remove_child, (gpointer)box);
}

void reset(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->center = 1;
  lib->track = lib->pan = 0;
  DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
}


void mouse_leave(dt_view_t *self)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  if(!lib->pan && dt_conf_get_int("plugins/lighttable/images_in_row") != 1)
  {
    DT_CTL_SET_GLOBAL(lib_image_mouse_over_id, -1);
    dt_control_queue_draw_all(); // remove focus
  }
}


void mouse_moved(dt_view_t *self, double x, double y, int which)
{
  // update stars/etc :(
  dt_control_queue_draw_all();
}


void button_released(dt_view_t *self, double x, double y, int which, uint32_t state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->pan = 0;
  if(which == 1) dt_control_change_cursor(GDK_ARROW);
}


void button_pressed(dt_view_t *self, double x, double y, int which, int type, uint32_t state)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  lib->modifiers = state;
  lib->button = which;
  lib->select_offset_x = lib->zoom_x;
  lib->select_offset_y = lib->zoom_y;
  lib->select_offset_x += x;
  lib->select_offset_y += y;
  lib->pan = 1;
  if(which == 1) dt_control_change_cursor(GDK_HAND1);
  // image button pressed?
  switch(lib->image_over)
  {
    case DT_LIB_DESERT: break;
    case DT_LIB_STAR_1: case DT_LIB_STAR_2: case DT_LIB_STAR_3: case DT_LIB_STAR_4:
    { 
      int32_t mouse_over_id;
      DT_CTL_GET_GLOBAL(mouse_over_id, lib_image_mouse_over_id);
      dt_image_t *image = dt_image_cache_get(mouse_over_id, 'r');
      if(lib->image_over == DT_LIB_STAR_1 && ((image->flags & 0x7) == 1)) image->flags &= ~0x7;
      else
      {
        image->flags &= ~0x7;
        image->flags |= lib->image_over;
      }
      dt_image_cache_flush(image);
      dt_image_cache_release(image, 'r');
      break;
    }
    default:
      break;
  }
}


void key_pressed(dt_view_t *self, uint16_t which)
{
  dt_library_t *lib = (dt_library_t *)self->data;
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "lighttable_zoom_spinbutton");
  int zoom = dt_conf_get_int("plugins/lighttable/images_in_row");
  switch (which)
  {
    case KEYCODE_Left: case KEYCODE_a:
      lib->track = -1;
      break;
    case KEYCODE_Right: case KEYCODE_e:
      lib->track = 1;
      break;
    case KEYCODE_Up: case KEYCODE_comma:
      lib->track = -DT_LIBRARY_MAX_ZOOM;
      break;
    case KEYCODE_Down: case KEYCODE_o:
      lib->track = DT_LIBRARY_MAX_ZOOM;
      break;
    case KEYCODE_1:
      zoom = 1;
      break;
    case KEYCODE_2:
      if(zoom <= 1) zoom = 1;
      else zoom --;
      lib->center = 1;
      break;
    case KEYCODE_3:
      if(zoom >= 2*DT_LIBRARY_MAX_ZOOM) zoom = 2*DT_LIBRARY_MAX_ZOOM;
      else zoom ++;
      lib->center = 1;
      break;
    case KEYCODE_4:
      zoom = DT_LIBRARY_MAX_ZOOM;
      break;
    case KEYCODE_apostrophe:
      lib->center = 1;
      break;
    default:
      break;
  }
  gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), zoom);
}

void border_scrolled(dt_view_t *view, double x, double y, int which, int up)
{
  dt_library_t *lib = (dt_library_t *)view->data;
  if(which == 0 || which == 1)
  {
    if(up) lib->track = -DT_LIBRARY_MAX_ZOOM;
    else   lib->track =  DT_LIBRARY_MAX_ZOOM;
  }
  else if(which == 2 || which == 3)
  {
    if(up) lib->track = -1;
    else   lib->track =  1;
  }
  dt_control_queue_draw_all();
}

void scrolled(dt_view_t *view, double x, double y, int up)
{
  dt_library_t *lib = (dt_library_t *)view->data;
  GtkWidget *widget = glade_xml_get_widget (darktable.gui->main_window, "lighttable_zoom_spinbutton");
  const int layout = dt_conf_get_int("plugins/lighttable/layout");
  if(layout == 1)
  {
    if(up) lib->track = -DT_LIBRARY_MAX_ZOOM;
    else   lib->track =  DT_LIBRARY_MAX_ZOOM;
  }
  else
  { // zoom
    int zoom = dt_conf_get_int("plugins/lighttable/images_in_row");
    if(up)
    {
      zoom--;
      if(zoom < 1) zoom = 1;
    }
    else
    {
      zoom++;
      if(zoom > 2*DT_LIBRARY_MAX_ZOOM) zoom = 2*DT_LIBRARY_MAX_ZOOM;
    }
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), zoom);
  }
}

