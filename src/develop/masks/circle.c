/*
    This file is part of darktable,
    copyright (c) 2012 aldric renaudin.

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
#include "develop/imageop.h"
#include "develop/blend.h"
#include "control/control.h"
#include "control/conf.h"
#include "develop/masks.h"
#include "common/debug.h"

static void dt_circle_get_distance(float x, int y, float as, dt_masks_form_gui_t *gui, int index, int *inside, int *inside_border, int *near, int *inside_source)
{
  if (!gui) return;
  
  int nb = 0;
  int last = -9999;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *) g_list_nth_data(gui->points,index);
  if (!gpt) return;
  
  //we first check if we are inside the source form
  if (gpt->source_count>6)
  {
    for (int i=2; i<gpt->source_count; i++)
    {
      int yy = (int) gpt->source[i*2+1];
      if (yy != last && yy == y)
      {
        if (gpt->source[i*2] > x) nb++;
      }
      last = yy;
    }  
    if (nb & 1)
    {
      *inside_source = 1;
      *inside = 1;
      *inside_border = 0;
      *near = -1;
      return;
    }
  }
  *inside_source = 0;
  
  //we check if it's inside borders
  nb=0;
  for (int i=2; i<gpt->border_count; i++)
  {
    int yy = (int) gpt->border[i*2+1];
    if (yy != last && yy == y)
    {
      if (gpt->border[i*2] > x) nb++;
    }
    last = yy;
  }  
  if (!(nb & 1))
  {
    *inside = 0;
    *inside_border = 0;
    *near = -1;
    return;
  }
  *inside = 1;
  *near = 0;
  
  //and we check if it's inside form
  nb = 0;
  last = -9999;
  for (int i=2; i<gpt->points_count; i++)
  {
    int yy = (int) gpt->points[i*2+1];
    if (yy != last && yy == y)
    {
      if (gpt->points[i*2] > x) nb++;
      if (gpt->points[i*2] - x < as && gpt->points[i*2] - x > -as) *near = 1;
    }
    last = yy;
  }
  *inside_border = !(nb & 1);
}

static int dt_circle_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up, uint32_t state,
                                          dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, int index)
{
  if (gui->form_selected)
  {
    if ((state&GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
    {
      //we try to change the opacity
      dt_masks_form_change_opacity(form,parentid,up);
    }
    else
    {
      dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *) (g_list_first(form->points)->data);
      if (gui->border_selected)
      {
        if(up && circle->border > 0.002f) circle->border *= 0.9f;
        else  if(circle->border < 1.0f  ) circle->border *= 1.0f/0.9f;
        dt_masks_write_form(form,darktable.develop);
        dt_masks_gui_form_remove(form,gui,index);
        dt_masks_gui_form_create(form,gui,index);
      }
      else
      {
        if(up && circle->radius > 0.002f) circle->radius *= 0.9f;
        else  if(circle->radius < 1.0f  ) circle->radius *= 1.0f/0.9f;
        dt_masks_write_form(form,darktable.develop);
        dt_masks_gui_form_remove(form,gui,index);
        dt_masks_gui_form_create(form,gui,index);
      }
      dt_masks_update_image(darktable.develop);
    }
    return 1;
  }
  return 0;
}

static int dt_circle_events_button_pressed(struct dt_iop_module_t *module,float pzx, float pzy, int which, int type, uint32_t state,
                                          dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, int index)
{
  if (which != 1) return 0;
  if (!gui) return 0;
  if (gui->source_selected && !gui->creation)
  {
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *) g_list_nth_data(gui->points,index);
    if (!gpt) return 0;
    //we start the form dragging
    gui->source_dragging = TRUE;
    gui->posx = pzx*darktable.develop->preview_pipe->backbuf_width;
    gui->posy = pzy*darktable.develop->preview_pipe->backbuf_height;
    gui->dx = gpt->source[0] - gui->posx;
    gui->dy = gpt->source[1] - gui->posy;
    return 1;
  }
  else if (gui->form_selected && !gui->creation)
  {
    dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *) g_list_nth_data(gui->points,index);
    if (!gpt) return 0;
    //we start the form dragging
    gui->form_dragging = TRUE;
    gui->posx = pzx*darktable.develop->preview_pipe->backbuf_width;
    gui->posy = pzy*darktable.develop->preview_pipe->backbuf_height;
    gui->dx = gpt->points[0] - gui->posx;
    gui->dy = gpt->points[1] - gui->posy;
    return 1;
  }
  else if (gui->creation)
  {
    dt_iop_module_t *crea_module = gui->creation_module;
    //we create the circle
    dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *) (malloc(sizeof(dt_masks_point_circle_t)));
    
    //we change the center value
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = {pzx*wd,pzy*ht};
    dt_dev_distort_backtransform(darktable.develop,pts,1);
    circle->center[0] = pts[0]/darktable.develop->preview_pipe->iwidth;
    circle->center[1] = pts[1]/darktable.develop->preview_pipe->iheight;
    circle->radius = 0.1f;
    circle->border = 0.05f;
    form->points = g_list_append(form->points,circle);
    form->source[0] = circle->center[0] + 0.1f;
    form->source[1] = circle->center[1] + 0.1f;
    dt_masks_gui_form_save_creation(crea_module,form,gui);
    
    if (crea_module)
    {    
      //we save the move
      dt_dev_add_history_item(darktable.develop, crea_module, TRUE);
      //and we switch in edit mode to show all the forms
      dt_masks_set_edit_mode(crea_module, TRUE);
      dt_masks_iop_update(crea_module);
      gui->creation_module = NULL;
    }
    else
    {
      //we select the new form
      dt_dev_masks_selection_change(darktable.develop,form->formid,TRUE);     
    }
    return 1;
  }
  return 0;
}

static int dt_circle_events_button_released(struct dt_iop_module_t *module,float pzx, float pzy, int which, uint32_t state,
                                          dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui,int index)
{
  if (which == 3)
  {
    dt_masks_init_formgui(darktable.develop);
    //we hide the form
    if (!(darktable.develop->form_visible->type & DT_MASKS_GROUP)) darktable.develop->form_visible = NULL;
    else if (g_list_length(darktable.develop->form_visible->points) < 2) darktable.develop->form_visible = NULL;
    else
    {
      GList *forms = g_list_first(darktable.develop->form_visible->points);
      while (forms)
      {
        dt_masks_point_group_t *gpt = (dt_masks_point_group_t *)forms->data;
        if (gpt->formid == form->formid)
        {
          darktable.develop->form_visible->points = g_list_remove(darktable.develop->form_visible->points,gpt);
          break;
        }
        forms = g_list_next(forms);
      }
    }    
    
    //we delete or remove the shape
    int id = 0;
    if(module) id = module->blend_params->mask_id;
    dt_masks_form_remove(module,dt_masks_get_from_id(darktable.develop,id),form);
    dt_dev_masks_list_change(darktable.develop);
    return 1;
  }
  if (gui->form_dragging)
  {
    //we get the circle
    dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *) (g_list_first(form->points)->data);
    
    //we end the form dragging
    gui->form_dragging = FALSE;
    
    //we change the center value
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = {pzx*wd+gui->dx,pzy*ht+gui->dy};
    dt_dev_distort_backtransform(darktable.develop,pts,1);
    circle->center[0] = pts[0]/darktable.develop->preview_pipe->iwidth;
    circle->center[1] = pts[1]/darktable.develop->preview_pipe->iheight;
    dt_masks_write_form(form,darktable.develop);

    //we recreate the form points
    dt_masks_gui_form_remove(form,gui,index);
    dt_masks_gui_form_create(form,gui,index);
    
    //we save the move
    dt_masks_update_image(darktable.develop);
    
    return 1;
  }
  else if (gui->source_dragging)
  {
    //we end the form dragging
    gui->source_dragging = FALSE;
    
    //we change the center value
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = {pzx*wd+gui->dx,pzy*ht+gui->dy};
    dt_dev_distort_backtransform(darktable.develop,pts,1);
    form->source[0] = pts[0]/darktable.develop->preview_pipe->iwidth;
    form->source[1] = pts[1]/darktable.develop->preview_pipe->iheight;
    dt_masks_write_form(form,darktable.develop);

    //we recreate the form points
    dt_masks_gui_form_remove(form,gui,index);
    dt_masks_gui_form_create(form,gui,index);
    
    //we save the move
    dt_masks_update_image(darktable.develop);
    
    return 1;
  }
  return 0;
}

static int dt_circle_events_mouse_moved(struct dt_iop_module_t *module,float pzx, float pzy, int which, dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, int index)
{
  if (gui->form_dragging || gui->source_dragging)
  {
    gui->posx = pzx*darktable.develop->preview_pipe->backbuf_width;
    gui->posy = pzy*darktable.develop->preview_pipe->backbuf_height;
    dt_control_queue_redraw_center();
    return 1;
  }
  else if (!gui->creation)
  {
    int32_t zoom, closeup;
    DT_CTL_GET_GLOBAL(zoom, dev_zoom);
    DT_CTL_GET_GLOBAL(closeup, dev_closeup);
    float zoom_scale = dt_dev_get_zoom_scale(darktable.develop, zoom, closeup ? 2 : 1, 1);
    float as = 0.005f/zoom_scale*darktable.develop->preview_pipe->backbuf_width;
    int in,inb,near,ins;
    dt_circle_get_distance(pzx*darktable.develop->preview_pipe->backbuf_width,pzy*darktable.develop->preview_pipe->backbuf_height,as,gui,index,&in,&inb,&near,&ins);
    if (ins)
    {
      gui->form_selected = TRUE;
      gui->source_selected = TRUE;
      gui->border_selected = FALSE;
    }
    else if (inb)
    {
      gui->form_selected = TRUE;
      gui->border_selected = TRUE;
      gui->source_selected = FALSE;
    }
    else if (in)
    {
      gui->form_selected = TRUE;
      gui->border_selected = FALSE;
      gui->source_selected = FALSE;
    }
    else
    {
      gui->form_selected = FALSE;
      gui->border_selected = FALSE;
      gui->source_selected = FALSE;
    }
    dt_control_queue_redraw_center();
    if (!gui->form_selected && !gui->border_selected) return 0;
    return 1;
  }
  
  return 0;
}

static void dt_circle_events_post_expose(cairo_t *cr,float zoom_scale,dt_masks_form_gui_t *gui,int index)
{
  double dashed[] = {4.0, 4.0};
  dashed[0] /= zoom_scale;
  dashed[1] /= zoom_scale;
  int len  = sizeof(dashed) / sizeof(dashed[0]);
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *) g_list_nth_data(gui->points,index);
  if (!gpt) return;
  float dx=0, dy=0, dxs=0, dys=0; 
  if ((gui->group_selected == index) && gui->form_dragging)
  {
    dx = gui->posx + gui->dx - gpt->points[0];
    dy = gui->posy + gui->dy - gpt->points[1];
  }
  if ((gui->group_selected == index) && gui->source_dragging)
  {
    dxs = gui->posx + gui->dx - gpt->source[0];
    dys = gui->posy + gui->dy - gpt->source[1];
  }
  
  if (gpt->points_count > 6)
  { 
    cairo_set_dash(cr, dashed, 0, 0);     
    if ((gui->group_selected == index) && (gui->form_selected || gui->form_dragging)) cairo_set_line_width(cr, 5.0/zoom_scale);
    else                                     cairo_set_line_width(cr, 3.0/zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_move_to(cr,gpt->points[2]+dx,gpt->points[3]+dy);
    for (int i=2; i<gpt->points_count; i++)
    {
      cairo_line_to(cr,gpt->points[i*2]+dx,gpt->points[i*2+1]+dy);
    }
    cairo_line_to(cr,gpt->points[2]+dx,gpt->points[3]+dy);
    cairo_stroke_preserve(cr);
    if ((gui->group_selected == index) && (gui->form_selected || gui->form_dragging)) cairo_set_line_width(cr, 2.0/zoom_scale);
    else                                     cairo_set_line_width(cr, 1.0/zoom_scale);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_stroke(cr);
  }

  //draw border
  if ((gui->group_selected == index) && gpt->border_count > 6)
  { 
    cairo_set_dash(cr, dashed, len, 0);     
    if ((gui->group_selected == index) && (gui->border_selected)) cairo_set_line_width(cr, 2.0/zoom_scale);
    else                     cairo_set_line_width(cr, 1.0/zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
      
    cairo_move_to(cr,gpt->border[2]+dx,gpt->border[3]+dy);
    for (int i=2; i<gpt->border_count; i++)
    {
      cairo_line_to(cr,gpt->border[i*2]+dx,gpt->border[i*2+1]+dy);
    }
    cairo_line_to(cr,gpt->border[2]+dx,gpt->border[3]+dy);

    cairo_stroke_preserve(cr);
    if ((gui->group_selected == index) && (gui->border_selected)) cairo_set_line_width(cr, 2.0/zoom_scale);
    else                     cairo_set_line_width(cr, 1.0/zoom_scale);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_set_dash(cr, dashed, len, 4);
    cairo_stroke(cr);
  }
  
  //draw the source if any
  if (gpt->source_count>6)
  {
    //we draw the line between source and dest
    cairo_move_to(cr,gpt->source[0]+dxs,gpt->source[1]+dys);
    cairo_line_to(cr,gpt->points[0]+dx,gpt->points[1]+dy);
    cairo_set_dash(cr, dashed, 0, 0);     
    if ((gui->group_selected == index) && (gui->form_selected || gui->form_dragging)) cairo_set_line_width(cr, 2.5/zoom_scale);
    else                                     cairo_set_line_width(cr, 1.5/zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_stroke_preserve(cr);
    if ((gui->group_selected == index) && (gui->form_selected || gui->form_dragging)) cairo_set_line_width(cr, 1.0/zoom_scale);
    else                                     cairo_set_line_width(cr, 0.5/zoom_scale);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_stroke(cr);
    
    //we draw the source
    cairo_set_dash(cr, dashed, 0, 0);     
    if ((gui->group_selected == index) && (gui->form_selected || gui->form_dragging)) cairo_set_line_width(cr, 2.5/zoom_scale);
    else                                     cairo_set_line_width(cr, 1.5/zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_move_to(cr,gpt->source[2]+dxs,gpt->source[3]+dys);
    for (int i=2; i<gpt->source_count; i++)
    {
      cairo_line_to(cr,gpt->source[i*2]+dxs,gpt->source[i*2+1]+dys);
    }
    cairo_line_to(cr,gpt->source[2]+dxs,gpt->source[3]+dys);
    cairo_stroke_preserve(cr);
    if ((gui->group_selected == index) && (gui->form_selected || gui->form_dragging)) cairo_set_line_width(cr, 1.0/zoom_scale);
    else                                     cairo_set_line_width(cr, 0.5/zoom_scale);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_stroke(cr);
  }
}

static int dt_circle_get_points(dt_develop_t *dev, float x, float y, float radius, float **points, int *points_count)
{
  float wd = dev->preview_pipe->iwidth;
  float ht = dev->preview_pipe->iheight;

  //how many points do we need ?
  float r = radius*MIN(wd,ht);
  int l = (int) (2.0*M_PI*r);
  
  //buffer allocations
  *points = malloc(2*(l+1)*sizeof(float));
  *points_count = l+1;  
  
  //now we set the points
  (*points)[0] = x*wd;
  (*points)[1] = y*ht;
  for (int i=1; i<l+1; i++)
  {
    float alpha = (i-1)*2.0*M_PI/(float) l;
    (*points)[i*2] = (*points)[0] + r*cosf(alpha);
    (*points)[i*2+1] = (*points)[1] + r*sinf(alpha);
  }
  
  //and we transform them with all distorted modules
  if (dt_dev_distort_transform(dev,*points,l+1)) return 1;
  
  //if we failed, then free all and return
  free(*points);
  *points = NULL;
  *points_count = 0;
  return 0;  
}

static int dt_circle_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form, int *width, int *height, int *posx, int *posy)
{  
  //we get the cicle values
  dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *) (g_list_first(form->points)->data);
  float wd = piece->pipe->iwidth, ht = piece->pipe->iheight;
  
  float r = (circle->radius + circle->border)*MIN(wd,ht);
  int l = (int) (2.0*M_PI*r);
  //buffer allocations
  float *points = malloc(2*(l+1)*sizeof(float)); 
  
  //now we set the points
  points[0] = form->source[0]*wd;
  points[1] = form->source[1]*ht;
  for (int i=1; i<l+1; i++)
  {
    float alpha = (i-1)*2.0*M_PI/(float) l;
    points[i*2] = points[0] + r*cosf(alpha);
    points[i*2+1] = points[1] + r*sinf(alpha);
  }
  
  //and we transform them with all distorted modules
  if (!dt_dev_distort_transform_plus(darktable.develop,piece->pipe,0,module->priority,points,l+1))
  {
    free(points);
    return 0;
  }
  
  //now we search min and max
  float xmin, xmax, ymin, ymax;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  for (int i=1; i < l+1; i++)
  {
    xmin = fminf(points[i*2],xmin);
    xmax = fmaxf(points[i*2],xmax);
    ymin = fminf(points[i*2+1],ymin);
    ymax = fmaxf(points[i*2+1],ymax);
  }
  free(points);
  //and we set values
  *posx = xmin;
  *posy = ymin;
  *width = (xmax-xmin);
  *height = (ymax-ymin);
  return 1;
}

static int dt_circle_get_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form, int *width, int *height, int *posx, int *posy)
{  
  //we get the cicle values
  dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *) (g_list_first(form->points)->data);
  float wd = piece->pipe->iwidth, ht = piece->pipe->iheight;
  
  float r = (circle->radius + circle->border)*MIN(wd,ht);
  int l = (int) (2.0*M_PI*r);
  //buffer allocations
  float *points = malloc(2*(l+1)*sizeof(float)); 
  
  //now we set the points
  points[0] = circle->center[0]*wd;
  points[1] = circle->center[1]*ht;
  for (int i=1; i<l+1; i++)
  {
    float alpha = (i-1)*2.0*M_PI/(float) l;
    points[i*2] = points[0] + r*cosf(alpha);
    points[i*2+1] = points[1] + r*sinf(alpha);
  }
  
  //and we transform them with all distorted modules
  if (!dt_dev_distort_transform_plus(module->dev,piece->pipe,0,module->priority,points,l+1))
  {
    free(points);
    return 0;
  }
  
  //now we search min and max
  float xmin, xmax, ymin, ymax;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  for (int i=1; i < l+1; i++)
  {
    xmin = fminf(points[i*2],xmin);
    xmax = fmaxf(points[i*2],xmax);
    ymin = fminf(points[i*2+1],ymin);
    ymax = fmaxf(points[i*2+1],ymax);
  }
  free(points);
  
  //and we set values
  *posx = xmin;
  *posy = ymin;
  *width = (xmax-xmin);
  *height = (ymax-ymin);
  return 1;
}

static int dt_circle_get_mask(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form, float **buffer, int *width, int *height, int *posx, int *posy)
{
  double start2 = dt_get_wtime();
  
  //we get the area
  if (!dt_circle_get_area(module,piece,form,width,height,posx,posy)) return 0;
  
  if (darktable.unmuted & DT_DEBUG_PERF) dt_print(DT_DEBUG_MASKS, "[masks %s] circle area took %0.04f sec\n", form->name, dt_get_wtime()-start2);
  start2 = dt_get_wtime();
  
  //we get the cicle values
  dt_masks_point_circle_t *circle = (dt_masks_point_circle_t *) (g_list_first(form->points)->data);
  
  //we create a buffer of points with all points in the area
  int w = *width, h = *height;
  float *points = malloc(w*h*2*sizeof(float));
  for (int i=0; i<h; i++)
    for (int j=0; j<w; j++)
    {
      points[(i*w+j)*2] = (j+(*posx));
      points[(i*w+j)*2+1] = (i+(*posy));
    }
  
  if (darktable.unmuted & DT_DEBUG_PERF) dt_print(DT_DEBUG_MASKS, "[masks %s] circle draw took %0.04f sec\n", form->name, dt_get_wtime()-start2);
  start2 = dt_get_wtime();
  
  //we back transform all this points
  if (!dt_dev_distort_backtransform_plus(module->dev,piece->pipe,0,module->priority,points,w*h)) return 0;
  
  if (darktable.unmuted & DT_DEBUG_PERF) dt_print(DT_DEBUG_MASKS, "[masks %s] circle transform took %0.04f sec\n", form->name, dt_get_wtime()-start2);
  start2 = dt_get_wtime();
  
  //we allocate the buffer
  *buffer = malloc(w*h*sizeof(float));
  
  //we populate the buffer
  int wi = piece->pipe->iwidth, hi=piece->pipe->iheight;
  float center[2] = {circle->center[0]*wi, circle->center[1]*hi};
  float radius2 = circle->radius*MIN(wi,hi)*circle->radius*MIN(wi,hi);
  float total2 = (circle->radius+circle->border)*MIN(wi,hi)*(circle->radius+circle->border)*MIN(wi,hi);
  for (int i=0; i<h; i++)
    for (int j=0; j<w; j++)
    {
      float x = points[(i*w+j)*2];
      float y = points[(i*w+j)*2+1];
      float l2 = (x-center[0])*(x-center[0]) + (y-center[1])*(y-center[1]);
      if (l2<radius2) (*buffer)[i*w+j] = 1.0f;
      else if (l2 < total2)
      {
        float f = (total2-l2)/(total2-radius2);
        (*buffer)[i*w+j] = f*f;
      }
      else (*buffer)[i*w+j] = 0.0f;
    }
  free(points);
  
  if (darktable.unmuted & DT_DEBUG_PERF) dt_print(DT_DEBUG_MASKS, "[masks %s] circle fill took %0.04f sec\n", form->name, dt_get_wtime()-start2);
  start2 = dt_get_wtime();
  
  return 1;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
