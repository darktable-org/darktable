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

#include <math.h>
#include "paint.h"

#ifndef M_PI
#define M_PI 3.141592654
#endif

void dtgtk_cairo_paint_color(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags)
{
  cairo_translate(cr, x, y);
  cairo_scale(cr,w,h);
  cairo_set_line_width(cr,0.1);
  cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);
  cairo_rectangle(cr,0.1,0.1,0.8,0.8);
  cairo_fill(cr);
  cairo_set_source_rgba(cr,0,0,0,0.6);
  cairo_stroke(cr);
   cairo_identity_matrix(cr);
}

void dtgtk_cairo_paint_presets(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags)
{
  gint s=w<h?w:h;
  cairo_translate(cr, x+(w/2.0)-(s/2.0), y+(h/2.0)-(s/2.0));
  cairo_scale(cr,s,s);
  cairo_set_line_width(cr,0.15);
  cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);
  cairo_move_to(cr,0.2,0.2);
  cairo_line_to(cr,0.8,0.2);
  cairo_move_to(cr,0.2,0.5);
  cairo_line_to(cr,0.8,0.5);
  cairo_move_to(cr,0.2,0.8);
  cairo_line_to(cr,0.8,0.8);
  cairo_stroke(cr);
  
  cairo_identity_matrix(cr);
}

void dtgtk_cairo_paint_arrow(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags)
{
  cairo_matrix_t hflip_matrix;
  cairo_matrix_init(&hflip_matrix,-1,0,0,1,1,0);
  
  double C=cos(-(M_PI/2.0)),S=sin(-(M_PI/2.0));  // -90 degrees
  C=flags&CPF_DIRECTION_UP?cos(-(M_PI*1.5)):C;	  
  S=flags&CPF_DIRECTION_UP?sin(-(M_PI*1.5)):S;	  
  cairo_matrix_t rotation_matrix;
  cairo_matrix_init(&rotation_matrix,C,S,-S,C,0.5-C*0.5+S*0.5,0.5-S*0.5-C*0.5);
 
  gint s=w<h?w:h;
  cairo_translate(cr, x+(w/2.0)-(s/2.0), y+(h/2.0)-(s/2.0));
  cairo_scale(cr,s,s);
  cairo_set_line_width(cr,0.1);
  cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);
  
  if( flags&CPF_DIRECTION_UP || flags &CPF_DIRECTION_DOWN)
    cairo_transform(cr,&rotation_matrix);
  else if(flags&CPF_DIRECTION_RIGHT)	// Flip x transformation
    cairo_transform(cr,&hflip_matrix);
  
  cairo_move_to(cr,0.8,0.2);
  cairo_line_to(cr,0.2,0.5);
  cairo_line_to(cr,0.8,0.8);
  cairo_stroke(cr);
  cairo_identity_matrix(cr);
}

void dtgtk_cairo_paint_flip(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags) 
{
  double C=cos(-1.570796327),S=sin(-1.570796327);
  cairo_matrix_t rotation_matrix;
  cairo_matrix_init(&rotation_matrix,C,S,-S,C,0.5-C*0.5+S*0.5,0.5-S*0.5-C*0.5);
  gint s=w<h?w:h;
  cairo_translate(cr, x+(w/2.0)-(s/2.0), y+(h/2.0)-(s/2.0));
  cairo_scale(cr,s,s);
 
  cairo_set_line_width(cr,0.1);
  cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);
  if( (flags&CPF_DIRECTION_UP) ) // Rotate -90 degrees
    cairo_transform(cr,&rotation_matrix);
  
  cairo_move_to(cr,0.15,0.50);
  cairo_line_to(cr,0.15,0);
  cairo_line_to(cr,0.85,0.50);
  cairo_line_to(cr,0.2,0.50);
  cairo_stroke(cr);
  cairo_set_line_width(cr,0.05);
  cairo_move_to(cr,0.15,0.62);
  cairo_line_to(cr,0.15,1.0);
  cairo_line_to(cr,0.85,0.62);
  cairo_stroke(cr);
  cairo_identity_matrix(cr);
}

void dtgtk_cairo_paint_reset(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags) 
{
  gint s=w<h?w:h;
  cairo_translate(cr, x+(w/2.0)-(s/2.0), y+(h/2.0)-(s/2.0));
  cairo_scale(cr,s,s);
  cairo_set_line_width(cr,0.15);
  cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);
  cairo_arc (cr, 0.5, 0.5, 0.46, 0, 6.2832);
  cairo_move_to(cr,0.5,0.32);
  cairo_line_to(cr,0.5,0.68);
  cairo_stroke(cr);
  cairo_identity_matrix(cr);
}

void dtgtk_cairo_paint_store(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags)
{
  gint s=w<h?w:h;
  cairo_translate(cr, x+(w/2.0)-(s/2.0), y+(h/2.0)-(s/2.0));
  cairo_scale(cr,s,s);
  cairo_set_line_width(cr,0.15);
  cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);
  cairo_move_to(cr,0.5,0.10);
  cairo_line_to(cr,0.5,0.80);
  cairo_move_to(cr,0.30,0.5);
  cairo_line_to(cr,0.5,0.80);
  cairo_line_to(cr,0.70,0.5);
  
  cairo_move_to(cr,0.1,0.9);
  cairo_line_to(cr,0.9,0.9);
  
  cairo_stroke(cr);
  cairo_identity_matrix(cr);
}

void dtgtk_cairo_paint_switch(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags) 
{
  gint s=w<h?w:h;
  cairo_translate(cr, x+(w/2.0)-(s/2.0), y+(h/2.0)-(s/2.0));
  cairo_scale(cr,s,s);
  
  cairo_set_line_width(cr,0.125);
  cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);
  cairo_arc (cr, 0.5, 0.5, 0.45, (-50*3.145/180),(230*3.145/180));
  cairo_move_to(cr,0.5,0.05);
  cairo_line_to(cr,0.5,0.45);
  cairo_stroke(cr);

  if( (flags&CPF_ACTIVE) ) // If active add some green diffuse light
  {
    cairo_set_source_rgba(cr, 1,1,1,0.2);
    cairo_set_line_width(cr,0.25);
    cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);
    cairo_arc (cr, 0.5, 0.5, 0.45, (-50*3.145/180),(230*3.145/180));
    cairo_move_to(cr,0.5,0.1);
    cairo_line_to(cr,0.5,0.5);
    cairo_stroke(cr);
  }
  
  cairo_identity_matrix(cr);
}

void dtgtk_cairo_paint_eye(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags)
{
  gint s=w<h?w:h;
  cairo_translate(cr, x+(w/2.0)-(s/2.0), y+(h/2.0)-(s/2.0));
  cairo_scale(cr,s,s);
  cairo_set_line_width(cr,0.15);
  cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);
  
  cairo_arc (cr, 0.5, 0.5, 0.1,0, 6.2832);
  cairo_stroke(cr);
  
  cairo_translate(cr, 0,0.20);
  cairo_scale(cr,1.0,0.60);
  cairo_set_line_width(cr,0.2);
  cairo_arc (cr, 0.5, 0.5, 0.45, 0, 6.2832);
  cairo_stroke(cr);
  cairo_identity_matrix(cr);
  
}

void dtgtk_cairo_paint_timer(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags)
{
  gint s=w<h?w:h;
  cairo_translate(cr, x+(w/2.0)-(s/2.0), y+(h/2.0)-(s/2.0));
  cairo_scale(cr,s,s);
  cairo_set_line_width(cr,0.15);
  cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);

  
  
  cairo_identity_matrix(cr);
}

void dtgtk_cairo_paint_filmstrip(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags)
{
  gint s=w<h?w:h;
  cairo_translate(cr, x+(w/2.0)-(s/2.0), y+(h/2.0)-(s/2.0));
  cairo_scale(cr,s,s);
  cairo_set_line_width(cr,0.15);
  cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);

  cairo_identity_matrix(cr);	
}

void dtgtk_cairo_paint_directory(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags)
{
  cairo_save(cr);
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_translate(cr, x+.05*w, y+.05*h);
  cairo_scale(cr, .9*w, .9*h);
  cairo_set_line_width(cr, 1./w);
  cairo_rectangle(cr, 0., 0., 1., 1.);
  cairo_stroke(cr);
  cairo_move_to(cr, 0., .2);
  cairo_line_to(cr, .5, .2);
  cairo_line_to(cr, .6, 0.);
  cairo_stroke(cr);
  cairo_restore(cr);
}

void dtgtk_cairo_paint_refresh(cairo_t *cr,gint x,gint y,gint w,gint h,gint flags) {
  gint s=w<h?w:h;
  cairo_translate(cr, x+(w/2.0)-(s/2.0), y+(h/2.0)-(s/2.0));
  cairo_scale(cr,s,s);
	
  cairo_set_line_width(cr,0.15);
  cairo_set_line_cap(cr,CAIRO_LINE_CAP_ROUND);
  cairo_move_to(cr,0.65,0.1);
  cairo_line_to(cr,0.5,0.2);
  cairo_line_to(cr,0.65,0.3);
  cairo_stroke(cr);
	
  cairo_set_line_width(cr,0.10);
  cairo_arc (cr, 0.5, 0.5, 0.35,(-80*3.145/180),(220*3.145/180));
  cairo_stroke(cr);
 
  cairo_identity_matrix(cr);
}


