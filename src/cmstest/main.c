/*
    This file is part of darktable,
    copyright (c) 2013 tobias ellinghaus.

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

#include "version.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <glib-object.h>
#include <lcms2.h>

#ifdef HAVE_X11
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>
#ifdef HAVE_COLORD
#include <colord.h>
#endif // HAVE_COLORD
#endif // HAVE_X11

#ifdef HAVE_X11
typedef struct monitor_t
{
  int scr;
  Window root;
  int xid;
  char *name;

  // X atom
  char *x_atom_name;
  long x_atom_length;
  unsigned char *x_atom_data;

#ifdef HAVE_COLORD
  // colord
  char *colord_filename;
#endif
} monitor_t;

char *get_profile_description(unsigned char *data, long data_size)
{
  cmsUInt32Number size;
  gchar *buf = NULL;
  wchar_t *wbuf = NULL;
  gchar *utf8 = NULL;
  char *result = NULL;

  if(!data || data_size == 0) return NULL;

  cmsHPROFILE p = cmsOpenProfileFromMem(data, data_size);
  if(!p) return NULL;

  size = cmsGetProfileInfoASCII(p, cmsInfoDescription, "en", "US", NULL, 0);
  if(size == 0) goto error;

  buf = (char *)calloc(size + 1, sizeof(char));
  size = cmsGetProfileInfoASCII(p, cmsInfoDescription, "en", "US", buf, size);
  if(size == 0) goto error;

  // most unix like systems should work with this, but at least Windows doesn't
  if(sizeof(wchar_t) != 4 || g_utf8_validate(buf, -1, NULL))
    result = g_strdup(buf); // better a little weird than totally borked
  else
  {
    wbuf = (wchar_t *)calloc(size + 1, sizeof(wchar_t));
    size = cmsGetProfileInfo(p, cmsInfoDescription, "en", "US", wbuf, sizeof(wchar_t) * size);
    if(size == 0) goto error;
    utf8 = g_ucs4_to_utf8((gunichar *)wbuf, -1, NULL, NULL, NULL);
    if(!utf8) goto error;
    result = g_strdup(utf8);
  }

  free(buf);
  free(wbuf);
  g_free(utf8);
  cmsCloseProfile(p);
  return result;

error:
  if(buf) result = g_strdup(buf); // better a little weird than totally borked
  free(buf);
  free(wbuf);
  g_free(utf8);
  cmsCloseProfile(p);
  return result;
}
#endif // HAVE_X11

int main(int argc, char *arg[])
{
  printf("darktable-cmstest version " PACKAGE_VERSION "\n");
#ifndef HAVE_X11
  printf("this executable doesn't do anything for non-X11 systems currently\n");
  return EXIT_FAILURE;
#else // HAVE_X11

#ifdef HAVE_COLORD
  printf("this executable was built with colord support enabled\n");
#else  // HAVE_COLORD
  printf("this executable was built without colord support\n");
#endif // HAVE_COLORD

#ifdef USE_COLORDGTK
  printf("darktable itself was built with colord support enabled\n");
#else
  printf("darktable itself was built without colord support\n");
#endif // USE_COLORDGTK

#if !GLIB_CHECK_VERSION(2, 35, 0)
  g_type_init();
#endif

  // get a list of all possible screens from xrandr
  GList *monitor_list = NULL;
  const char *disp_name = NULL;
  Display *dpy = XOpenDisplay(disp_name);
  if(dpy == NULL)
  {
    fprintf(stderr, "can't open display `%s'\n", XDisplayName(disp_name));
    return EXIT_FAILURE;
  }
  int max_scr = ScreenCount(dpy);
  for(int scr = 0; scr < max_scr; ++scr)
  {
    Window root = RootWindow(dpy, scr);
    XRRScreenResources *rsrc = XRRGetScreenResources(dpy, root);
    int xid = 0;
    for(int i = 0; i < rsrc->noutput; ++i)
    {
      XRROutputInfo *info = XRRGetOutputInfo(dpy, rsrc, rsrc->outputs[i]);
      if(!info)
      {
        fprintf(stderr, "can't get output info for output %d\n", i);
        return EXIT_FAILURE;
      }
      // only handle those that are attached though
      if(info->connection != RR_Disconnected)
      {
        monitor_t *monitor = (monitor_t *)calloc(1, sizeof(monitor_t));
        // we also want the edid data
        //         Atom edid_atom = XInternAtom(dpy, "EDID", False), actual_type;
        //         int actual_format;
        //         unsigned long nitems, bytes_after;
        //         unsigned char *prop;
        //         int res = XRRGetOutputProperty (dpy, rsrc->outputs[i], edid_atom, 0, G_MAXLONG, FALSE,
        //         FALSE, XA_INTEGER, &actual_type, &actual_format, &nitems, &bytes_after, &prop);
        //         if(res == Success && actual_type == XA_INTEGER && actual_format == 8 && nitems != 0)
        //         {
        //           printf("EDID for %s has size %lu\n", monitor->name, nitems);
        //           // TODO: parse the edid blob in prop. since that is really ugly code I left it out for
        //           now.
        //         }
        //         if(prop)
        //           XFree(prop);

        monitor->scr = scr;
        monitor->root = root;
        monitor->xid = xid;
        monitor->name = g_strdup(info->name);
        monitor_list = g_list_append(monitor_list, monitor);
        xid++;
      }

      XRRFreeOutputInfo(info);
    }
    XRRFreeScreenResources(rsrc);
  }


  // get the profile from the X atom
  GList *iter = g_list_first(monitor_list);
  while(iter)
  {
    monitor_t *monitor = (monitor_t *)iter->data;
    if(monitor->xid == 0)
      monitor->x_atom_name = g_strdup("_ICC_PROFILE");
    else
      monitor->x_atom_name = g_strdup_printf("_ICC_PROFILE_%d", monitor->xid);

    Atom atom = XInternAtom(dpy, monitor->x_atom_name, FALSE), actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop;

    int res = XGetWindowProperty(dpy, monitor->root, atom, 0, G_MAXLONG, FALSE, XA_CARDINAL, &actual_type,
                                 &actual_format, &nitems, &bytes_after, &prop);

    if(res == Success && actual_type == XA_CARDINAL && actual_format == 8)
    {
      monitor->x_atom_length = nitems;
      monitor->x_atom_data = prop;
    }

    iter = g_list_next(iter);
  }


#ifdef HAVE_COLORD
  // and also the one from colord
  iter = g_list_first(monitor_list);
  CdClient *client = cd_client_new();
  if(!client || !cd_client_connect_sync(client, NULL, NULL))
  {
    fprintf(stderr, "error connecting to colord\n");
  }
  else
    while(iter)
    {
      monitor_t *monitor = (monitor_t *)iter->data;

      CdDevice *device = cd_client_find_device_by_property_sync(client, CD_DEVICE_METADATA_XRANDR_NAME,
                                                                monitor->name, NULL, NULL);
      if(device && cd_device_connect_sync(device, NULL, NULL))
      {
        CdProfile *profile = cd_device_get_default_profile(device);
        if(profile)
        {
          if(cd_profile_connect_sync(profile, NULL, NULL))
          {
            CdIcc *icc = cd_profile_load_icc(profile, CD_ICC_LOAD_FLAGS_FALLBACK_MD5, NULL, NULL);
            if(icc)
            {
              monitor->colord_filename = g_strdup(cd_icc_get_filename(icc));
              g_object_unref(icc);
            }
          }
          g_object_unref(profile);
        }
      }
      if(device) g_object_unref(device);
      iter = g_list_next(iter);
    }
  if(client) g_object_unref(client);
#endif // HAVE_COLORD


  // check if they are the same and print out some metadata like name, filename, filesize, ...
  gboolean any_profile_mismatch = FALSE, any_unprofiled_monitor = FALSE;
  iter = g_list_first(monitor_list);
  while(iter)
  {
    monitor_t *monitor = (monitor_t *)iter->data;
    char *message = NULL;

    char *monitor_name = monitor->name ? monitor->name : "(unknown)";
    char *x_atom_name = monitor->x_atom_name ? monitor->x_atom_name : "(not found)";
    char *tmp = get_profile_description(monitor->x_atom_data, monitor->x_atom_length);
    char *x_atom_description = tmp ? tmp : g_strdup("(none)");

#ifndef HAVE_COLORD
    if(monitor->x_atom_length == 0)
    {
      message = "the X atom seems to be missing";
      any_unprofiled_monitor = TRUE;
    }
#else // HAVE_COLORD
    char *colord_filename = monitor->colord_filename ? monitor->colord_filename : "(none)",
         *colord_description;
    if(monitor->colord_filename == NULL
       || g_file_test(monitor->colord_filename, G_FILE_TEST_IS_REGULAR) == FALSE)
    {
      colord_description = g_strdup("(file not found)");
      if(monitor->x_atom_length > 0)
      {
        any_profile_mismatch = TRUE;
        message = "the X atom and colord returned different profiles";
      }
      else
      {
        any_unprofiled_monitor = TRUE;
        message = "the X atom and colord returned the same profile";
      }
    }
    else
    {
      unsigned char *tmp_data = NULL;
      size_t size = 0;
      g_file_get_contents(monitor->colord_filename, (gchar **)&tmp_data, &size, NULL);
      gboolean profiles_equal = (size == monitor->x_atom_length
                                 && (size == 0 || memcmp(monitor->x_atom_data, tmp_data, size) == 0));
      if(!profiles_equal) any_profile_mismatch = TRUE;
      if(size == 0 && monitor->x_atom_length == 0) any_unprofiled_monitor = TRUE;
      message = profiles_equal ? "the X atom and colord returned the same profile"
                               : "the X atom and colord returned different profiles";
      char *tmp = get_profile_description(tmp_data, size);
      colord_description = tmp ? tmp : g_strdup("(none)");
      g_free(tmp_data);
    }
#endif


    // print it
    printf("\n%s", monitor_name);
    if(message) printf("\t%s", message);
    printf("\n\tX atom:\t%s (%ld bytes)\n\t\tdescription: %s\n", x_atom_name, monitor->x_atom_length,
           x_atom_description);
#ifdef HAVE_COLORD
    printf("\tcolord:\t\"%s\"\n\t\tdescription: %s\n", colord_filename, colord_description);
#endif

    g_free(x_atom_description);
#ifdef HAVE_COLORD
    g_free(colord_description);
#endif

    iter = g_list_next(iter);
  }


  // conclusion
  if(any_profile_mismatch || any_unprofiled_monitor)
  {
    printf("\nBetter check your system setup\n");
    if(any_profile_mismatch) printf(" - some monitors reported different profiles\n");
    if(any_unprofiled_monitor) printf(" - some monitors lacked a profile\n");
    printf("You may experience inconsistent color rendition between color managed applications\n");
  }
  else
    printf("\nYour system seems to be correctly configured\n");


  // cleanup
  XCloseDisplay(dpy);
  iter = g_list_first(monitor_list);
  while(iter)
  {
    monitor_t *monitor = (monitor_t *)iter->data;
    g_free(monitor->name);
    g_free(monitor->x_atom_name);
    XFree(monitor->x_atom_data);
#ifdef HAVE_COLORD
    g_free(monitor->colord_filename);
#endif
    iter = g_list_next(iter);
  }
  g_list_free_full(monitor_list, g_free);

  return EXIT_SUCCESS;

#endif // HAVE_X11
}


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
