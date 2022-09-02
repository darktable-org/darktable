/*
    This file is part of darktable,
    Copyright (C) 2013-2021 darktable developers.

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

/*
  You can compile this tool standalone.
  Dependencies: libX11, libXrandr, liblcms2, libglib and optionally libcolord
  Compile with something like this:
    gcc -W -Wall -std=c99 `pkg-config --cflags --libs glib-2.0 lcms2 colord x11 xrandr` \
        -DHAVE_X11 -DHAVE_COLORD -Ddarktable_package_version=\"'standalone'\" main.c -o darktable-cmstest
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib-object.h>
#include <glib.h>
#include <lcms2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_X11
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#ifdef HAVE_COLORD
#include <colord.h>
#endif // HAVE_COLORD
#endif // HAVE_X11

#ifdef HAVE_X11
typedef struct monitor_t
{
  int screen;
  int crtc;
  Window root;
  int atom_id;
  char *name;

  gboolean is_primary;

  // X atom
  char *x_atom_name;
  size_t x_atom_length;
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

// sort them according to screen. then for each screen we want the screen's primary first, followed by the rest
// in the original order
static gint sort_monitor_list(gconstpointer a, gconstpointer b)
{
  monitor_t *monitor_a = (monitor_t *)a;
  monitor_t *monitor_b = (monitor_t *)b;

  if(monitor_a->screen != monitor_b->screen)
    return monitor_a->screen - monitor_b->screen;

  if(monitor_a->is_primary) return -1;
  if(monitor_b->is_primary) return 1;

  return monitor_a->atom_id - monitor_b->atom_id;
}
#endif // HAVE_X11

int main(int argc __attribute__((unused)), char *arg[] __attribute__((unused)))
{
  printf("darktable-cmstest version %s\n", darktable_package_version);
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

  printf("\n");

  // get a list of all possible screens from xrandr
  GList *monitor_list = NULL;
  const char *disp_name_env;
  char disp_name[100];

  // find the base display name
  if((disp_name_env = g_getenv("DISPLAY")) != NULL)
  {
    char *pp;
    g_strlcpy(disp_name, disp_name_env, sizeof(disp_name));
    if((pp = g_strrstr(disp_name, ":")) != NULL)
    {
      if((pp = g_strstr_len(pp, -1, ".")) == NULL)
        g_strlcat(disp_name, ".0", sizeof(disp_name));
      else  {
        if(pp[1] == '\0')
          g_strlcat(disp_name, "0", sizeof(disp_name));
        else
        {
          pp[1] = '0';
          pp[2] = '\0';
        }
      }
    }
  }
  else
    g_strlcpy(disp_name, ":0.0", sizeof(disp_name));

  Display *display = XOpenDisplay(disp_name);
  if(display == NULL)
  {
    fprintf(stderr, "can't open display `%s'\n", XDisplayName(disp_name));
    return EXIT_FAILURE;
  }

  int max_screen = ScreenCount(display);
  for(int screen = 0; screen < max_screen; ++screen)
  {
    int atom_id = 0; // the id of the x atom. might be changed when sorting in the primary later!

    Window root = RootWindow(display, screen);
    XRRScreenResources *rsrc = XRRGetScreenResources(display, root);

    // see if there is a primary screen.
    XID primary = XRRGetOutputPrimary(display, root);
    gboolean have_primary = FALSE;
    int primary_id = -1;
    if(rsrc)
    {
      for(int crtc = 0; crtc < rsrc->ncrtc; crtc++)
      {
        XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(display, rsrc, rsrc->crtcs[crtc]);
        if(!crtc_info)
          continue;

        if(crtc_info->mode != None && crtc_info->noutput > 0)
        {
          for(int output = 0; output < crtc_info->noutput; output++)
          {
            if(crtc_info->outputs[output] == primary)
            {
              primary_id = crtc;
              break;
            }
          }
        }

        XRRFreeCrtcInfo(crtc_info);
      }
    }
    if(primary_id == -1)
      printf("couldn't locate primary CRTC!\n");
    else
    {
      printf("primary CRTC is at CRTC %d\n", primary_id);
      have_primary = TRUE;
    }

    // now iterate over the CRTCs again and add the relevant ones to the list
    if(rsrc)
    {
      for(int crtc = 0; crtc < rsrc->ncrtc; ++crtc)
      {
        XRROutputInfo *output_info = NULL;
        XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(display, rsrc, rsrc->crtcs[crtc]);
        if(!crtc_info)
        {
          printf("can't get CRTC info for screen %d CRTC %d\n", screen, crtc);
          goto end;
        }
        // only handle those that are attached though
        if(crtc_info->mode == None || crtc_info->noutput <= 0)
        {
          printf("CRTC for screen %d CRTC %d has no mode or no output, skipping\n", screen, crtc);
          goto end;
        }

        // Choose the primary output of the CRTC if we have one, else default to the first. i.e. we punt with
        // mirrored displays.
        gboolean is_primary = FALSE;
        int output = 0;
        if(have_primary)
        {
          for(int j = 0; j < crtc_info->noutput; j++)
          {
            if(crtc_info->outputs[j] == primary)
            {
              output = j;
              is_primary = TRUE;
              break;
            }
          }
        }

        output_info = XRRGetOutputInfo(display, rsrc, crtc_info->outputs[output]);
        if(!output_info)
        {
          printf("can't get output info for screen %d CRTC %d output %d\n", screen, crtc, output);
          goto end;
        }

        if(output_info->connection == RR_Disconnected)
        {
          printf("screen %d CRTC %d output %d is disconnected, skipping\n", screen, crtc, output);
          goto end;
        }

        monitor_t *monitor = (monitor_t *)calloc(1, sizeof(monitor_t));
#if 0
        // in case we also want the edid data
        Atom edid_atom = XInternAtom(display, "EDID", False), actual_type;
        int actual_format;
        unsigned long nitems, bytes_after;
        unsigned char *prop;
        int res = XRRGetOutputProperty(display, rsrc->outputs[output], edid_atom, 0, G_MAXLONG, FALSE,
            FALSE, XA_INTEGER, &actual_type, &actual_format, &nitems, &bytes_after, &prop);
        if(res == Success && actual_type == XA_INTEGER && actual_format == 8 && nitems != 0)
        {
          printf("EDID for %s has size %lu\n", output_info->name, nitems);
          // TODO: parse the edid blob in prop. since that is really ugly code I left it out for now.
        }
        if(prop)
          XFree(prop);
#endif

        monitor->root = root;
        monitor->screen = screen;
        monitor->crtc = crtc;
        monitor->is_primary = is_primary;
        monitor->atom_id = atom_id++;
        monitor->name = g_strdup(output_info->name);
        monitor_list = g_list_prepend(monitor_list, monitor);

end:
        XRRFreeCrtcInfo(crtc_info);
        XRRFreeOutputInfo(output_info);
      }
    }
    XRRFreeScreenResources(rsrc);
  }

  // sort the list of monitors so that the primary one is first. also updates the atom_id.
  monitor_list = g_list_sort(monitor_list, sort_monitor_list);
  int atom_id = 0;
  int last_screen = -1;
  for(GList *iter = monitor_list; iter; iter = g_list_next(iter))
  {
    monitor_t *monitor = (monitor_t *)iter->data;
    if(monitor->screen != last_screen) atom_id = 0;
    last_screen = monitor->screen;
    monitor->atom_id = atom_id++;
  }

  // get the profile from the X atom
  for(GList *iter = monitor_list; iter; iter = g_list_next(iter))
  {
    monitor_t *monitor = (monitor_t *)iter->data;
    if(monitor->atom_id == 0)
      monitor->x_atom_name = g_strdup("_ICC_PROFILE");
    else
      monitor->x_atom_name = g_strdup_printf("_ICC_PROFILE_%d", monitor->atom_id);

    Atom atom = XInternAtom(display, monitor->x_atom_name, FALSE), actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *prop;

    int res = XGetWindowProperty(display, monitor->root, atom, 0, G_MAXLONG, FALSE, XA_CARDINAL, &actual_type,
                                 &actual_format, &nitems, &bytes_after, &prop);

    if(res == Success && actual_type == XA_CARDINAL && actual_format == 8)
    {
      monitor->x_atom_length = nitems;
      monitor->x_atom_data = prop;
    }
    else
      XFree(prop);
  }


#ifdef HAVE_COLORD
  // and also the profile from colord
  CdClient *client = cd_client_new();
  if(!client || !cd_client_connect_sync(client, NULL, NULL))
  {
    fprintf(stderr, "error connecting to colord\n");
  }
  else
    for(GList *iter = monitor_list; iter; iter = g_list_next(iter))
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
    }
  if(client) g_object_unref(client);
#endif // HAVE_COLORD


  // check if they are the same and print out some metadata like name, filename, filesize, ...
  gboolean any_profile_mismatch = FALSE, any_unprofiled_monitor = FALSE;
  for(GList *iter = monitor_list; iter; iter = g_list_next(iter))
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
      tmp = get_profile_description(tmp_data, size);
      colord_description = tmp ? tmp : g_strdup("(none)");
      g_free(tmp_data);
    }
#endif // HAVE_COLORD


    // print it
    printf("\n%s", monitor_name);
    if(message) printf("\t%s", message);
    printf("\n\tX atom:\t%s (%zu bytes)\n\t\tdescription: %s\n", x_atom_name, monitor->x_atom_length,
           x_atom_description);
#ifdef HAVE_COLORD
    printf("\tcolord:\t\"%s\"\n\t\tdescription: %s\n", colord_filename, colord_description);
#endif

    g_free(x_atom_description);
#ifdef HAVE_COLORD
    g_free(colord_description);
#endif

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
  XCloseDisplay(display);
  for(GList *iter = monitor_list; iter; iter = g_list_next(iter))
  {
    monitor_t *monitor = (monitor_t *)iter->data;
    g_free(monitor->name);
    g_free(monitor->x_atom_name);
    XFree(monitor->x_atom_data);
#ifdef HAVE_COLORD
    g_free(monitor->colord_filename);
#endif
  }
  g_list_free_full(monitor_list, g_free);

  return EXIT_SUCCESS;

#endif // HAVE_X11
}


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

