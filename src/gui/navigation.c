/*
		This file is part of darktable,
		copyright (c) 2009--2010 johannes hanika.

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
#include "gui/gtk.h"
#include "gui/navigation.h"
#include "develop/develop.h"
#include "control/control.h"

#include <math.h>
#define DT_NAVIGATION_INSET 5


void dt_gui_navigation_init(dt_gui_navigation_t *n, GtkWidget *widget)
{
	n->dragging = 0;
	
	GTK_WIDGET_UNSET_FLAGS (widget, GTK_DOUBLE_BUFFERED);
	GTK_WIDGET_SET_FLAGS   (widget, GTK_APP_PAINTABLE);
	g_signal_connect (G_OBJECT (widget), "expose-event",
										G_CALLBACK (dt_gui_navigation_expose), n);
	g_signal_connect (G_OBJECT (widget), "button-press-event",
										G_CALLBACK (dt_gui_navigation_button_press), n);
	g_signal_connect (G_OBJECT (widget), "button-release-event",
										G_CALLBACK (dt_gui_navigation_button_release), n);
	g_signal_connect (G_OBJECT (widget), "motion-notify-event",
										G_CALLBACK (dt_gui_navigation_motion_notify), n);
	g_signal_connect (G_OBJECT (widget), "leave-notify-event",
										G_CALLBACK (dt_gui_navigation_leave_notify), n);
}

void dt_gui_navigation_cleanup(dt_gui_navigation_t *n) {}

gboolean dt_gui_navigation_expose(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
	const int inset = DT_NAVIGATION_INSET;
	int width = widget->allocation.width, height = widget->allocation.height;

	dt_develop_t *dev = darktable.develop;
	if(dev->image && dev->preview_pipe->backbuf && !dev->preview_dirty)
	{
		cairo_surface_t *cst = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
		cairo_t *cr = cairo_create(cst);
		cairo_set_source_rgb(cr, darktable.gui->bgcolor[0], darktable.gui->bgcolor[1], darktable.gui->bgcolor[2]);
		cairo_paint(cr);

		width -= 2*inset; height -= 2*inset;
		cairo_translate(cr, inset, inset);

		pthread_mutex_t *mutex = &dev->preview_pipe->backbuf_mutex;
		pthread_mutex_lock(mutex);
		const int wd = dev->preview_pipe->backbuf_width;
		const int ht = dev->preview_pipe->backbuf_height;
		const float scale = fminf(width/(float)wd, height/(float)ht);

		const int stride = cairo_format_stride_for_width (CAIRO_FORMAT_RGB24, wd);
		cairo_surface_t *surface = cairo_image_surface_create_for_data (dev->preview_pipe->backbuf, CAIRO_FORMAT_RGB24, wd, ht, stride); 
		cairo_translate(cr, width/2.0, height/2.0f);
		cairo_scale(cr, scale, scale);
		cairo_translate(cr, -.5f*wd, -.5f*ht);

		// draw shadow around
		float alpha = 1.0f;
		for(int k=0;k<4;k++)
		{
			cairo_rectangle(cr, -k/scale, -k/scale, wd + 2*k/scale, ht + 2*k/scale);
			cairo_set_source_rgba(cr, 0, 0, 0, alpha);
			alpha *= 0.6f;
			cairo_fill(cr);
		}

		cairo_rectangle(cr, 0, 0, wd, ht);
		cairo_set_source_surface (cr, surface, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_FAST);
		cairo_fill(cr);
		cairo_surface_destroy (surface);

		pthread_mutex_unlock(mutex);

		// draw box where we are
		dt_dev_zoom_t zoom;
		int closeup;
		float zoom_x, zoom_y;
		DT_CTL_GET_GLOBAL(zoom, dev_zoom);
		DT_CTL_GET_GLOBAL(closeup, dev_closeup);
		DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
		DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
		const float min_scale = dt_dev_get_zoom_scale(dev, DT_ZOOM_FIT, closeup ? 2.0 : 1.0, 0);
		const float cur_scale = dt_dev_get_zoom_scale(dev, zoom,        closeup ? 2.0 : 1.0, 0);
		// avoid numerical instability for small resolutions:
		if(cur_scale > min_scale+0.001)
		{
			float boxw = 1, boxh = 1;
			dt_dev_check_zoom_bounds(darktable.develop, &zoom_x, &zoom_y, zoom, closeup, &boxw, &boxh);

			cairo_translate(cr, wd*(.5f+zoom_x), ht*(.5f+zoom_y));
			cairo_set_source_rgb(cr, 0., 0., 0.);
			cairo_set_line_width(cr, 1.f/scale);
			boxw *= wd;
			boxh *= ht;
			cairo_rectangle(cr, -boxw/2-1, -boxh/2-1, boxw+2, boxh+2);
			cairo_stroke(cr);
			cairo_set_source_rgb(cr, 1., 1., 1.);
			cairo_rectangle(cr, -boxw/2, -boxh/2, boxw, boxh);
			cairo_stroke(cr);
		}

		cairo_destroy(cr);
		cairo_t *cr_pixmap = gdk_cairo_create(gtk_widget_get_window(widget));
		cairo_set_source_surface (cr_pixmap, cst, 0, 0);
		cairo_paint(cr_pixmap);
		cairo_destroy(cr_pixmap);
		cairo_surface_destroy(cst);
	}
	return TRUE;
}

void dt_gui_navigation_set_position(dt_gui_navigation_t *n, double x, double y, int wd, int ht)
{
	dt_dev_zoom_t zoom;
	int closeup;
	float zoom_x, zoom_y;
	DT_CTL_GET_GLOBAL(zoom, dev_zoom);
	DT_CTL_GET_GLOBAL(closeup, dev_closeup);
	DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
	DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);

	if(n->dragging && zoom != DT_ZOOM_FIT)
	{
		const int inset = DT_NAVIGATION_INSET;
		const float width = wd - 2*inset, height = ht - 2*inset;
		const dt_develop_t *dev = darktable.develop;
		int iwd, iht;
		dt_dev_get_processed_size(dev, &iwd, &iht);
		zoom_x = fmaxf(-.5, fminf(((x-inset)/width  - .5f)/(iwd*fminf(wd/(float)iwd, ht/(float)iht)/(float)wd), .5));
		zoom_y = fmaxf(-.5, fminf(((y-inset)/height - .5f)/(iht*fminf(wd/(float)iwd, ht/(float)iht)/(float)ht), .5));
		dt_dev_check_zoom_bounds(darktable.develop, &zoom_x, &zoom_y, zoom, closeup, NULL, NULL);
		DT_CTL_SET_GLOBAL(dev_zoom_x, zoom_x);
		DT_CTL_SET_GLOBAL(dev_zoom_y, zoom_y);

		dt_dev_invalidate(darktable.develop);
		dt_control_gui_queue_draw();
	}
}

gboolean dt_gui_navigation_motion_notify(GtkWidget *widget, GdkEventMotion *event, gpointer user_data)
{
	dt_gui_navigation_t *n = (dt_gui_navigation_t *)user_data;
	dt_gui_navigation_set_position(n, event->x, event->y, widget->allocation.width, widget->allocation.height);
	gint x, y; // notify gtk for motion_hint.
	gdk_window_get_pointer(event->window, &x, &y, NULL);
	return TRUE;
}
gboolean dt_gui_navigation_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	dt_gui_navigation_t *n = (dt_gui_navigation_t *)user_data;
	n->dragging = 1;
	dt_gui_navigation_set_position(n, event->x, event->y, widget->allocation.width, widget->allocation.height);
	return TRUE;
}
gboolean dt_gui_navigation_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
	dt_gui_navigation_t *n = (dt_gui_navigation_t *)user_data;
	n->dragging = 0;
	return TRUE;
}
gboolean dt_gui_navigation_leave_notify(GtkWidget *widget, GdkEventCrossing *event, gpointer user_data)
{
	return TRUE;
}
void dt_gui_navigation_get_pos(dt_gui_navigation_t *n, float *x, float *y)
{
}
