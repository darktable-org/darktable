
/*
		This file is part of darktable,
		copyright (c) 2010-2011 henrik andersson.

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

#include "control/control.h"
#include "common/darktable.h"
#include "views/view.h"
#include "libs/lib.h"

DT_MODULE(1)


typedef struct dt_map_t
{

}
dt_map_t;

const char *name(dt_view_t *self)
{
  return _("map");
}

uint32_t view(dt_view_t *self)
{
  return DT_VIEW_MAP;
}

void init(dt_view_t *self)
{
  self->data = malloc(sizeof(dt_map_t));
  memset(self->data,0,sizeof(dt_map_t));
  //  dt_map_t *lib = (dt_map_t *)self->data;

}

void cleanup(dt_view_t *self)
{
  free(self->data);
}


void configure(dt_view_t *self, int wd, int ht)
{
  //dt_capture_t *lib=(dt_capture_t*)self->data;
}

void expose(dt_view_t *self, cairo_t *cri, int32_t width_i, int32_t height_i, int32_t pointerx, int32_t pointery)
{
  //  dt_map_t *lib = (dt_map_t *)self->data;

  const int32_t capwd = darktable.thumbnail_width;
  const int32_t capht = darktable.thumbnail_height;
  int32_t width  = MIN(width_i,  capwd);
  int32_t height = MIN(height_i, capht);

  cairo_set_source_rgb (cri, .2, .2, .2);
  cairo_rectangle(cri, 0, 0, width_i, height_i);
  cairo_fill (cri);


  if(width_i  > capwd) cairo_translate(cri, -(capwd-width_i) *.5f, 0.0f);
  if(height_i > capht) cairo_translate(cri, 0.0f, -(capht-height_i)*.5f);

  // Mode dependent expose of center view
  cairo_save(cri);

  
  cairo_restore(cri);

  GList *modules = darktable.lib->plugins;
  while(modules)
  {
    dt_lib_module_t *module = (dt_lib_module_t *)(modules->data);
    if( (module->views() & self->view(self)) && module->gui_post_expose )
      module->gui_post_expose(module,cri,width,height,pointerx,pointery);
    modules = g_list_next(modules);
  }

}

int try_enter(dt_view_t *self)
{
  return 0;
}


void enter(dt_view_t *self)
{
  //  dt_map_t *lib = (dt_map_t *)self->data;

}

void leave(dt_view_t *self)
{
  //  dt_map_t *cv = (dt_map_t *)self->data;
}

void mouse_moved(dt_view_t *self, double x, double y, int which)
{
  // redraw center on mousemove
  dt_control_queue_redraw_center();
}

void init_key_accels(dt_view_t *self)
{
#if 0
  // Setup key accelerators in capture view...
  dt_accel_register_view(self, NC_("accel", "toggle film strip"),
                         GDK_f, GDK_CONTROL_MASK);
#endif
}

void connect_key_accels(dt_view_t *self)
{
#if 0
  GClosure *closure = g_cclosure_new(G_CALLBACK(film_strip_key_accel),
                                     (gpointer)self, NULL);
  dt_accel_connect_view(self, "toggle film strip", closure);
#endif
}
