/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2012 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU Lesser General Public License Version 2.1
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

/**
 * SECTION:cd-window-sync
 * @short_description: Sync helpers for #CdWindow
 *
 * These helper functions provide a simple way to use the async functions
 * in command line tools.
 *
 * See also: #CdWindow
 */

#include "config.h"

#include <glib.h>
#include <gio/gio.h>

#include <colord.h>

#include "cd-window.h"
#include "cd-window-sync.h"

/* tiny helper to help us do the async operation */
typedef struct {
	GError		**error;
	GMainLoop	*loop;
	CdProfile	*profile;
	CdWindow	*window;
} CdWindowHelper;

/**********************************************************************/

static void
cd_window_get_profile_finish_sync (CdWindow *window,
				   GAsyncResult *res,
				   CdWindowHelper *helper)
{
	helper->profile = cd_window_get_profile_finish (window,
							res,
							helper->error);
	g_main_loop_quit (helper->loop);
}

/**
 * cd_window_get_profile_sync:
 * @window: a #CdWindow instance.
 * @widget: a #GtkWidget
 * @cancellable: a #GCancellable or %NULL
 * @error: a #GError, or %NULL.
 *
 * Gets the screen profile that should be used for the widget,
 * which corresponds to the screen output the widget most covers.
 *
 * WARNING: This function is synchronous, and may block.
 * Do not use it in GUI applications.
 *
 * Return value: (transfer full): a #CdProfile or %NULL
 *
 * Since: 0.1.20
 **/
CdProfile *
cd_window_get_profile_sync (CdWindow *window,
			    GtkWidget *widget,
			    GCancellable *cancellable,
			    GError **error)
{
	CdWindowHelper helper;

	/* create temp object */
	helper.loop = g_main_loop_new (NULL, FALSE);
	helper.error = error;

	/* run async method */
	cd_window_get_profile (window, widget, cancellable,
				(GAsyncReadyCallback) cd_window_get_profile_finish_sync,
				&helper);
	g_main_loop_run (helper.loop);

	/* free temp object */
	g_main_loop_unref (helper.loop);

	return helper.profile;
}
