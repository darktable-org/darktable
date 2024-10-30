/*
    This file is part of darktable,

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

/* workaround to fix issue #12720 */
#define _DARWIN_C_SOURCE

#include <Carbon/Carbon.h>
#include <ApplicationServices/ApplicationServices.h>
#include <CoreServices/CoreServices.h>
#include <AppKit/AppKit.h>
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_QUARTZ
#include <gdk/gdkquartz.h>
#endif
#include <gio/gio.h>
#include <glib.h>
#include <string.h>
#ifdef MAC_INTEGRATION
#include <gtkosxapplication.h>
#endif
#ifdef HAVE_P11KIT
#define P11_KIT_FUTURE_UNSTABLE_API
#include <p11-kit/p11-kit.h>
#endif
#include "osx.h"
#include "libintl.h"


float dt_osx_get_ppd()
{
  @autoreleasepool
  {
    NSScreen *nsscreen = [NSScreen mainScreen];
    if([nsscreen respondsToSelector: NSSelectorFromString(@"backingScaleFactor")])
    {
      return [[nsscreen valueForKey: @"backingScaleFactor"] floatValue];
    }
    else
    {
      return [[nsscreen valueForKey: @"userSpaceScaleFactor"] floatValue];
    }
  }
}

#if !GTK_CHECK_VERSION(3, 24, 14)
static void dt_osx_disable_fullscreen(GtkWidget *widget)
{
#ifdef GDK_WINDOWING_QUARTZ
  @autoreleasepool
  {
    GdkWindow *window = gtk_widget_get_window(widget);
    if(window)
    {
      NSWindow *native = gdk_quartz_window_get_nswindow(window);
      [native setCollectionBehavior: ([native collectionBehavior] & ~NSWindowCollectionBehaviorFullScreenPrimary) | NSWindowCollectionBehaviorFullScreenAuxiliary];
    }
  }
#endif
}
#endif

void dt_osx_disallow_fullscreen(GtkWidget *widget)
{
#if !GTK_CHECK_VERSION(3, 24, 14)
#ifdef GDK_WINDOWING_QUARTZ
  if(gtk_widget_get_realized(widget))
    dt_osx_disable_fullscreen(widget);
  else
    g_signal_connect(G_OBJECT(widget), "realize", G_CALLBACK(dt_osx_disable_fullscreen), NULL);
#endif
#endif
}

gboolean dt_osx_file_trash(const char *filename, GError **error)
{
  @autoreleasepool
  {
    NSFileManager *fm = [NSFileManager defaultManager];
    NSError *err;

    NSURL *url = [NSURL fileURLWithPath:@(filename)];

    if ([fm respondsToSelector:@selector(trashItemAtURL:resultingItemURL:error:)]) {
      if (![fm trashItemAtURL:url resultingItemURL:nil error:&err]) {
        if (error != NULL)
          *error = g_error_new_literal(G_IO_ERROR, err.code == NSFileNoSuchFileError ? G_IO_ERROR_NOT_FOUND : G_IO_ERROR_FAILED, err.localizedDescription.UTF8String);
        return FALSE;
      }
    } else {
      if (error != NULL)
        *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "trash not supported on OS X versions < 10.8");
      return FALSE;
    }
    return TRUE;
  }
}

char* dt_osx_get_bundle_res_path()
{
  char *result = NULL;
#ifdef MAC_INTEGRATION
  gchar *bundle_id;

  bundle_id = gtkosx_application_get_bundle_id();
  if(bundle_id)
    result = gtkosx_application_get_resource_path();
  g_free(bundle_id);

#endif

  return result;
}

static char* _get_user_locale()
{
  @autoreleasepool
  {
    NSLocale* locale_ns = [NSLocale currentLocale];
    NSString* locale_c;
    if([locale_ns respondsToSelector: @selector(languageCode)] && [locale_ns respondsToSelector: @selector(countryCode)])
    {
      locale_c = [NSString stringWithFormat: @"%@_%@", [locale_ns languageCode], [locale_ns countryCode]];
    }
    else
    {
      // not ideal, but better than nothing
      locale_c = [locale_ns localeIdentifier];
    }
    return strdup([locale_c UTF8String]);
  }
}

static void _setup_ssl_trust(const char* const res_path)
{
#ifdef HAVE_P11KIT
  gchar* const hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, res_path, strlen(res_path));
  gchar* const file_path = g_build_filename(g_get_user_data_dir(), "darktable", "pkcs11", hash, "p11-kit-trust.module", NULL);
  g_free(hash);
  {
    GFile* const cfg_file = g_file_new_for_path(file_path);
    {
      GFile* const cfg_dir = g_file_get_parent(cfg_file);
      g_file_make_directory_with_parents(cfg_dir, NULL, NULL);
      {
        const char* const dir_path = g_file_get_path(cfg_dir);
        p11_kit_override_system_files("", NULL, "", dir_path, NULL);
        // p11_kit_override_system_files function doesn't copy its parameters,
        // so do NOT call g_free(dir_path);
      }
      g_object_unref(cfg_dir);
    }
    {
      gchar* const buf = g_strdup_printf("module: %s/lib/pkcs11/p11-kit-trust.so\n"
                                         "trust-policy: yes\n"
                                         "x-init-reserved: paths=%s/share/curl/curl-ca-bundle.crt\n",
                                         res_path, res_path);
      g_file_replace_contents(cfg_file, buf, strlen(buf), NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, NULL);
      g_free(buf);
    }
    g_object_unref(cfg_file);
  }
  g_free(file_path);
#endif
}

void dt_osx_prepare_environment()
{
  // check that LC_CTYPE is set to something sane
  // on macOS it's usually set to UTF-8
  // which is fine for native setlocale function
  // but since we link with libintl
  // we are actually using libintl_setlocale (in case of macOS)
  // which expects LC_CTYPE to be normal locale name
  // otherwise calling setlocale(LC_ALL, "") fails
  const gchar* ctype = g_getenv("LC_CTYPE");
  if(ctype)
  {
    char *saved_locale = strdup(setlocale(LC_ALL, NULL));
    if(!setlocale(LC_ALL, ctype))
    {
      g_unsetenv("LC_CTYPE");
    }
    else
    {
      setlocale(LC_ALL, saved_locale);
    }
    free(saved_locale);
  }
  // set LANG according to user settings, unless already set
  // otherwise we may get some non-default interface language
  // and not even detect it
  char* user_locale = _get_user_locale();
  g_setenv("LANG", user_locale, FALSE);
  free(user_locale);
  // set all required paths if we are in the app bundle
  char* res_path = dt_osx_get_bundle_res_path();
  if(res_path)
  {
    g_setenv("GTK_DATA_PREFIX", res_path, TRUE);
    g_setenv("GTK_EXE_PREFIX", res_path, TRUE);
    g_setenv("GTK_PATH", res_path, TRUE);

    gchar* etc_path = g_build_filename(res_path, "etc", NULL);
    gchar* lib_path = g_build_filename(res_path, "lib", NULL);

    {
      g_setenv("XDG_CONFIG_DIRS", etc_path, TRUE);
      {
        gchar* gtk_im_path = g_build_filename(etc_path, "gtk-3.0", "gtk.immodules", NULL);
        g_setenv("GTK_IM_MODULE_FILE", gtk_im_path, TRUE);
        g_free(gtk_im_path);
      }
      {
        gchar* pixbuf_path = g_build_filename(etc_path, "gtk-3.0", "loaders.cache", NULL);
        g_setenv("GDK_PIXBUF_MODULE_FILE", pixbuf_path, TRUE);
        g_free(pixbuf_path);
      }
    }
    {
      gchar* share_path = g_build_filename(res_path, "share", NULL);
      g_setenv("XDG_DATA_DIRS", share_path, TRUE);
      {
        gchar* schema_path = g_build_filename(share_path, "glib-2.0", "schemas", NULL);
        g_setenv("GSETTINGS_SCHEMA_DIR", schema_path, TRUE);
        g_free(schema_path);
      }
      g_free(share_path);
    }
    {
      {
        gchar* io_path = g_build_filename(lib_path, "libgphoto2_port", NULL);
        g_setenv("IOLIBS", io_path, TRUE);
        g_free(io_path);
      }
      {
        gchar* cam_path = g_build_filename(lib_path, "libgphoto2", NULL);
        g_setenv("CAMLIBS", cam_path, TRUE);
        g_free(cam_path);
      }
      {
        gchar* gio_path = g_build_filename(lib_path, "gio", "modules", NULL);
        g_setenv("GIO_MODULE_DIR", gio_path, TRUE);
        g_free(gio_path);
      }
    }

#ifdef HAVE_IMAGEMAGICK7
    {
      g_setenv("MAGICK_HOME", res_path, TRUE);
      gchar* im_config_path = g_build_filename(etc_path, "ImageMagick-7", NULL);
      g_setenv("MAGICK_CONFIGURE_PATH", im_config_path, TRUE);
      g_free(im_config_path);
      gchar* im_modules_path = g_build_filename(lib_path, "ImageMagick", "modules-Q16HDRI", NULL);
      g_setenv("MAGICK_CODER_MODULE_PATH", im_modules_path, TRUE);
      g_setenv("MAGICK_CODER_FILTER_PATH", im_modules_path, TRUE);
      g_free(im_modules_path);
    }
#endif

    _setup_ssl_trust(res_path); //uses GIO, so call after GIO_MODULE_DIR is set

    g_free(etc_path);
    g_free(lib_path);
    g_free(res_path);
  }
}

void dt_osx_focus_window()
{
  [NSApp activateIgnoringOtherApps:YES];
}

gboolean dt_osx_open_url(const char *url)
{
  return [[NSWorkspace sharedWorkspace] openURL:[NSURL URLWithString:@(url)]];
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
