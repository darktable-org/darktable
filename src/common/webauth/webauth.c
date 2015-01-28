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
// along with darktable.  If not, see <http://www.gnu.org/licenses/>

#include "common/webauth/webauth.h"
#include <webkit/webkit.h>

dt_webauth_t *dt_webauth_new()
{
  dt_webauth_t *webauth = g_malloc(sizeof(dt_webauth_t));

  GtkWidget *main_window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size(GTK_WINDOW(main_window), 800, 600);
  GtkWidget *scrolled_window = gtk_scrolled_window_new (NULL, NULL);
  GtkWidget *web_view = webkit_web_view_new ();

  gtk_container_add (GTK_CONTAINER (scrolled_window), web_view);
  gtk_container_add (GTK_CONTAINER (main_window), scrolled_window);

  webauth->window = main_window;
  webauth->web_view = web_view;

  //g_signal_connect(webView, "close", G_CALLBACK(closeWebViewCb), main_window);
  //g_signal_connect(web_view, "document-load-finished", G_CALLBACK(webauth->on_load_cb), webauth); 

  /* DEBUG */
  //dt_webauth_load_uri_and_show(webauth, "http://www.darktable.org");

  return webauth;
}

void dt_webauth_load_uri_and_show(dt_webauth_t *webauth, const gchar *uri)
{
  webkit_web_view_load_uri(WEBKIT_WEB_VIEW(webauth->web_view), uri);
  
  gtk_widget_grab_focus(webauth->web_view);
  gtk_widget_show_all(webauth->window);
}

void dt_webauth_destroy(dt_webauth_t *webauth)
{
  g_return_if_fail(webauth != NULL);
  gtk_window_close(GTK_WINDOW(webauth->window));
  g_free(webauth);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
