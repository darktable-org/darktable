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
 * SECTION:cd-window
 * @short_description: Additional helper classes for working with GTK
 *
 * These functions are useful when GTK is being used alongside colord and
 * are just provided for convenience.
 *
 * See also: #CdDevice
 */

#include "config.h"

#include <gio/gio.h>
#include <glib.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include <colord.h>

#include "cd-window.h"

static void	cd_window_class_init	(CdWindowClass	*klass);
static void	cd_window_init		(CdWindow	*window);
static void	cd_window_finalize	(GObject	*object);

#define CD_WINDOW_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), CD_TYPE_WINDOW, CdWindowPrivate))

/**
 * CdWindowPrivate:
 *
 * Private #CdWindow data
 **/
struct _CdWindowPrivate
{
	CdClient		*client;
	CdDevice		*device;
	CdProfile		*profile;
	gchar			*plug_name;
	GtkWidget		*widget;
	guint			 device_changed_id;
};

enum {
	SIGNAL_CHANGED,
	SIGNAL_LAST
};

enum {
	PROP_0,
	PROP_PROFILE,
	PROP_LAST
};

static guint signals [SIGNAL_LAST] = { 0 };

G_DEFINE_TYPE (CdWindow, cd_window, G_TYPE_OBJECT)

/**
 * cd_window_error_quark:
 *
 * Return value: An error quark.
 *
 * Since: 0.1.20
 **/
GQuark
cd_window_error_quark (void)
{
	static GQuark quark = 0;
	if (!quark) {
		quark = g_quark_from_static_string ("cd_window_error");
	}
	return quark;
}

/**
 * cd_window_get_last_profile:
 * @window: a #CdWindow instance.
 *
 * Gets the color profile to use for this widget.
 *
 * Return value: (transfer none): a #CdProfile
 *
 * Since: 0.1.20
 **/
CdProfile *
cd_window_get_last_profile (CdWindow *window)
{
	g_return_val_if_fail (CD_IS_WINDOW (window), NULL);
	return window->priv->profile;
}

typedef struct {
	CdWindow		*window;
	GCancellable		*cancellable;
	GSimpleAsyncResult	*res;
} CdWindowSetWidgetHelper;

/**
 * cd_window_get_profile_finish:
 * @window: a #CdWindow instance.
 * @res: the #GAsyncResult
 * @error: A #GError or %NULL
 *
 * Gets the result from the asynchronous function.
 *
 * Return value: (transfer full): a #CdProfile or %NULL
 *
 * Since: 0.1.20
 **/
CdProfile *
cd_window_get_profile_finish (CdWindow *window,
			      GAsyncResult *res,
			      GError **error)
{
	GSimpleAsyncResult *simple;

	g_return_val_if_fail (CD_IS_WINDOW (window), NULL);
	g_return_val_if_fail (G_IS_SIMPLE_ASYNC_RESULT (res), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	simple = G_SIMPLE_ASYNC_RESULT (res);
	if (g_simple_async_result_propagate_error (simple, error))
		return NULL;

	return g_object_ref (g_simple_async_result_get_op_res_gpointer (simple));
}

static void
cd_window_import_free_helper (CdWindowSetWidgetHelper *helper)
{
	if (helper->cancellable != NULL)
		g_object_unref (helper->cancellable);
	g_object_unref (helper->window);
	g_object_unref (helper->res);
	g_free (helper);
}

static void cd_window_get_profile_new_data (CdWindowSetWidgetHelper *helper);

static void
cd_window_get_profile_client_connect_cb (GObject *source,
					 GAsyncResult *res,
					 gpointer user_data)
{
	CdClient *client = CD_CLIENT (source);
	CdWindowSetWidgetHelper *helper = (CdWindowSetWidgetHelper *) user_data;
	gboolean ret;
	GError *error = NULL;

	ret = cd_client_connect_finish (client, res, &error);
	if (!ret) {
		g_simple_async_result_set_error (helper->res,
						 CD_WINDOW_ERROR,
						 CD_WINDOW_ERROR_FAILED,
						 "failed to connect to colord: %s",
						 error->message);
		g_simple_async_result_complete_in_idle (helper->res);
		cd_window_import_free_helper (helper);
		g_error_free (error);
		return;
	}
	cd_window_get_profile_new_data (helper);
}

static void
cd_window_get_profile_device_connect_cb (GObject *source,
					 GAsyncResult *res,
					 gpointer user_data)
{
	CdDevice *device = CD_DEVICE (source);
	CdWindowSetWidgetHelper *helper = (CdWindowSetWidgetHelper *) user_data;
	CdWindowPrivate *priv = helper->window->priv;
	gboolean ret;
	GError *error = NULL;

	ret = cd_device_connect_finish (device, res, &error);
	if (!ret) {
		g_simple_async_result_set_error (helper->res,
						 CD_WINDOW_ERROR,
						 CD_WINDOW_ERROR_FAILED,
						 "failed to connect to device: %s",
						 error->message);
		g_simple_async_result_complete_in_idle (helper->res);
		cd_window_import_free_helper (helper);
		g_error_free (error);
		return;
	}

	/* get the default profile for the device */
	priv->profile = cd_device_get_default_profile (priv->device);
	if (priv->profile == NULL) {
		g_simple_async_result_set_error (helper->res,
						 CD_WINDOW_ERROR,
						 CD_WINDOW_ERROR_FAILED,
						 "no default profile for device: %s",
						 priv->plug_name);
		g_simple_async_result_complete_in_idle (helper->res);
		cd_window_import_free_helper (helper);
		return;
	}

	cd_window_get_profile_new_data (helper);
}

static void
cd_window_get_profile_profile_connect_cb (GObject *source,
					  GAsyncResult *res,
					  gpointer user_data)
{
	CdProfile *profile = CD_PROFILE (source);
	CdWindowSetWidgetHelper *helper = (CdWindowSetWidgetHelper *) user_data;
	CdWindowPrivate *priv = helper->window->priv;
	const gchar *filename;
	gboolean ret;
	GError *error = NULL;

	ret = cd_profile_connect_finish (profile, res, &error);
	if (!ret) {
		g_simple_async_result_set_error (helper->res,
						 CD_WINDOW_ERROR,
						 CD_WINDOW_ERROR_FAILED,
						 "failed to connect to profile: %s",
						 error->message);
		g_simple_async_result_complete_in_idle (helper->res);
		cd_window_import_free_helper (helper);
		g_error_free (error);
		return;
	}

	/* get the filename of the profile */
	filename = cd_profile_get_filename (priv->profile);
	if (filename == NULL) {
		g_simple_async_result_set_error (helper->res,
						 CD_WINDOW_ERROR,
						 CD_WINDOW_ERROR_FAILED,
						 "profile has no physical file, must be virtual");
		g_simple_async_result_complete_in_idle (helper->res);
		cd_window_import_free_helper (helper);
		return;
	}

	/* SUCCESS! */
	g_simple_async_result_set_op_res_gpointer (helper->res,
						   g_object_ref (priv->profile),
						   (GDestroyNotify) g_object_unref);
	g_simple_async_result_complete_in_idle (helper->res);
	cd_window_import_free_helper (helper);
}

static void
cd_window_get_profile_device_find_cb (GObject *source,
				      GAsyncResult *res,
				      gpointer user_data)
{
	CdClient *client = CD_CLIENT (source);
	CdWindowSetWidgetHelper *helper = (CdWindowSetWidgetHelper *) user_data;
	CdWindowPrivate *priv = helper->window->priv;
	GError *error = NULL;

	priv->device = cd_client_find_device_by_property_finish (client,
								 res,
								 &error);
	if (priv->device == NULL) {
		g_simple_async_result_set_error (helper->res,
						 CD_WINDOW_ERROR,
						 CD_WINDOW_ERROR_FAILED,
						 "no device with that property: %s",
						 error->message);
		g_simple_async_result_complete_in_idle (helper->res);
		cd_window_import_free_helper (helper);
		g_error_free (error);
		return;
	}
	cd_window_get_profile_new_data (helper);
}

static void
cd_window_device_changed_cb (CdDevice *device, CdWindow *window)
{
	CdProfile *profile;

	/* no device set yet */
	if (window->priv->device == NULL)
		return;

	/* the same device */
	if (!cd_device_equal (device, window->priv->device))
		return;

	/* get new default profile */
	profile = cd_device_get_default_profile (window->priv->device);
	if (cd_profile_equal (profile, window->priv->profile))
		return;

	/* replace profile instance and emit if changed */
	if (window->priv->profile != NULL)
		g_object_unref (window->priv->profile);
	window->priv->profile = g_object_ref (profile);
	g_signal_emit (window, signals[SIGNAL_CHANGED], 0,
		       window->priv->profile);
}

static void
cd_window_get_profile_new_data (CdWindowSetWidgetHelper *helper)
{
	CdWindowPrivate *priv = helper->window->priv;

	/* connect to the daemon */
	if (priv->client == NULL) {
		priv->client = cd_client_new ();
		priv->device_changed_id =
			g_signal_connect (priv->client, "device-changed",
					  G_CALLBACK (cd_window_device_changed_cb),
					  helper->window);
		cd_client_connect (priv->client,
				   helper->cancellable,
				   cd_window_get_profile_client_connect_cb,
				   helper);
		goto out;
	}

	/* find the new device */
	if (priv->device == NULL && priv->plug_name != NULL) {
		cd_client_find_device_by_property (priv->client,
						   CD_DEVICE_METADATA_XRANDR_NAME,
						   priv->plug_name,
						   helper->cancellable,
						   cd_window_get_profile_device_find_cb,
						   helper);
		goto out;
	}

	/* connect to the device */
	if (priv->device != NULL && !cd_device_get_connected (priv->device)) {
		cd_device_connect (priv->device,
				   helper->cancellable,
				   cd_window_get_profile_device_connect_cb,
				   helper);
		goto out;
	}

	/* connect to the profile */
	if (priv->profile != NULL && !cd_profile_get_connected (priv->profile)) {
		cd_profile_connect (priv->profile,
				    helper->cancellable,
				    cd_window_get_profile_profile_connect_cb,
				    helper);
		goto out;
	}
out:
	return;
}

static void
cd_window_update_widget_plug_name (CdWindow *window,
				   GtkWidget *widget)
{
	CdWindowPrivate *priv = window->priv;
	const gchar *plug_name;
	GdkScreen *screen;
	GdkWindow *gdk_window;
	gint monitor_num;

	/* use the largest bounding area */
	gdk_window = gtk_widget_get_window (widget);
	screen = gdk_window_get_screen (gdk_window);

	monitor_num = gdk_screen_get_monitor_at_window (screen,
							gdk_window);
	plug_name = gdk_screen_get_monitor_plug_name (screen, monitor_num);

	/* ignoring MAP as plug_name has not changed */
	if (g_strcmp0 (plug_name, priv->plug_name) == 0)
		return;

	/* refresh data */
	g_free (priv->plug_name);
	priv->plug_name = g_strdup (plug_name);
	if (priv->device != NULL) {
		g_object_unref (priv->device);
		priv->device = NULL;
	}
	if (priv->profile != NULL) {
		g_object_unref (priv->profile);
		priv->profile = NULL;
	}
}

/**
 * cd_window_get_profile:
 * @window: a #CdWindow instance.
 * @widget: a #GtkWidget
 * @cancellable: a #GCancellable or %NULL
 * @callback: the function to run on completion
 * @user_data: the data to pass to @callback
 *
 * Gets the screen profile that should be used for the widget,
 * which corresponds to the screen output the widget most covers.
 *
 * This method should be called when the widget has mapped, i.e.
 * g_signal_connect (dialog, "map", G_CALLBACK (map_cb), priv);
 *
 * Note, the returned profile from cd_client_get_profile_for_widget_finish()
 * has already been connected to, as is ready to use.
 *
 * Since: 0.1.20
 **/
void
cd_window_get_profile (CdWindow *window,
		      GtkWidget *widget,
		      GCancellable *cancellable,
		      GAsyncReadyCallback callback,
		      gpointer user_data)
{
	CdWindowSetWidgetHelper *helper;

	g_return_if_fail (CD_IS_WINDOW (window));
	g_return_if_fail (GTK_IS_WIDGET (widget));
	g_return_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable));

	helper = g_new0 (CdWindowSetWidgetHelper, 1);
	helper->window = g_object_ref (window);
	helper->res = g_simple_async_result_new (G_OBJECT (window),
						 callback,
						 user_data,
						 cd_window_get_profile);
	if (cancellable != NULL)
		helper->cancellable = g_object_ref (cancellable);

	/* intially set the plug name */
	window->priv->widget = g_object_ref (widget);
	cd_window_update_widget_plug_name (window, widget);
	cd_window_get_profile_new_data (helper);
}

/**********************************************************************/

/*
 * cd_window_get_property:
 */
static void
cd_window_get_property (GObject *object,
			guint prop_id,
			GValue *value,
			GParamSpec *pspec)
{
	CdWindow *window = CD_WINDOW (object);

	switch (prop_id) {
	case PROP_PROFILE:
		g_value_set_object (value, window->priv->profile);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/*
 * cd_window_class_init:
 */
static void
cd_window_class_init (CdWindowClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->get_property = cd_window_get_property;
	object_class->finalize = cd_window_finalize;

	/**
	 * CdWindow:profile:
	 *
	 * The window profile.
	 *
	 * Since: 0.1.20
	 */
	g_object_class_install_property (object_class,
					 PROP_PROFILE,
					 g_param_spec_string ("Profile",
							      "Color profile",
							      NULL,
							      NULL,
							      G_PARAM_READABLE));

	/**
	 * CdWindow::changed:
	 * @window: the #CdDevice instance that emitted the signal
	 *
	 * The ::changed signal is emitted when the device profile has
	 * changed. The #CdProfile that is referenced in the callback
	 * has not been connected to, and you will need to call
	 * cd_profile_connect() if the ICC filename is required.
	 *
	 * Since: 0.1.20
	 **/
	signals [SIGNAL_CHANGED] =
		g_signal_new ("changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (CdWindowClass, changed),
			      NULL, NULL, g_cclosure_marshal_VOID__OBJECT,
			      G_TYPE_NONE, 1, CD_TYPE_PROFILE);

	g_type_class_add_private (klass, sizeof (CdWindowPrivate));
}

/*
 * cd_window_init:
 */
static void
cd_window_init (CdWindow *window)
{
	window->priv = CD_WINDOW_GET_PRIVATE (window);

	/* ensure the remote errors are registered */
	cd_window_error_quark ();
}

/*
 * cd_window_finalize:
 */
static void
cd_window_finalize (GObject *object)
{
	CdWindow *window = CD_WINDOW (object);

	g_return_if_fail (CD_IS_WINDOW (object));

	if (window->priv->client != NULL) {
		g_signal_handler_disconnect (window->priv->client,
					     window->priv->device_changed_id);
		g_object_unref (window->priv->client);
	}
	if (window->priv->device != NULL)
		g_object_unref (window->priv->device);
	if (window->priv->profile != NULL)
		g_object_unref (window->priv->profile);
	g_free (window->priv->plug_name);

	G_OBJECT_CLASS (cd_window_parent_class)->finalize (object);
}

/**
 * cd_window_new:
 *
 * Creates a new #CdWindow object.
 *
 * Return value: a new CdWindow object.
 *
 * Since: 0.1.20
 **/
CdWindow *
cd_window_new (void)
{
	CdWindow *window;
	window = g_object_new (CD_TYPE_WINDOW, NULL);
	return CD_WINDOW (window);
}
