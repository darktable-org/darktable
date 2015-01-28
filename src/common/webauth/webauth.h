// This file is part of darktable
// Copyright (c) 2015 Jose Carlos Garcia Sogo <jcsogo@gmail.com>

// darktable is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// darktable is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with darktable.  If not, see <http://www.gnu.org/licenses/>.


#ifndef __WEBAUTH_H__
#define __WEBAUTH_H__

#include "common/darktable.h"

#include <webkit/webkit.h>

typedef void (*load_cb)(WebKitWebView *web_view,
                        WebKitWebFrame *web_frame,
                        gpointer user_data);

typedef struct dt_webauth_t
{
  GtkWidget *window;
  GtkWidget *web_view;

  gint result;

} dt_webauth_t;

/* Initalize a new webauth */
dt_webauth_t *dt_webauth_new();

/* Destroy a webauth */
void dt_webauth_destroy(dt_webauth_t *webauth);

void dt_webauth_load_uri_and_show(dt_webauth_t *webauth, const gchar *uri);
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
