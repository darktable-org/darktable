/*
    This file is part of darktable,
    Copyright (C) 2024 darktable developers.

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
#include "common/image.h"
#include "common/mipmap_cache.h"
#include "control/conf.h"
#include "control/signal.h"
#include "develop/develop.h"
#include "gui/gtk.h"
#include "imageio/proxy.h"
#include "libs/lib.h"
#include "libs/lib_api.h"

DT_MODULE(1)

typedef struct dt_lib_proxy_media_t
{
  GtkWidget *source_label;
  GtkWidget *toggle_button;
  gboolean   generating;
} dt_lib_proxy_media_t;

typedef struct _gen_ctx_t
{
  dt_lib_module_t *self;
  dt_imgid_t       imgid;
  char             filename[PATH_MAX];
  int              quality;
} _gen_ctx_t;

const char *name(dt_lib_module_t *self)
{
  return _("media source");
}

dt_view_type_flags_t views(dt_lib_module_t *self)
{
  return DT_VIEW_DARKROOM;
}

uint32_t container(dt_lib_module_t *self)
{
  return DT_UI_CONTAINER_PANEL_LEFT_BOTTOM;
}

int position(const dt_lib_module_t *self)
{
  return 900;
}

// Must be called on the GTK main thread.
static void _reload_from_source(dt_imgid_t imgid, gboolean prefer_proxy)
{
  dt_conf_set_bool("plugins/darkroom/prefer_proxy_media", prefer_proxy);
  // dt_mipmap_cache_remove_at_size() silently returns for DT_MIPMAP_FULL/F
  // because it guards mip <= DT_MIPMAP_LDR_MAX.  Use evict_at_size instead.
  dt_mipmap_cache_evict_at_size(imgid, DT_MIPMAP_FULL);
  dt_mipmap_cache_evict_at_size(imgid, DT_MIPMAP_F);
  // Proxy is half the linear resolution of the raw.  Reset to fit so the
  // viewport doesn't show a 2× scale jump or a tiny cropped view.
  dt_dev_zoom_move(&darktable.develop->full, DT_ZOOM_FIT, 0.0f, 0, -1.0f, -1.0f, TRUE);
  dt_dev_reload_image(darktable.develop, imgid);
}

static void _update(dt_lib_module_t *self)
{
  dt_lib_proxy_media_t *d = self->data;

  if(d->generating)
    return; // leave label/button in "generating…" state until thread finishes

  if(!darktable.develop || !dt_is_valid_imgid(darktable.develop->image_storage.id))
  {
    gtk_label_set_text(GTK_LABEL(d->source_label), "—");
    gtk_widget_set_sensitive(d->toggle_button, FALSE);
    return;
  }

  const dt_image_t *img = &darktable.develop->image_storage;
  const gboolean using_proxy = img->flags & DT_IMAGE_PROXY_MEDIA;

  char filename[PATH_MAX];
  dt_image_full_path(img->id, filename, sizeof(filename), NULL);

  const gboolean raw_exists   = g_file_test(filename, G_FILE_TEST_IS_REGULAR);
  char proxy_path[PATH_MAX];
  g_snprintf(proxy_path, sizeof(proxy_path), "%s.proxy.avif", filename);
  const gboolean proxy_exists = g_file_test(proxy_path, G_FILE_TEST_IS_REGULAR);

  if(using_proxy)
  {
    gtk_label_set_markup(GTK_LABEL(d->source_label),
                         "<span foreground='orange'><b>proxy</b></span>");
    gtk_button_set_label(GTK_BUTTON(d->toggle_button), _("use original"));
    gtk_widget_set_sensitive(d->toggle_button, raw_exists);
  }
  else if(proxy_exists)
  {
    gtk_label_set_markup(GTK_LABEL(d->source_label), "<b>original</b>");
    gtk_button_set_label(GTK_BUTTON(d->toggle_button), _("use proxy"));
    gtk_widget_set_sensitive(d->toggle_button, TRUE);
  }
  else
  {
    // No proxy on disk at all.
    gtk_label_set_markup(GTK_LABEL(d->source_label), "<b>original</b>");
    gtk_button_set_label(GTK_BUTTON(d->toggle_button), _("generate proxy"));
    // Can only generate if we actually have the raw to read from.
    gtk_widget_set_sensitive(d->toggle_button, raw_exists);
  }
}

// Called on the GTK main thread via gdk_threads_add_idle after generation.
static gboolean _after_proxy_generated(gpointer user_data)
{
  _gen_ctx_t *ctx = user_data;
  dt_lib_proxy_media_t *d = ctx->self->data;

  d->generating = FALSE;

  // Check if the proxy was actually created before switching.
  char proxy_path[PATH_MAX];
  g_snprintf(proxy_path, sizeof(proxy_path), "%s.proxy.avif", ctx->filename);
  if(g_file_test(proxy_path, G_FILE_TEST_IS_REGULAR)
     && darktable.develop
     && darktable.develop->image_storage.id == ctx->imgid)
  {
    // _reload_from_source blocks until the mipmap is regenerated, so
    // dev->image_storage already has the updated flags when it returns.
    // DT_SIGNAL_DEVELOP_IMAGE_CHANGED only fires during dt_dev_load_image
    // (new-image transitions), not during dt_dev_reload_image, so we must
    // call _update explicitly here to refresh the label/button.
    _reload_from_source(ctx->imgid, TRUE);
    _update(ctx->self);
  }
  else
  {
    _update(ctx->self);
  }

  free(ctx);
  return G_SOURCE_REMOVE;
}

static gpointer _generate_proxy_thread(gpointer user_data)
{
  _gen_ctx_t *ctx = user_data;
  dt_imageio_create_proxy(ctx->filename, ctx->quality);
  gdk_threads_add_idle(_after_proxy_generated, ctx);
  return NULL;
}

static void _toggle_source_clicked(GtkButton *btn, gpointer user_data)
{
  dt_lib_module_t *self = user_data;
  dt_lib_proxy_media_t *d = self->data;

  if(!darktable.develop || !dt_is_valid_imgid(darktable.develop->image_storage.id))
    return;

  const dt_imgid_t imgid = darktable.develop->image_storage.id;
  const dt_image_t *img  = &darktable.develop->image_storage;
  const gboolean using_proxy = img->flags & DT_IMAGE_PROXY_MEDIA;

  char filename[PATH_MAX];
  dt_image_full_path(imgid, filename, sizeof(filename), NULL);

  char proxy_path[PATH_MAX];
  g_snprintf(proxy_path, sizeof(proxy_path), "%s.proxy.avif", filename);
  const gboolean proxy_exists = g_file_test(proxy_path, G_FILE_TEST_IS_REGULAR);

  if(using_proxy)
  {
    // Switch from proxy → original
    _reload_from_source(imgid, FALSE);
  }
  else if(proxy_exists)
  {
    // Switch from original → proxy
    _reload_from_source(imgid, TRUE);
  }
  else
  {
    // No proxy yet — generate one then auto-switch.
    d->generating = TRUE;
    gtk_button_set_label(btn, _("generating…"));
    gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
    gtk_label_set_markup(GTK_LABEL(d->source_label), "<i>generating proxy…</i>");

    _gen_ctx_t *ctx = malloc(sizeof(*ctx));
    ctx->self    = self;
    ctx->imgid   = imgid;
    ctx->quality = dt_conf_get_and_sanitize_int("plugins/lighttable/proxy_quality", 0, 100);
    g_strlcpy(ctx->filename, filename, sizeof(ctx->filename));

    g_thread_new("proxy-generate", _generate_proxy_thread, ctx);
  }
}

static void _image_changed_callback(gpointer instance, dt_lib_module_t *self)
{
  _update(self);
}

void gui_init(dt_lib_module_t *self)
{
  dt_lib_proxy_media_t *d = calloc(1, sizeof(*d));
  self->data = d;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);

  GtkWidget *lbl = gtk_label_new(_("source:"));
  gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);

  d->source_label = gtk_label_new("—");
  gtk_label_set_xalign(GTK_LABEL(d->source_label), 0.0f);
  gtk_box_pack_start(GTK_BOX(box), d->source_label, TRUE, TRUE, 0);

  d->toggle_button = gtk_button_new_with_label(_("generate proxy"));
  gtk_widget_set_sensitive(d->toggle_button, FALSE);
  g_signal_connect(G_OBJECT(d->toggle_button), "clicked",
                   G_CALLBACK(_toggle_source_clicked), self);
  gtk_box_pack_end(GTK_BOX(box), d->toggle_button, FALSE, FALSE, 0);

  self->widget = box;

  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_IMAGE_CHANGED, _image_changed_callback);
  DT_CONTROL_SIGNAL_HANDLE(DT_SIGNAL_DEVELOP_INITIALIZE, _image_changed_callback);
}

void gui_cleanup(dt_lib_module_t *self)
{
  free(self->data);
  self->data = NULL;
}
