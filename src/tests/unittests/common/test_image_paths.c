/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>

#include <cmocka.h>

#include "common/act_on.h"
#include "common/image.h"
#include <gtk/gtk.h>

static GList *_selected_images;
static const char *_original_paths[] = {
  NULL,
  "/photos/first folder/image.raw",
  "/photos/other folder/image.raw",
  "/photos/日本語/café.raw",
  "/photos/first folder/image.raw"
};
static char *_copied_text;
static gboolean _real_clipboard;

static GList *_test_act_on_images(const gboolean only_visible,
                                  const gboolean force,
                                  const gboolean ordered)
{
  assert_false(only_visible);
  assert_true(force);
  assert_true(ordered);
  return g_list_copy(_selected_images);
}

static void _test_image_full_path(const dt_imgid_t imgid,
                                   char *pathname,
                                   const size_t pathname_len,
                                   gboolean *from_cache)
{
  // requesting cached files here would publish a different path for local copies
  assert_null(from_cache);
  if(imgid > 0 && imgid < G_N_ELEMENTS(_original_paths))
    g_strlcpy(pathname, _original_paths[imgid], pathname_len);
}

static GtkClipboard *_test_clipboard_get_default(GdkDisplay *display)
{
  return _real_clipboard ? gtk_clipboard_get_default(display) : NULL;
}

static void _test_clipboard_set_text(GtkClipboard *clipboard,
                                      const gchar *text,
                                      gint len)
{
  assert_int_equal(len, -1);
  g_free(_copied_text);
  _copied_text = g_strdup(text);
  if(_real_clipboard) gtk_clipboard_set_text(clipboard, text, len);
}

#define dt_act_on_get_images _test_act_on_images
#define dt_image_full_path _test_image_full_path
#define gtk_clipboard_get_default _test_clipboard_get_default
#define gtk_clipboard_set_text _test_clipboard_set_text
#include "libs/image.c"
#undef dt_act_on_get_images
#undef dt_image_full_path
#undef gtk_clipboard_get_default
#undef gtk_clipboard_set_text

#ifdef _WIN32
#include "win/main_wrapper.h"
#endif

static int _setup(void **state)
{
  _selected_images = NULL;
  _copied_text = g_strdup("previous clipboard contents");
  return 0;
}

static int _teardown(void **state)
{
  g_list_free(_selected_images);
  g_free(_copied_text);
  return 0;
}

static void test_single_original_path(void **state)
{
  _selected_images = g_list_append(NULL, GINT_TO_POINTER(1));
  _copy_file_paths_callback(NULL, NULL);
  assert_string_equal(_copied_text, _original_paths[1]);
}

static void test_distinct_paths_in_selection_order(void **state)
{
  const dt_imgid_t ids[] = { 3, 2, 4, 1 };
  for(size_t i = 0; i < G_N_ELEMENTS(ids); i++)
    _selected_images = g_list_append(_selected_images, GINT_TO_POINTER(ids[i]));
  _copy_file_paths_callback(NULL, NULL);
  assert_string_equal(_copied_text,
                      "/photos/日本語/café.raw\n"
                      "/photos/other folder/image.raw\n"
                      "/photos/first folder/image.raw");
}

static void test_empty_selection_preserves_clipboard(void **state)
{
  _copy_file_paths_callback(NULL, NULL);
  assert_string_equal(_copied_text, "previous clipboard contents");
}

static void test_missing_image_preserves_clipboard(void **state)
{
  _selected_images = g_list_append(NULL, GINT_TO_POINTER(999));
  _copy_file_paths_callback(NULL, NULL);
  assert_string_equal(_copied_text, "previous clipboard contents");
}

static void test_missing_image_is_omitted(void **state)
{
  _selected_images = g_list_append(NULL, GINT_TO_POINTER(999));
  _selected_images = g_list_append(_selected_images, GINT_TO_POINTER(2));
  _copy_file_paths_callback(NULL, NULL);
  assert_string_equal(_copied_text, _original_paths[2]);
}

static void test_gtk_clipboard(void **state)
{
  if(!_real_clipboard) skip();
  GtkClipboard *clipboard = gtk_clipboard_get_default(gdk_display_get_default());
  gtk_clipboard_set_text(clipboard, "previous clipboard contents", -1);
  _copy_file_paths_callback(NULL, NULL);
  gchar *text = gtk_clipboard_wait_for_text(clipboard);
  assert_string_equal(text, "previous clipboard contents");
  g_free(text);

  _selected_images = g_list_append(NULL, GINT_TO_POINTER(3));
  _copy_file_paths_callback(NULL, NULL);
  text = gtk_clipboard_wait_for_text(clipboard);
  assert_string_equal(text, _original_paths[3]);
  g_free(text);
}

int main(int argc, char *argv[])
{
  if(argc > 1 && !strcmp(argv[1], "--clipboard"))
  {
    gtk_init(NULL, NULL);
    _real_clipboard = TRUE;
  }

  const struct CMUnitTest tests[] = {
    cmocka_unit_test_setup_teardown(test_single_original_path, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_distinct_paths_in_selection_order, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_empty_selection_preserves_clipboard, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_missing_image_preserves_clipboard, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_missing_image_is_omitted, _setup, _teardown),
    cmocka_unit_test_setup_teardown(test_gtk_clipboard, _setup, _teardown)
  };
  return cmocka_run_group_tests(tests, NULL, NULL);
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
