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

#if !defined (__COLORD_GTK_H_INSIDE__) && !defined (CD_COMPILATION)
#error "Only <colord-gtk.h> can be included directly."
#endif

#ifndef __CD_WINDOW_H
#define __CD_WINDOW_H

#include <glib-object.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

// #include <colord/cd-profile.h>

G_BEGIN_DECLS

#define CD_TYPE_WINDOW		(cd_window_get_type ())
#define CD_WINDOW(o)		(G_TYPE_CHECK_INSTANCE_CAST ((o), CD_TYPE_WINDOW, CdWindow))
#define CD_WINDOW_CLASS(k)	(G_TYPE_CHECK_CLASS_CAST((k), CD_TYPE_WINDOW, CdWindowClass))
#define CD_IS_WINDOW(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), CD_TYPE_WINDOW))
#define CD_IS_WINDOW_CLASS(k)	(G_TYPE_CHECK_CLASS_TYPE ((k), CD_TYPE_WINDOW))
#define CD_WINDOW_GET_CLASS(o)	(G_TYPE_INSTANCE_GET_CLASS ((o), CD_TYPE_WINDOW, CdWindowClass))
#define CD_WINDOW_ERROR		(cd_window_error_quark ())
#define CD_WINDOW_TYPE_ERROR	(cd_window_error_get_type ())

typedef struct _CdWindowPrivate CdWindowPrivate;

typedef struct
{
	 GObject		 parent;
	 CdWindowPrivate	*priv;
} CdWindow;

typedef struct
{
	GObjectClass		 parent_class;
	void			(*changed)	(CdWindow	*window,
						 CdProfile	*profile);
	/*< private >*/
	/* Padding for future expansion */
	void (*_cd_window_reserved1) (void);
	void (*_cd_window_reserved2) (void);
	void (*_cd_window_reserved3) (void);
	void (*_cd_window_reserved4) (void);
	void (*_cd_window_reserved5) (void);
	void (*_cd_window_reserved6) (void);
	void (*_cd_window_reserved7) (void);
	void (*_cd_window_reserved8) (void);
} CdWindowClass;

/**
 * CdWindowError:
 * @CD_WINDOW_ERROR_FAILED: the transaction failed for an unknown reason
 *
 * Errors that can be thrown
 */
typedef enum
{
	CD_WINDOW_ERROR_FAILED,
	CD_WINDOW_ERROR_LAST
} CdWindowError;

GType		 cd_window_get_type			(void);
GQuark		 cd_window_error_quark			(void);
CdWindow	*cd_window_new				(void);

/* async */
void		cd_window_get_profile			(CdWindow	*window,
							 GtkWidget	*widget,
							 GCancellable	*cancellable,
							 GAsyncReadyCallback callback,
							 gpointer	 user_data);
CdProfile	*cd_window_get_profile_finish		(CdWindow	*window,
							 GAsyncResult	*res,
							 GError		**error);

/* getters */
CdProfile	*cd_window_get_last_profile		(CdWindow	*window);

G_END_DECLS

#endif /* __CD_WINDOW_H */

