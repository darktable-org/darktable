/*
   This file is part of darktable,
   Copyright (C) 2015-2026 darktable developers.

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
#include "lua/types.h"
#include "lua/widget/common.h"

/*
  we can't guarantee the order of label and ellipsize|halign calls so
  sometimes we have to store the ellipsize|halign mode until the
  label is created.
*/
typedef struct dt_lua_pending_property_t
{
  gboolean used;
  int value;
} dt_lua_pending_property_t;

static dt_lua_pending_property_t ellipsize_store = { .used = FALSE };
static dt_lua_pending_property_t halign_store = { .used = FALSE };

/*
  we can't guarantee the order of image and position_type calls so
  sometimes we have to store the position_type mode until the
  label is created.
*/
struct dt_lua_position_type_info
{
  gboolean used;
  dt_lua_position_type_t position;
};

static struct dt_lua_position_type_info position_type_store =
{
  .used = FALSE
};

static dt_lua_widget_type_t button_type =
{
  .name = "button",
  .gui_init = NULL,
  .gui_cleanup = NULL,
  .alloc_size = sizeof(dt_lua_widget_t),
  .parent= &widget_type
};

static void clicked_callback(GtkButton *widget, gpointer user_data)
{
  dt_lua_async_call_alien(dt_lua_widget_trigger_callback,
      0, NULL, NULL,
      LUA_ASYNC_TYPENAME, "lua_widget", user_data,
      LUA_ASYNC_TYPENAME, "const char*", "clicked",
      LUA_ASYNC_DONE);
}

/* Find the first child of `box` that is an instance of `type` (e.g.
   GTK_TYPE_LABEL or GTK_TYPE_IMAGE), or NULL if none is found. */
static GtkWidget *dt_lua_button_get_widget_in_box(GtkWidget *box, GType type)
{
  GList *children = gtk_container_get_children(GTK_CONTAINER(box));
  GtkWidget *found = NULL;
  for(GList *l = children; l; l = l->next)
  {
    if(G_TYPE_CHECK_INSTANCE_TYPE(l->data, type))
    {
      found = GTK_WIDGET(l->data);
      break;
    }
  }
  g_list_free(children);
  return found;
}

/* Return the index of `widget` among `box`'s children, or -1 if
   `widget` is not a child of `box`. */
static int dt_lua_button_get_child_index(GtkWidget *box, GtkWidget *widget)
{
  GList *children = gtk_container_get_children(GTK_CONTAINER(box));
  int idx = -1;
  int i = 0;
  for(GList *l = children; l; l = l->next, i++)
  {
    if(l->data == (gpointer)widget)
    {
      idx = i;
      break;
    }
  }
  g_list_free(children);
  return idx;
}

/* Return the button's current label widget, or NULL if it doesn't
   have one yet (e.g. right after creation, before label/image is
   set). Used by ellipsize_member and halign_member, which both need
   to find the label before they can get/set a property on it. */
static GtkWidget *dt_lua_button_get_current_label(lua_button button)
{
  GtkWidget *child = gtk_bin_get_child(GTK_BIN(button->widget));
  if(GTK_IS_BOX(child))
    return dt_lua_button_get_widget_in_box(child, GTK_TYPE_LABEL);
  if(gtk_button_get_label(GTK_BUTTON(button->widget)))
    return child;
  return NULL;
}

static int ellipsize_member(lua_State *L)
{
  lua_button button;
  luaA_to(L, lua_button, &button, 1);
  GtkWidget *label = dt_lua_button_get_current_label(button);

  if(lua_gettop(L) > 2)
  {
    dt_lua_ellipsize_mode_t ellipsize;
    luaA_to(L, dt_lua_ellipsize_mode_t, &ellipsize, 3);
    if(label)
      gtk_label_set_ellipsize(GTK_LABEL(label), ellipsize);
    else
    {
      ellipsize_store.value = ellipsize;
      ellipsize_store.used = TRUE;
    }
    return 0;
  }

  dt_lua_ellipsize_mode_t ellipsize = label
    ? gtk_label_get_ellipsize(GTK_LABEL(label))
    : PANGO_ELLIPSIZE_NONE;
  luaA_push(L, dt_lua_ellipsize_mode_t, &ellipsize);
  return 1;
}

static int halign_member(lua_State *L)
{
  lua_button button;
  luaA_to(L, lua_button, &button, 1);
  GtkWidget *label = dt_lua_button_get_current_label(button);

  if(lua_gettop(L) > 2)
  {
    dt_lua_align_t halign;
    luaA_to(L, dt_lua_align_t, &halign, 3);
    if(label)
      gtk_widget_set_halign(GTK_WIDGET(label), halign);
    else
    {
      halign_store.value = halign;
      halign_store.used = TRUE;
    }
    return 0;
  }

  dt_lua_align_t halign = label
    ? gtk_widget_get_halign(GTK_WIDGET(label))
    : GTK_ALIGN_FILL;
  luaA_push(L, dt_lua_align_t, &halign);
  return 1;
}

static int label_member(lua_State *L)
{
  lua_button button;
  luaA_to(L, lua_button, &button, 1);
  if(lua_gettop(L) > 2)
  {
    const char * label = luaL_checkstring(L, 3);
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(button->widget));
    gboolean had_box = child && GTK_IS_BOX(child);
    if(had_box) g_object_ref(G_OBJECT(child));

    gtk_button_set_label(GTK_BUTTON(button->widget), label);

    if(had_box)
    {
      GtkWidget *new_child = gtk_bin_get_child(GTK_BIN(button->widget));
      if(new_child != child)
      {
        gtk_container_remove(GTK_CONTAINER(button->widget), new_child);
        gtk_container_add(GTK_CONTAINER(button->widget), child);
      }
      g_object_unref(G_OBJECT(child));
    }
    child = gtk_bin_get_child(GTK_BIN(button->widget));
    if(GTK_IS_BOX(child))
    {
      GtkWidget *label_widget = dt_lua_button_get_widget_in_box(child, GTK_TYPE_LABEL);
      if(label_widget)
      {
        gtk_label_set_text(GTK_LABEL(label_widget), label);
      }
      else
      {
        label_widget = gtk_label_new(label);
        GtkWidget *image_widget = dt_lua_button_get_widget_in_box(child, GTK_TYPE_IMAGE);
        gtk_box_pack_start(GTK_BOX(child), label_widget, FALSE, FALSE, 0);
        if(image_widget && dt_lua_button_get_child_index(child, image_widget) == 1)
          gtk_box_reorder_child(GTK_BOX(child), label_widget, 0);
        gtk_widget_show(label_widget);
      }

      gtk_label_set_ellipsize(GTK_LABEL(label_widget),
                              ellipsize_store.used ? (PangoEllipsizeMode)ellipsize_store.value : PANGO_ELLIPSIZE_END);
      ellipsize_store.used = FALSE;
      if(halign_store.used)
      {
        gtk_widget_set_halign(GTK_WIDGET(label_widget), (GtkAlign)halign_store.value);
        halign_store.used = FALSE;
      }
    }
    else
    {
      gtk_label_set_ellipsize(GTK_LABEL(child),
                              ellipsize_store.used ? (PangoEllipsizeMode)ellipsize_store.value : PANGO_ELLIPSIZE_END);
      ellipsize_store.used = FALSE;
      if(halign_store.used)
      {
        gtk_widget_set_halign(GTK_WIDGET(GTK_LABEL(child)), (GtkAlign)halign_store.value);
        halign_store.used = FALSE;
      }
    }
    return 0;
  }
  lua_pushstring(L, gtk_button_get_label(GTK_BUTTON(button->widget)));
  return 1;
}

// gtk_image_new_from_file() ignores the display scale; decode at device
// resolution and size the result against the line height instead
static GtkWidget *_image_new_from_file(GtkWidget *widget, const char *filename)
{
  int native_w = 0, native_h = 0;
  GdkPixbufFormat *format = gdk_pixbuf_get_file_info(filename, &native_w, &native_h);
  if(!format || native_w <= 0 || native_h <= 0)
    return gtk_image_new_from_file(filename);

  // a single pixbuf would freeze an animation on its first frame, so let gtk
  // keep those. only gtk can tell us it built one, hence the throwaway decode
  gchar *format_name = gdk_pixbuf_format_get_name(format);
  const gboolean may_animate = !g_strcmp0(format_name, "gif")
                               || !g_strcmp0(format_name, "webp");
  g_free(format_name);
  if(may_animate)
  {
    GtkWidget *animation = gtk_image_new_from_file(filename);
    if(gtk_image_get_storage_type(GTK_IMAGE(animation)) == GTK_IMAGE_ANIMATION)
      return animation;
    g_object_ref_sink(animation);
    g_object_unref(animation);
  }

  PangoLayout *layout = gtk_widget_create_pango_layout(widget, "X");
  int line_w = 0, line_h = 0;
  pango_layout_get_pixel_size(layout, &line_w, &line_h);
  g_object_unref(layout);

  // ppd, not the widget's scale factor: gtk reports that unreliably on osx
  // (gtk.c:2282) and an unparented button answers for monitor 0
  const double ppd = darktable.gui ? darktable.gui->ppd : 1.0;

  // an svg renders at any size; a raster has no detail beyond its own
  const gboolean scalable = gdk_pixbuf_format_is_scalable(format);
  const int wanted_h = line_h > 0 ? line_h : native_h;
  const int logical_h = scalable ? wanted_h : MIN(native_h, wanted_h);
  const int device_h = scalable ? (int)round(logical_h * ppd)
                                : MIN(native_h, (int)round(logical_h * ppd));

  GError *error = NULL;
  GdkPixbuf *pixbuf =
    gdk_pixbuf_new_from_file_at_scale(filename, -1, device_h, TRUE, &error);
  if(!pixbuf)
  {
    g_clear_error(&error);
    return gtk_image_new_from_file(filename);
  }

  // gtk lays a surface out as its pixels over its device scale, so derive that
  // scale from what was decoded: a raster may hold fewer pixels than ppd asked
  cairo_surface_t *surface =
    gdk_cairo_surface_create_from_pixbuf(pixbuf, 1, gtk_widget_get_window(widget));
  // one ulp up: cairo inverts the scale rather than dividing, so ceil() would
  // otherwise gain a pixel
  const double device_scale =
    nextafter((double)gdk_pixbuf_get_height(pixbuf) / logical_h, INFINITY);
  cairo_surface_set_device_scale(surface, device_scale, device_scale);

  GtkWidget *image = gtk_image_new_from_surface(surface);
  cairo_surface_destroy(surface);
  g_object_unref(pixbuf);
  return image;
}

static int image_member(lua_State *L)
{
  lua_button button;
  GtkWidget *image;

  luaA_to(L, lua_button, &button, 1);
  if(lua_gettop(L) > 2)
  {
    const char * imagefile = luaL_checkstring(L, 3);
    image = _image_new_from_file(button->widget, imagefile);

    GtkWidget *child = gtk_bin_get_child(GTK_BIN(button->widget));

    if(GTK_IS_BOX(child))
    {
      GtkWidget *old_image = dt_lua_button_get_widget_in_box(child, GTK_TYPE_IMAGE);
      int old_idx = old_image ? dt_lua_button_get_child_index(child, old_image) : -1;

      if(old_image)
        gtk_container_remove(GTK_CONTAINER(child), old_image);
      gtk_box_pack_start(GTK_BOX(child), image, FALSE, FALSE, 0);
      if(old_idx >= 0)
        gtk_box_reorder_child(GTK_BOX(child), image, old_idx);
      gtk_widget_show(image);
    }
    else
    {
      const gchar *label_text = gtk_button_get_label(GTK_BUTTON(button->widget));
      GtkWidget *old_label = (child && GTK_IS_LABEL(child)) ? child : NULL;

      PangoEllipsizeMode ellipsize_mode = PANGO_ELLIPSIZE_END;
      GtkAlign halign_val = GTK_ALIGN_FILL;
      gboolean has_label_style = (old_label != NULL);
      if(old_label)
      {
        ellipsize_mode = gtk_label_get_ellipsize(GTK_LABEL(old_label));
        halign_val = gtk_widget_get_halign(GTK_WIDGET(old_label));
      }

      GtkPositionType pos = position_type_store.used ? position_type_store.position : GTK_POS_LEFT;
      GtkOrientation orientation = (pos == GTK_POS_TOP || pos == GTK_POS_BOTTOM)
        ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL;
      gboolean image_first = (pos == GTK_POS_LEFT || pos == GTK_POS_TOP);

      GtkWidget *box = gtk_box_new(orientation, 0);
      GtkWidget *new_label = (label_text && label_text[0]) ? gtk_label_new(label_text) : NULL;

      if(image_first)
        gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);
      if(new_label)
      {
        gtk_box_pack_start(GTK_BOX(box), new_label, FALSE, FALSE, 0);
        if(has_label_style)
        {
          gtk_label_set_ellipsize(GTK_LABEL(new_label), ellipsize_mode);
          gtk_widget_set_halign(GTK_WIDGET(new_label), halign_val);
        }
      }
      if(!image_first)
        gtk_box_pack_start(GTK_BOX(box), image, FALSE, FALSE, 0);

      g_object_set_data(G_OBJECT(box), "dt-image-pos", GINT_TO_POINTER(pos));
      position_type_store.used = FALSE;

      if(child)
        gtk_container_remove(GTK_CONTAINER(button->widget), child);
      gtk_widget_set_halign(GTK_WIDGET(box), GTK_ALIGN_CENTER);
      gtk_container_add(GTK_CONTAINER(button->widget), box);
      gtk_widget_show_all(GTK_WIDGET(button->widget));
    }
    return 0;
  }
  return 0;
}

static int image_position_member(lua_State *L)
{
  lua_button button;
  luaA_to(L, lua_button, &button, 1);
  dt_lua_position_type_t image_position;
  if(lua_gettop(L) > 2)
  {
    luaA_to(L, dt_lua_position_type_t, &image_position, 3);
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(button->widget));
    if(GTK_IS_BOX(child))
    {
      GtkWidget *image = dt_lua_button_get_widget_in_box(child, GTK_TYPE_IMAGE);
      GtkWidget *label = dt_lua_button_get_widget_in_box(child, GTK_TYPE_LABEL);
      if(image)
      {
        GtkOrientation new_orient = (image_position == GTK_POS_LEFT
                                      || image_position == GTK_POS_RIGHT)
          ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL;
        gtk_orientable_set_orientation(GTK_ORIENTABLE(child), new_orient);

        gboolean image_should_be_first = (image_position == GTK_POS_LEFT
                                           || image_position == GTK_POS_TOP);
        if(image_should_be_first)
          gtk_box_reorder_child(GTK_BOX(child), image, 0);
        else if(label)
          gtk_box_reorder_child(GTK_BOX(child), label, 0);

        g_object_set_data(G_OBJECT(child), "dt-image-pos", GINT_TO_POINTER(image_position));
        gtk_widget_queue_resize(GTK_WIDGET(child));
      }
    }
    else
    {
      position_type_store.position = image_position;
      position_type_store.used = TRUE;
    }
    return 0;
  }
  GtkWidget *child = gtk_bin_get_child(GTK_BIN(button->widget));
  if(GTK_IS_BOX(child))
  {
    GtkWidget *image = dt_lua_button_get_widget_in_box(child, GTK_TYPE_IMAGE);
    GtkWidget *label = dt_lua_button_get_widget_in_box(child, GTK_TYPE_LABEL);
    int image_idx = image ? dt_lua_button_get_child_index(child, image) : -1;
    int label_idx = label ? dt_lua_button_get_child_index(child, label) : -1;

    if(image_idx >= 0 && label_idx >= 0)
    {
      GtkOrientation orient = gtk_orientable_get_orientation(GTK_ORIENTABLE(child));
      if(image_idx < label_idx)
        image_position = (orient == GTK_ORIENTATION_HORIZONTAL) ? GTK_POS_LEFT : GTK_POS_TOP;
      else
        image_position = (orient == GTK_ORIENTATION_HORIZONTAL) ? GTK_POS_RIGHT : GTK_POS_BOTTOM;
    }
    else
    {
      /* No image, or its position within the box couldn't be
         determined from ordering alone (e.g. image-only button):
         fall back to whatever position was last recorded. */
      gpointer data = g_object_get_data(G_OBJECT(child), "dt-image-pos");
      image_position = data ? GPOINTER_TO_INT(data) : GTK_POS_LEFT;
    }
  }
  else
  {
    image_position = GTK_POS_LEFT;
  }
  luaA_push(L, dt_lua_position_type_t, &image_position);
  return 1;
}

static int tostring_member(lua_State *L)
{
  lua_button widget;
  luaA_to(L, lua_button, &widget, 1);
  const gchar *text = gtk_button_get_label(GTK_BUTTON(widget->widget));
  gchar *res = g_strdup_printf("%s (\"%s\")", G_OBJECT_TYPE_NAME(widget->widget), text ? text : "");
  lua_pushstring(L, res);
  g_free(res);
  return 1;
}

int dt_lua_init_widget_button(lua_State* L)
{
  dt_lua_init_widget_type(L, &button_type, lua_button, GTK_TYPE_BUTTON);

  lua_pushcfunction(L, tostring_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_setmetafield(L, lua_button, "__tostring");
  lua_pushcfunction(L, label_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_button, "label");
  lua_pushcfunction(L, image_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_button, "image");
  lua_pushcfunction(L, image_position_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_button, "image_position");
  lua_pushcfunction(L, ellipsize_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_button, "ellipsize");
  lua_pushcfunction(L, halign_member);
  dt_lua_gtk_wrap(L);
  dt_lua_type_register(L, lua_button, "halign");
  dt_lua_widget_register_gtk_callback(L, lua_button, "clicked", "clicked_callback", G_CALLBACK(clicked_callback));

  return 0;
}
// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
