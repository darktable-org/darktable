/*
    This file is part of darktable,
    Copyright (C) 2011-2022 darktable developers.

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
#include "common/file_location.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/imageio.h"
#include "common/imageio_module.h"
#include "common/metadata.h"
#include "common/utility.h"
#include "common/variables.h"
#include "control/conf.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/paint.h"
#include "gui/gtk.h"
#include "gui/gtkentry.h"
#include "gui/accelerators.h"
#include "imageio/storage/imageio_storage_api.h"
#ifdef GDK_WINDOWING_QUARTZ
#include "osx/osx.h"
#endif
#include <stdio.h>
#include <stdlib.h>

DT_MODULE(2)

// gui data
typedef struct gallery_t
{
  GtkEntry *entry;
  GtkEntry *title_entry;
} gallery_t;

// saved params
typedef struct dt_imageio_gallery_t
{
  char filename[DT_MAX_PATH_FOR_PARAMS];
  char title[1024];
  char cached_dirname[DT_MAX_PATH_FOR_PARAMS]; // expanded during first img store, not stored in param struct.
  dt_variables_params_t *vp;
  GList *l;
} dt_imageio_gallery_t;

// sorted list of all images
typedef struct pair_t
{
  char line[4096];
  char item[4096];
  int pos;
} pair_t;


const char *name(const struct dt_imageio_module_storage_t *self)
{
  return _("website gallery");
}

void *legacy_params(dt_imageio_module_storage_t *self, const void *const old_params,
                    const size_t old_params_size, const int old_version, const int new_version,
                    size_t *new_size)
{
  if(old_version == 1 && new_version == 2)
  {
    typedef struct dt_imageio_gallery_v1_t
    {
      char filename[1024];
      char title[1024];
      char cached_dirname[1024]; // expanded during first img store, not stored in param struct.
      dt_variables_params_t *vp;
      GList *l;
    } dt_imageio_gallery_v1_t;

    dt_imageio_gallery_t *n = (dt_imageio_gallery_t *)malloc(sizeof(dt_imageio_gallery_t));
    dt_imageio_gallery_v1_t *o = (dt_imageio_gallery_v1_t *)old_params;

    g_strlcpy(n->filename, o->filename, sizeof(n->filename));
    g_strlcpy(n->title, o->title, sizeof(n->title));
    g_strlcpy(n->cached_dirname, o->cached_dirname, sizeof(n->cached_dirname));

    *new_size = self->params_size(self);
    return n;
  }
  return NULL;
}

static void button_clicked(GtkWidget *widget, dt_imageio_module_storage_t *self)
{
  gallery_t *d = (gallery_t *)self->gui_data;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  GtkFileChooserNative *filechooser = gtk_file_chooser_native_new(
         _("select directory"), GTK_WINDOW(win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
         _("_select as output destination"), _("_cancel"));

  gchar *old = g_strdup(gtk_entry_get_text(d->entry));
  char *c = g_strstr_len(old, -1, "$");
  if(c) *c = '\0';
  gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(filechooser), old);
  g_free(old);
  if(gtk_native_dialog_run(GTK_NATIVE_DIALOG(filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    gchar *dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(filechooser));
    char *composed = g_build_filename(dir, "$(FILE_NAME)", NULL);

    // composed can now contain '\': on Windows it's the path separator,
    // on other platforms it can be part of a regular folder name.
    // This would later clash with variable substitution, so we have to escape them
    gchar *escaped = dt_util_str_replace(composed, "\\", "\\\\");

    gtk_entry_set_text(GTK_ENTRY(d->entry), escaped); // the signal handler will write this to conf
    g_free(dir);
    g_free(composed);
    g_free(escaped);
  }
  g_object_unref(filechooser);
}

static void entry_changed_callback(GtkEntry *entry, gpointer user_data)
{
  dt_conf_set_string("plugins/imageio/storage/gallery/file_directory", gtk_entry_get_text(entry));
}

static void title_changed_callback(GtkEntry *entry, gpointer user_data)
{
  dt_conf_set_string("plugins/imageio/storage/gallery/title", gtk_entry_get_text(entry));
}

void gui_init(dt_imageio_module_storage_t *self)
{
  gallery_t *d = (gallery_t *)malloc(sizeof(gallery_t));
  self->gui_data = (void *)d;
  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);
  GtkWidget *widget;

  d->entry = GTK_ENTRY(dt_action_entry_new(DT_ACTION(self), N_("path"), G_CALLBACK(entry_changed_callback), self,
                                           _("enter the path where to put exported images\nvariables support bash like string manipulation\n"
                                             "type '$(' to activate the completion and see the list of variables"),
                                           dt_conf_get_string_const("plugins/imageio/storage/gallery/file_directory")));
  dt_gtkentry_setup_completion(d->entry, dt_gtkentry_get_default_path_compl_list());
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(d->entry), TRUE, TRUE, 0);

  widget = dtgtk_button_new(dtgtk_cairo_paint_directory, CPF_NONE, NULL);
  gtk_widget_set_name(widget, "non-flat");
  gtk_widget_set_tooltip_text(widget, _("select directory"));
  gtk_box_pack_start(GTK_BOX(hbox), widget, FALSE, FALSE, 0);
  g_signal_connect(G_OBJECT(widget), "clicked", G_CALLBACK(button_clicked), self);

  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(hbox), dt_ui_label_new(_("title")), FALSE, FALSE, 0);
  d->title_entry = GTK_ENTRY(dt_action_entry_new(DT_ACTION(self), N_("title"), G_CALLBACK(title_changed_callback), self,
                                                 _("enter the title of the website"),
                                                 dt_conf_get_string_const("plugins/imageio/storage/gallery/title")));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(d->title_entry), TRUE, TRUE, 0);
}

void gui_cleanup(dt_imageio_module_storage_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_storage_t *self)
{
  gallery_t *d = (gallery_t *)self->gui_data;
  gtk_entry_set_text(d->entry, dt_confgen_get("plugins/imageio/storage/gallery/file_directory", DT_DEFAULT));
  gtk_entry_set_text(d->title_entry, dt_confgen_get("plugins/imageio/storage/gallery/title", DT_DEFAULT));
  dt_conf_set_string("plugins/imageio/storage/gallery/file_directory", gtk_entry_get_text(d->entry));
  dt_conf_set_string("plugins/imageio/storage/gallery/title", gtk_entry_get_text(d->title_entry));
}

static gint sort_pos(pair_t *a, pair_t *b)
{
  return a->pos - b->pos;
}

int store(dt_imageio_module_storage_t *self, dt_imageio_module_data_t *sdata, const int imgid,
          dt_imageio_module_format_t *format, dt_imageio_module_data_t *fdata, const int num, const int total,
          const gboolean high_quality, const gboolean upscale, const gboolean export_masks,
          dt_colorspaces_color_profile_type_t icc_type, const gchar *icc_filename, dt_iop_color_intent_t icc_intent,
          dt_export_metadata_t *metadata)
{
  dt_imageio_gallery_t *d = (dt_imageio_gallery_t *)sdata;

  char filename[PATH_MAX] = { 0 };
  char dirname[PATH_MAX] = { 0 };
  gboolean from_cache = FALSE;
  dt_image_full_path(imgid, dirname, sizeof(dirname), &from_cache);

  char tmp_dir[PATH_MAX] = { 0 };

  // set variable values to expand them afterwards in darktable variables
  dt_variables_set_max_width_height(d->vp, fdata->max_width, fdata->max_height);
  dt_variables_set_upscale(d->vp, upscale);

  d->vp->filename = dirname;
  d->vp->jobcode = "export";
  d->vp->imgid = imgid;
  d->vp->sequence = num;

  gchar *result_tmp_dir = dt_variables_expand(d->vp, d->filename, TRUE);
  g_strlcpy(tmp_dir, result_tmp_dir, sizeof(tmp_dir));
  g_free(result_tmp_dir);

  // if filenamepattern is a directory just let att ${FILE_NAME} as default..
  if(g_file_test(tmp_dir, G_FILE_TEST_IS_DIR)
     || ((d->filename + strlen(d->filename) - 1)[0] == '/'
         || (d->filename + strlen(d->filename) - 1)[0] == '\\'))
    snprintf(d->filename + strlen(d->filename), sizeof(d->filename) - strlen(d->filename), "/$(FILE_NAME)");

  // avoid braindead export which is bound to overwrite at random:
  if(total > 1 && !g_strrstr(d->filename, "$"))
  {
    snprintf(d->filename + strlen(d->filename), sizeof(d->filename) - strlen(d->filename), "_$(SEQUENCE)");
  }

  gchar *fixed_path = dt_util_fix_path(d->filename);
  g_strlcpy(d->filename, fixed_path, sizeof(d->filename));
  g_free(fixed_path);

  gchar *result_filename = dt_variables_expand(d->vp, d->filename, TRUE);
  g_strlcpy(filename, result_filename, sizeof(filename));
  g_free(result_filename);

  g_strlcpy(dirname, filename, sizeof(dirname));

  const char *ext = format->extension(fdata);
  char *c = dirname + strlen(dirname);
  for(; c > dirname && *c != '/'; c--)
    ;
  if(*c == '/') *c = '\0';
  if(g_mkdir_with_parents(dirname, 0755))
  {
    fprintf(stderr, "[imageio_storage_gallery] could not create directory: `%s'!\n", dirname);
    dt_control_log(_("could not create directory `%s'!"), dirname);
    return 1;
  }

  // store away dir.
  g_strlcpy(d->cached_dirname, dirname, sizeof(d->cached_dirname));

  c = filename + strlen(filename);
  for(; c > filename && *c != '.' && *c != '/'; c--)
    ;
  if(c <= filename || *c == '/') c = filename + strlen(filename);

  sprintf(c, ".%s", ext);

  // save image to list, in order:
  pair_t *pair = malloc(sizeof(pair_t));

  char *title = NULL, *description = NULL;
  GList *res_title = NULL, *res_desc = NULL;

  if((metadata->flags & DT_META_METADATA) && !(metadata->flags & DT_META_CALCULATED))
  {
    res_title = dt_metadata_get(imgid, "Xmp.dc.title", NULL);
    if(res_title)
    {
      title = res_title->data;
    }

    res_desc = dt_metadata_get(imgid, "Xmp.dc.description", NULL);
    if(res_desc)
    {
      description = res_desc->data;
    }
  }

  char relfilename[PATH_MAX] = { 0 }, relthumbfilename[PATH_MAX] = { 0 };
  c = filename + strlen(filename);
  for(; c > filename && *c != '/'; c--)
    ;
  if(*c == '/') c++;
  if(c <= filename) c = filename;
  g_strlcpy(relfilename, c, sizeof(relfilename));
  g_strlcpy(relthumbfilename, relfilename, sizeof(relthumbfilename));
  c = relthumbfilename + strlen(relthumbfilename);
  for(; c > relthumbfilename && *c != '.'; c--)
    ;
  if(c <= relthumbfilename) c = relthumbfilename + strlen(relthumbfilename);
  sprintf(c, "-thumb.%s", ext);

  char subfilename[PATH_MAX] = { 0 }, relsubfilename[PATH_MAX] = { 0 };
  g_strlcpy(subfilename, d->cached_dirname, sizeof(subfilename));
  char *sc = subfilename + strlen(subfilename);
  sprintf(sc, "/img_%d.html", num);
  snprintf(relsubfilename, sizeof(relsubfilename), "img_%d.html", num);

  // escape special character and especially " which is used in <img> and below in src and msrc

  gchar *esc_relfilename = g_strescape(relfilename, NULL);
  gchar *esc_relthumbfilename = g_strescape(relthumbfilename, NULL);

  snprintf(pair->line, sizeof(pair->line),
           "\n"
           "      <div><div class=\"dia\">\n"
           "      <img src=\"%s\" alt=\"img%d\" class=\"img\" onclick=\"openSwipe(%d)\"/></div>\n"
           "      <h1>%s</h1>\n"
           "      %s</div>\n",
           esc_relthumbfilename,
           num, num-1, title ? title : "&nbsp;", description ? description : "&nbsp;");

  if(res_title) g_list_free_full(res_title, &g_free);
  if(res_desc) g_list_free_full(res_desc, &g_free);

  // export image to file. need this to be able to access meaningful
  // fdata->width and height below.
  if(dt_imageio_export(imgid, filename, format, fdata, high_quality, upscale, TRUE, export_masks, icc_type,
                       icc_filename, icc_intent, self, sdata, num, total, metadata) != 0)
  {
    fprintf(stderr, "[imageio_storage_gallery] could not export to file: `%s'!\n", filename);
    dt_control_log(_("could not export to file `%s'!"), filename);
    free(pair);
    g_free(esc_relfilename);
    g_free(esc_relthumbfilename);
    return 1;
  }

  snprintf(pair->item, sizeof(pair->item),
           "{\n"
           "src: \"%s\",\n"
           "w: %d,\n"
           "h: %d,\n"
           "msrc: \"%s\",\n"
           "},\n",
           esc_relfilename, fdata->width, fdata->height, esc_relthumbfilename);

  g_free(esc_relfilename);
  g_free(esc_relthumbfilename);

  pair->pos = num;
  d->l = g_list_insert_sorted(d->l, pair, (GCompareFunc)sort_pos);

  /* also export thumbnail: */
  // write with reduced resolution:
  const int save_max_width = fdata->max_width;
  const int save_max_height = fdata->max_height;
  fdata->max_width = 200;
  fdata->max_height = 200;
  // alter filename with -thumb:
  c = filename + strlen(filename);
  for(; c > filename && *c != '.' && *c != '/'; c--)
    ;
  if(c <= filename || *c == '/') c = filename + strlen(filename);
  ext = format->extension(fdata);
  sprintf(c, "-thumb.%s", ext);
  if(dt_imageio_export(imgid, filename, format, fdata, FALSE, TRUE, FALSE, export_masks, icc_type, icc_filename,
                       icc_intent, self, sdata, num, total, NULL) != 0)
  {
    fprintf(stderr, "[imageio_storage_gallery] could not export to file: `%s'!\n", filename);
    dt_control_log(_("could not export to file `%s'!"), filename);
    return 1;
  }
  // restore for next image:
  fdata->max_width = save_max_width;
  fdata->max_height = save_max_height;

  printf("[export_job] exported to `%s'\n", filename);
  dt_control_log(ngettext("%d/%d exported to `%s'", "%d/%d exported to `%s'", num),
                 num, total, filename);
  return 0;
}

void finalize_store(dt_imageio_module_storage_t *self, dt_imageio_module_data_t *dd)
{
  dt_imageio_gallery_t *d = (dt_imageio_gallery_t *)dd;
  char filename[PATH_MAX] = { 0 };
  g_strlcpy(filename, d->cached_dirname, sizeof(filename));
  char *c = filename + strlen(filename);

  // also create style/ subdir:
  sprintf(c, "/style");
  g_mkdir_with_parents(filename, 0755);
  sprintf(c, "/style/style.css");
  dt_copy_resource_file("/style/style.css", filename);
  sprintf(c, "/style/favicon.ico");
  dt_copy_resource_file("/style/favicon.ico", filename);

  // create subdir pswp for photoswipe scripts
  sprintf(c, "/pswp/default-skin/");
  g_mkdir_with_parents(filename, 0755);
  sprintf(c, "/pswp/photoswipe.js");
  dt_copy_resource_file("/pswp/photoswipe.js", filename);
  sprintf(c, "/pswp/photoswipe.min.js");
  dt_copy_resource_file("/pswp/photoswipe.min.js", filename);
  sprintf(c, "/pswp/photoswipe-ui-default.js");
  dt_copy_resource_file("/pswp/photoswipe-ui-default.js", filename);
  sprintf(c, "/pswp/photoswipe.css");
  dt_copy_resource_file("/pswp/photoswipe.css", filename);
  sprintf(c, "/pswp/photoswipe-ui-default.min.js");
  dt_copy_resource_file("/pswp/photoswipe-ui-default.min.js", filename);
  sprintf(c, "/pswp/default-skin/default-skin.css");
  dt_copy_resource_file("/pswp/default-skin/default-skin.css", filename);
  sprintf(c, "/pswp/default-skin/default-skin.png");
  dt_copy_resource_file("/pswp/default-skin/default-skin.png", filename);
  sprintf(c, "/pswp/default-skin/default-skin.svg");
  dt_copy_resource_file("/pswp/default-skin/default-skin.svg", filename);
  sprintf(c, "/pswp/default-skin/preloader.gif");
  dt_copy_resource_file("/pswp/default-skin/preloader.gif", filename);

  sprintf(c, "/index.html");

  const char *title = d->title;

  FILE *f = g_fopen(filename, "wb");
  if(!f) return;
  fprintf(f,
          "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.0 Transitional//EN\" "
          "\"http://www.w3.org/TR/xhtml1/DTD/xhtml1-transitional.dtd\">\n"
          "<html xmlns=\"http://www.w3.org/1999/xhtml\">\n"
          "  <head>\n"
          "    <meta http-equiv=\"Content-type\" content=\"text/html;charset=UTF-8\" />\n"
          "    <link rel=\"shortcut icon\" href=\"style/favicon.ico\" />\n"
          "    <link rel=\"stylesheet\" href=\"style/style.css\" type=\"text/css\" />\n"
          "    <link rel=\"stylesheet\" href=\"pswp/photoswipe.css\">\n"
          "    <link rel=\"stylesheet\" href=\"pswp/default-skin/default-skin.css\">\n"
          "    <script src=\"pswp/photoswipe.min.js\"></script>\n"
          "    <script src=\"pswp/photoswipe-ui-default.min.js\"></script>\n"
          "    <title>%s</title>\n"
          "  </head>\n"
          "  <body>\n"
          "    <div class=\"title\">%s</div>\n"
          "    <div class=\"page\">\n",
          title, title);

  size_t count = 0;
  for(GList *tmp = d->l; tmp; tmp = g_list_next(tmp))
  {
    pair_t *p = (pair_t *)tmp->data;
    fprintf(f, "%s", p->line);
    count++;
  }

  fprintf(f, "        <p style=\"clear:both;\"></p>\n"
             "    </div>\n"
             "    <div class=\"footer\">\n"
             "      <script language=\"JavaScript\" type=\"text/javascript\">\n"
             "      document.write(\"download all: <em>curl -O#  \" + document.documentURI.replace( /\\\\/g, '/' ).replace( /\\/[^\\/]*$/, '' ) + \"/img_[0000-%04zu].jpg</em>\")\n"
             "      </script><br />\n"
             "      created with %s\n"
             "    </div>\n"
             "    <div class=\"pswp\" tabindex=\"-1\" role=\"dialog\" aria-hidden=\"true\">\n"
             "        <div class=\"pswp__bg\"></div>\n"
             "        <div class=\"pswp__scroll-wrap\">\n"
             "            <div class=\"pswp__container\">\n"
             "                <div class=\"pswp__item\"></div>\n"
             "                <div class=\"pswp__item\"></div>\n"
             "                <div class=\"pswp__item\"></div>\n"
             "            </div>\n"
             "            <div class=\"pswp__ui pswp__ui--hidden\">\n"
             "                <div class=\"pswp__top-bar\">\n"
             "                    <div class=\"pswp__counter\"></div>\n"
             "                    <button class=\"pswp__button pswp__button--close\" title=\"Close (Esc)\"></button>\n"
             "                    <button class=\"pswp__button pswp__button--share\" title=\"Share\"></button>\n"
             "                    <button class=\"pswp__button pswp__button--fs\" title=\"Toggle fullscreen\"></button>\n"
             "                    <button class=\"pswp__button pswp__button--zoom\" title=\"Zoom in/out\"></button>\n"
             "                    <div class=\"pswp__preloader\">\n"
             "                        <div class=\"pswp__preloader__icn\">\n"
             "                          <div class=\"pswp__preloader__cut\">\n"
             "                            <div class=\"pswp__preloader__donut\"></div>\n"
             "                          </div>\n"
             "                        </div>\n"
             "                   </div>\n"
             "                </div>\n"
             "                <div class=\"pswp__share-modal pswp__share-modal--hidden pswp__single-tap\">\n"
             "                    <div class=\"pswp__share-tooltip\"></div>\n"
             "                </div>\n"
             "                <button class=\"pswp__button pswp__button--arrow--left\" title=\"Previous (arrow left)\">\n"
             "                </button>\n"
             "                <button class=\"pswp__button pswp__button--arrow--right\" title=\"Next (arrow right)\">\n"
             "                </button>\n"
             "                <div class=\"pswp__caption\">\n"
             "                    <div class=\"pswp__caption__center\"></div>\n"
             "                </div>\n"
             "            </div>\n"
             "        </div>\n"
             "    </div>\n"
             "  </body>\n"
             "<script>\n"
             "var pswpElement = document.querySelectorAll('.pswp')[0];\n"
             "var items = [\n",
          count,
          darktable_package_string);
  while(d->l)
  {
    pair_t *p = (pair_t *)d->l->data;
    fprintf(f, "%s", p->item);
    free(p);
    d->l = g_list_delete_link(d->l, d->l);
  }
  fprintf(f, "];\n"
             "function openSwipe(img)\n"
             "{\n"
             "    // define options (if needed)\n"
             "    var options = {\n"
             "          // optionName: 'option value'\n"
             "          index: img // start at first slide\n"
             "    };\n"
             "    var gallery = new PhotoSwipe( pswpElement, PhotoSwipeUI_Default, items, options);\n"
             "    gallery.init();\n"
             "}\n"
             "</script>\n"
             "</html>\n");
  fclose(f);
}

size_t params_size(dt_imageio_module_storage_t *self)
{
  return sizeof(dt_imageio_gallery_t) - 2 * sizeof(void *) - DT_MAX_PATH_FOR_PARAMS;
}

void init(dt_imageio_module_storage_t *self)
{
#ifdef USE_LUA
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_gallery_t, filename,
                                char_path_length);
  dt_lua_register_module_member(darktable.lua_state.state, self, dt_imageio_gallery_t, title, char_1024);
#endif
}

void *get_params(dt_imageio_module_storage_t *self)
{
  dt_imageio_gallery_t *d = (dt_imageio_gallery_t *)calloc(1, sizeof(dt_imageio_gallery_t));
  d->vp = NULL;
  d->l = NULL;
  dt_variables_params_init(&d->vp);

  const char *text = dt_conf_get_string_const("plugins/imageio/storage/gallery/file_directory");
  g_strlcpy(d->filename, text, sizeof(d->filename));

  text = dt_conf_get_string_const("plugins/imageio/storage/gallery/title");
  g_strlcpy(d->title, text, sizeof(d->title));

  return d;
}

void free_params(dt_imageio_module_storage_t *self, dt_imageio_module_data_t *params)
{
  if(!params) return;
  dt_imageio_gallery_t *d = (dt_imageio_gallery_t *)params;
  dt_variables_params_destroy(d->vp);
  free(params);
}

int set_params(dt_imageio_module_storage_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;
  dt_imageio_gallery_t *d = (dt_imageio_gallery_t *)params;
  gallery_t *g = (gallery_t *)self->gui_data;
  gtk_entry_set_text(GTK_ENTRY(g->entry), d->filename);
  dt_conf_set_string("plugins/imageio/storage/gallery/file_directory", d->filename);
  gtk_entry_set_text(GTK_ENTRY(g->title_entry), d->title);
  dt_conf_set_string("plugins/imageio/storage/gallery/title", d->title);
  return 0;
}

int supported(dt_imageio_module_storage_t *storage, dt_imageio_module_format_t *format)
{
  const char *mime = format->mime(NULL);
  if(strcmp(mime, "image/jpeg") == 0) return 1;
  if(strcmp(mime, "image/png") == 0) return 1;
  if(strcmp(mime, "image/webp") == 0) return 1;
  if(strcmp(mime, "image/avif") == 0) return 1;
  if(strcmp(mime, "image/jxl") == 0) return 1;

  return 0;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
