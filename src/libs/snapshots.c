/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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

#include "common/darktable.h"
#include "common/debug.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/develop.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

#define DT_LIB_SNAPSHOTS_COUNT 4

#define HANDLE_SIZE 0.02

/* a snapshot */
typedef struct dt_lib_snapshot_t
{
  GtkWidget *button;
  float zoom_x, zoom_y, zoom_scale;
  int32_t zoom, closeup;
  char filename[512];
} dt_lib_snapshot_t;


typedef struct dt_lib_snapshots_t
{
  GtkWidget *snapshots_box;

  uint32_t selected;

  /* current active snapshots */
  uint32_t num_snapshots;

  /* size of snapshots */
  uint32_t size;

  /* snapshots */
  dt_lib_snapshot_t *snapshot;

  /* snapshot cairo surface */
  cairo_surface_t *snapshot_image;


  /* change snapshot overlay controls */
  gboolean dragging, vertical, inverted;
  double vp_width, vp_height, vp_xpointer, vp_ypointer;

  GtkWidget *take_button;

} dt_lib_snapshots_t;

/* callback for take snapshot */
static void _lib_snapshots_add_button_clicked_callback(GtkWidget *widget, gpointer user_data);
static void _lib_snapshots_toggled_callback(GtkToggleButton *widget, gpointer user_data);


const char *name(dt_lib_module_t *self)
{
  return _("snapshots");
}

const char **views(dt_lib_module_t *self)
{
  static const char *v[] = {"darkroom", NULL};
  return v;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int position()
{
  return 1000;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "take snapshot"), 0, 0);
}

void connect_key_accels(dt_lib_module_t *self)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;

  dt_accel_connect_button_lib(self, "take snapshot", d->take_button);
}

/* expose snapshot over center viewport */
void gui_post_expose(dt_lib_module_t *self, cairo_t *cri, int32_t width, int32_t height, int32_t pointerx,
                     int32_t pointery)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;

  if(d->snapshot_image)
  {
    d->vp_width = width;
    d->vp_height = height;

    /* set x,y,w,h of surface depending on split align and invert */
    double x = d->vertical ? (d->inverted ? width * d->vp_xpointer : 0) : 0;
    double y = d->vertical ? 0 : (d->inverted ? height * d->vp_ypointer : 0);
    double w = d->vertical ? (d->inverted ? (width * (1.0 - d->vp_xpointer)) : width * d->vp_xpointer)
                           : width;
    double h = d->vertical ? height
                           : (d->inverted ? (height * (1.0 - d->vp_ypointer)) : height * d->vp_ypointer);

    cairo_set_source_surface(cri, d->snapshot_image, 0, 0);
    // cairo_rectangle(cri, 0, 0, width*d->vp_xpointer, height);
    cairo_rectangle(cri, x, y, w, h);
    cairo_fill(cri);

    /* draw the split line */
    cairo_set_source_rgb(cri, .7, .7, .7);
    cairo_set_line_width(cri, 1.);

    if(d->vertical)
    {
      cairo_move_to(cri, width * d->vp_xpointer, 0.0f);
      cairo_line_to(cri, width * d->vp_xpointer, height);
    }
    else
    {
      cairo_move_to(cri, 0.0f, height * d->vp_ypointer);
      cairo_line_to(cri, width, height * d->vp_ypointer);
    }
    cairo_stroke(cri);

    /* if mouse over control lets draw center rotate control, hide if split is dragged */
    if(!d->dragging)
    {
      cairo_set_line_width(cri, 0.5);
      double s = width * HANDLE_SIZE;
      dtgtk_cairo_paint_refresh(cri, (d->vertical ? width * d->vp_xpointer : width * 0.5) - (s * 0.5),
                                (d->vertical ? height * 0.5 : height * d->vp_ypointer) - (s * 0.5), s, s, 0, NULL);
    }
  }
}

int button_released(struct dt_lib_module_t *self, double x, double y, int which, uint32_t state)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  if(d->snapshot_image)
  {
    d->dragging = FALSE;
    return 1;
  }
  return 0;
}

static int _lib_snapshot_rotation_cnt = 0;

int button_pressed(struct dt_lib_module_t *self, double x, double y, double pressure, int which, int type,
                   uint32_t state)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;

  if(d->snapshot_image)
  {
    double xp = x / d->vp_width;
    double yp = y / d->vp_height;

    /* do the split rotating */
    double hhs = HANDLE_SIZE * 0.5;
    if(which == 1
       && (((d->vertical && xp > d->vp_xpointer - hhs && xp < d->vp_xpointer + hhs) && yp > 0.5 - hhs
            && yp < 0.5 + hhs)
           || ((yp > d->vp_ypointer - hhs && yp < d->vp_ypointer + hhs) && xp > 0.5 - hhs && xp < 0.5 + hhs)))
    {
      /* let's rotate */
      _lib_snapshot_rotation_cnt++;

      d->vertical = !d->vertical;
      if(_lib_snapshot_rotation_cnt % 2) d->inverted = !d->inverted;

      d->vp_xpointer = xp;
      d->vp_ypointer = yp;
      dt_control_queue_redraw_center();
    }
    /* do the dragging !? */
    else if(which == 1)
    {
      d->dragging = TRUE;
      d->vp_ypointer = yp;
      d->vp_xpointer = xp;
      dt_control_queue_redraw_center();
    }
    return 1;
  }
  return 0;
}

int mouse_moved(dt_lib_module_t *self, double x, double y, double pressure, int which)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;

  if(d->snapshot_image)
  {
    double xp = x / d->vp_width;
    double yp = y / d->vp_height;

    /* update x pointer */
    if(d->dragging)
    {
      d->vp_xpointer = xp;
      d->vp_ypointer = yp;
    }
    dt_control_queue_redraw_center();
    return 1;
  }

  return 0;
}

void gui_reset(dt_lib_module_t *self)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  d->num_snapshots = 0;
  d->snapshot_image = NULL;

  for(uint32_t k = 0; k < d->size; k++)
  {
    gtk_widget_hide(d->snapshot[k].button);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->snapshot[k].button), FALSE);
  }

  dt_control_queue_redraw_center();
}

void gui_init(dt_lib_module_t *self)
{
  /* initialize ui widgets */
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)g_malloc0(sizeof(dt_lib_snapshots_t));
  self->data = (void *)d;

  /* initialize snapshot storages */
  d->size = 4;
  d->snapshot = (dt_lib_snapshot_t *)g_malloc0_n(d->size, sizeof(dt_lib_snapshot_t));
  d->vp_xpointer = 0.5;
  d->vp_ypointer = 0.5;
  d->vertical = TRUE;

  /* initialize ui containers */
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  dt_gui_add_help_link(self->widget, "snapshots.html#snapshots");
  d->snapshots_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /* create take snapshot button */
  GtkWidget *button = gtk_button_new_with_label(_("take snapshot"));
  d->take_button = button;
  g_signal_connect(G_OBJECT(button), "clicked", G_CALLBACK(_lib_snapshots_add_button_clicked_callback), self);
  gtk_widget_set_tooltip_text(button, _("take snapshot to compare with another image "
                                        "or the same image at another stage of development"));
  dt_gui_add_help_link(button, "snapshots.html#snapshots");

  /*
   * initialize snapshots
   */
  char wdname[32] = { 0 };
  char localtmpdir[PATH_MAX] = { 0 };
  dt_loc_get_tmp_dir(localtmpdir, sizeof(localtmpdir));

  for(int k = 0; k < d->size; k++)
  {
    /* create snapshot button */
    d->snapshot[k].button = gtk_toggle_button_new_with_label(wdname);
    gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(d->snapshot[k].button)), GTK_ALIGN_START);
    g_signal_connect(G_OBJECT(d->snapshot[k].button), "clicked", G_CALLBACK(_lib_snapshots_toggled_callback),
                     self);

    /* assign snapshot number to widget */
    g_object_set_data(G_OBJECT(d->snapshot[k].button), "snapshot", GINT_TO_POINTER(k + 1));

    /* setup filename for snapshot */
    snprintf(d->snapshot[k].filename, sizeof(d->snapshot[k].filename), "%s/dt_snapshot_%d.png", localtmpdir,
             k);

    /* add button to snapshot box */
    gtk_box_pack_start(GTK_BOX(d->snapshots_box), d->snapshot[k].button, TRUE, TRUE, 0);

    /* prevent widget to show on external show all */
    gtk_widget_set_no_show_all(d->snapshot[k].button, TRUE);
  }

  /* add snapshot box and take snapshot button to widget ui*/
  gtk_box_pack_start(GTK_BOX(self->widget), d->snapshots_box, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), button, TRUE, TRUE, 0);
}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;

  g_free(d->snapshot);

  g_free(self->data);
  self->data = NULL;
}

static void _lib_snapshots_add_button_clicked_callback(GtkWidget *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;

  /* backup last snapshot slot */
  dt_lib_snapshot_t last = d->snapshot[d->size - 1];

  /* rotate slots down to make room for new one on top */
  for(int k = d->size - 1; k > 0; k--)
  {
    GtkWidget *b = d->snapshot[k].button;
    d->snapshot[k] = d->snapshot[k - 1];
    d->snapshot[k].button = b;
    gtk_button_set_label(GTK_BUTTON(d->snapshot[k].button),
                         gtk_button_get_label(GTK_BUTTON(d->snapshot[k - 1].button)));
    gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(d->snapshot[k].button)), GTK_ALIGN_START);
  }

  /* update top slot with new snapshot */
  char label[64];
  GtkWidget *b = d->snapshot[0].button;
  d->snapshot[0] = last;
  d->snapshot[0].button = b;
  const gchar *name = _("original");
  if(darktable.develop->history_end > 0)
  {
    dt_dev_history_item_t *history_item = g_list_nth_data(darktable.develop->history,
                                                          darktable.develop->history_end - 1);
    if(history_item && history_item->module)
      name = history_item->module->name();
    else
      name = _("unknown");
  }
  g_snprintf(label, sizeof(label), "%s (%d)", name, darktable.develop->history_end);
  gtk_button_set_label(GTK_BUTTON(d->snapshot[0].button), label);
  gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(d->snapshot[0].button)), GTK_ALIGN_START);

  dt_lib_snapshot_t *s = d->snapshot + 0;
  s->zoom_y = dt_control_get_dev_zoom_y();
  s->zoom_x = dt_control_get_dev_zoom_x();
  s->zoom = dt_control_get_dev_zoom();
  s->closeup = dt_control_get_dev_closeup();
  s->zoom_scale = dt_control_get_dev_zoom_scale();

  /* update slots used */
  if(d->num_snapshots != d->size) d->num_snapshots++;

  /* show active snapshot slots */
  for(uint32_t k = 0; k < d->num_snapshots; k++) gtk_widget_show(d->snapshot[k].button);

  /* request a new snapshot for top slot */
  dt_dev_snapshot_request(darktable.develop, (const char *)&d->snapshot[0].filename);
}

static void _lib_snapshots_toggled_callback(GtkToggleButton *widget, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  /* get current snapshot index */
  int which = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "snapshot"));

  /* free current snapshot image if exists */
  if(d->snapshot_image)
  {
    cairo_surface_destroy(d->snapshot_image);
    d->snapshot_image = NULL;
  }

  /* check if snapshot is activated */
  if(gtk_toggle_button_get_active(widget))
  {
    /* lets inactivate all togglebuttons except for self */
    for(uint32_t k = 0; k < d->size; k++)
      if(GTK_WIDGET(widget) != d->snapshot[k].button)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(d->snapshot[k].button), FALSE);

    /* setup snapshot */
    d->selected = which;
    dt_lib_snapshot_t *s = d->snapshot + (which - 1);
    dt_control_set_dev_zoom_y(s->zoom_y);
    dt_control_set_dev_zoom_x(s->zoom_x);
    dt_control_set_dev_zoom(s->zoom);
    dt_control_set_dev_closeup(s->closeup);
    dt_control_set_dev_zoom_scale(s->zoom_scale);

    dt_dev_invalidate(darktable.develop);

    d->snapshot_image = dt_cairo_image_surface_create_from_png(s->filename);
  }

  /* redraw center view */
  dt_control_queue_redraw_center();
}

#ifdef USE_LUA
typedef enum
{
  SNS_LEFT,
  SNS_RIGHT,
  SNS_TOP,
  SNS_BOTTOM,
} snapshot_direction_t;

static int direction_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  if(lua_gettop(L) != 3)
  {
    snapshot_direction_t result;
    if(!d->vertical && !d->inverted)
    {
      result = SNS_TOP;
    }
    else if(!d->vertical && d->inverted)
    {
      result = SNS_BOTTOM;
    }
    else if(d->vertical && !d->inverted)
    {
      result = SNS_LEFT;
    }
    else
    {
      result = SNS_RIGHT;
    }
    luaA_push(L, snapshot_direction_t, &result);
    return 1;
  }
  else
  {
    snapshot_direction_t direction;
    luaA_to(L, snapshot_direction_t, &direction, 3);
    if(direction == SNS_TOP)
    {
      d->vertical = FALSE;
      d->inverted = FALSE;
    }
    else if(direction == SNS_BOTTOM)
    {
      d->vertical = FALSE;
      d->inverted = TRUE;
    }
    else if(direction == SNS_LEFT)
    {
      d->vertical = TRUE;
      d->inverted = FALSE;
    }
    else
    {
      d->vertical = TRUE;
      d->inverted = TRUE;
    }
    return 0;
  }
}

static int ratio_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  if(lua_gettop(L) != 3)
  {
    if(!d->vertical && !d->inverted)
    {
      lua_pushnumber(L, d->vp_ypointer);
    }
    else if(!d->vertical && d->inverted)
    {
      lua_pushnumber(L, 1 - d->vp_ypointer);
    }
    else if(d->vertical && !d->inverted)
    {
      lua_pushnumber(L, d->vp_xpointer);
    }
    else
    {
      lua_pushnumber(L, 1 - d->vp_xpointer);
    }
    return 1;
  }
  else
  {
    double ratio;
    luaA_to(L, double, &ratio, 3);
    if(ratio < 0.0) ratio = 0.0;
    if(ratio > 1.0) ratio = 1.0;
    if(!d->vertical && !d->inverted)
    {
      d->vp_ypointer = ratio;
    }
    else if(!d->vertical && d->inverted)
    {
      d->vp_ypointer = 1.0 - ratio;
    }
    else if(d->vertical && !d->inverted)
    {
      d->vp_xpointer = ratio;
    }
    else
    {
      d->vp_xpointer = 1.0 - ratio;
    }
    return 0;
  }
}

static int max_snapshot_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  lua_pushinteger(L, d->size);
  return 1;
}

static int lua_take_snapshot(lua_State *L)
{
  dt_lib_module_t *self = lua_touserdata(L, lua_upvalueindex(1));
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  _lib_snapshots_add_button_clicked_callback(d->take_button, self);
  return 0;
}

typedef int dt_lua_snapshot_t;
static int selected_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  for(int i = 0; i < d->num_snapshots; i++)
  {
    GtkWidget *widget = d->snapshot[i].button;
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)))
    {
      luaA_push(L, dt_lua_snapshot_t, &i);
      return 1;
    }
  }
  lua_pushnil(L);
  return 1;
}

static int snapshots_length(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  lua_pushinteger(L, d->num_snapshots);
  return 1;
}

static int number_member(lua_State *L)
{
  dt_lib_module_t *self = *(dt_lib_module_t **)lua_touserdata(L, 1);
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)self->data;
  int index = luaL_checkinteger(L, 2);
  if( index < 1)
  {
    return luaL_error(L, "Accessing a non-existant snapshot");
  }else if(index > d->num_snapshots ) {
    lua_pushnil(L);
    return 1;
  }
  index = index - 1;
  luaA_push(L, dt_lua_snapshot_t, &index);
  return 1;
}


static int filename_member(lua_State *L)
{
  dt_lua_snapshot_t index;
  luaA_to(L, dt_lua_snapshot_t, &index, 1);
  dt_lib_module_t *module = lua_touserdata(L, lua_upvalueindex(1));
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)module->data;
  if(index >= d->num_snapshots || index < 0)
  {
    return luaL_error(L, "Accessing a non-existant snapshot");
  }
  lua_pushstring(L, d->snapshot[index].filename);
  return 1;
}
static int name_member(lua_State *L)
{
  dt_lua_snapshot_t index;
  luaA_to(L, dt_lua_snapshot_t, &index, 1);
  dt_lib_module_t *module = lua_touserdata(L, lua_upvalueindex(1));
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)module->data;
  if(index >= d->num_snapshots || index < 0)
  {
    return luaL_error(L, "Accessing a non-existant snapshot");
  }
  lua_pushstring(L, gtk_button_get_label(GTK_BUTTON(d->snapshot[index].button)));
  return 1;
}

static int lua_select(lua_State *L)
{
  dt_lua_snapshot_t index;
  luaA_to(L, dt_lua_snapshot_t, &index, 1);
  dt_lib_module_t *module = lua_touserdata(L, lua_upvalueindex(1));
  dt_lib_snapshots_t *d = (dt_lib_snapshots_t *)module->data;
  if(index >= d->num_snapshots || index < 0)
  {
    return luaL_error(L, "Accessing a non-existant snapshot");
  }
  dt_lib_snapshot_t *self = &d->snapshot[index];
  gtk_button_clicked(GTK_BUTTON(self->button));
  return 0;
}

// selected : boolean r/w

void init(struct dt_lib_module_t *self)
{
  lua_State *L = darktable.lua_state.state;
  int my_type = dt_lua_module_entry_get_type(L, "lib", self->plugin_name);
  lua_pushcfunction(L, direction_member);
  dt_lua_type_register_type(L, my_type, "direction");
  lua_pushcfunction(L, ratio_member);
  dt_lua_type_register_type(L, my_type, "ratio");
  lua_pushcfunction(L, max_snapshot_member);
  dt_lua_type_register_const_type(L, my_type, "max_snapshot");
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, lua_take_snapshot, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const_type(L, my_type, "take_snapshot");
  lua_pushcfunction(L, snapshots_length);
  lua_pushcfunction(L, number_member);
  dt_lua_type_register_number_const_type(L, my_type);
  lua_pushcfunction(L, selected_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register_const_type(L, my_type, "selected");

  dt_lua_init_int_type(L, dt_lua_snapshot_t);
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, filename_member, 1);
  dt_lua_type_register_const(L, dt_lua_snapshot_t, "filename");
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, name_member, 1);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register_const(L, dt_lua_snapshot_t, "name");
  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, lua_select, 1);
  dt_lua_gtk_wrap(L);
  lua_pushcclosure(L, dt_lua_type_member_common, 1);
  dt_lua_type_register_const(L, dt_lua_snapshot_t, "select");

  lua_pushlightuserdata(L, self);
  lua_pushcclosure(L, name_member, 1);
  dt_lua_gtk_wrap(L);
  dt_lua_type_setmetafield(L,dt_lua_snapshot_t,"__tostring");



  luaA_enum(L, snapshot_direction_t);
  luaA_enum_value_name(L, snapshot_direction_t, SNS_LEFT, "left");
  luaA_enum_value_name(L, snapshot_direction_t, SNS_RIGHT, "right");
  luaA_enum_value_name(L, snapshot_direction_t, SNS_TOP, "top");
  luaA_enum_value_name(L, snapshot_direction_t, SNS_BOTTOM, "bottom");
}
#endif // USE_LUA

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
