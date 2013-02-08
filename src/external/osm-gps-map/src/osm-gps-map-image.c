/* vim:set et sw=4 ts=4 */
/*
 * Copyright (C) 2010 John Stowers <john.stowers@gmail.com>
 *
 * This is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/**
 * SECTION:osm-gps-map-image
 * @short_description: An image shown on the map
 * @stability: Stable
 * @include: osm-gps-map.h
 *
 * #OsmGpsMapImage represents an image (a #GdkPixbuf) shown on the map
 * (osm_gps_map_image_add()) at a specific location (a #OsmGpsMapPoint).
 **/

#include <gdk-pixbuf/gdk-pixbuf.h>

#include "converter.h"
#include "osm-gps-map-track.h"
#include "osm-gps-map-image.h"

G_DEFINE_TYPE (OsmGpsMapImage, osm_gps_map_image, G_TYPE_OBJECT)

enum
{
    PROP_0,
    PROP_PIXBUF,
    PROP_X_ALIGN,
    PROP_Y_ALIGN,
    PROP_POINT,
    PROP_Z_ORDER,
};

struct _OsmGpsMapImagePrivate
{
    OsmGpsMapPoint  *pt;
    GdkPixbuf       *pixbuf;
    int             w;
    int             h;
    gfloat          xalign;
    gfloat          yalign;
    int             zorder;
};

static void
osm_gps_map_image_get_property (GObject    *object,
                                guint       property_id,
                                GValue     *value,
                                GParamSpec *pspec)
{
    OsmGpsMapImagePrivate *priv = OSM_GPS_MAP_IMAGE(object)->priv;
    switch (property_id)
    {
        case PROP_PIXBUF:
            g_value_set_object (value, priv->pixbuf);
            break;
        case PROP_X_ALIGN:
            g_value_set_float (value, priv->xalign);
            break;
        case PROP_Y_ALIGN:
            g_value_set_float (value, priv->yalign);
            break;
        case PROP_POINT:
            g_value_set_boxed (value, priv->pt);
            break;
        case PROP_Z_ORDER:
            g_value_set_int   (value, priv->zorder);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
osm_gps_map_image_set_property (GObject      *object,
                                guint         property_id,
                                const GValue *value,
                                GParamSpec   *pspec)
{
    OsmGpsMapImagePrivate *priv = OSM_GPS_MAP_IMAGE(object)->priv;
    switch (property_id)
    {
        case PROP_PIXBUF:
            if (priv->pixbuf)
                g_object_unref (priv->pixbuf);
            priv->pixbuf = g_value_dup_object (value);
            priv->w = gdk_pixbuf_get_width(priv->pixbuf);
            priv->h = gdk_pixbuf_get_height(priv->pixbuf);
            break;
        case PROP_X_ALIGN:
            priv->xalign = g_value_get_float (value);
            break;
        case PROP_Y_ALIGN:
            priv->yalign = g_value_get_float (value);
            break;
        case PROP_POINT:
            priv->pt = g_value_dup_boxed (value);
            break;
        case PROP_Z_ORDER:
            priv->zorder = g_value_get_int (value);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
osm_gps_map_image_dispose (GObject *object)
{
    OsmGpsMapImagePrivate *priv = OSM_GPS_MAP_IMAGE(object)->priv;

    if (priv->pixbuf)
        g_object_unref (priv->pixbuf);

    G_OBJECT_CLASS (osm_gps_map_image_parent_class)->dispose (object);
}

static void
osm_gps_map_image_finalize (GObject *object)
{
    G_OBJECT_CLASS (osm_gps_map_image_parent_class)->finalize (object);
}

static void
osm_gps_map_image_class_init (OsmGpsMapImageClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (klass, sizeof (OsmGpsMapImagePrivate));

    object_class->get_property = osm_gps_map_image_get_property;
    object_class->set_property = osm_gps_map_image_set_property;
    object_class->dispose = osm_gps_map_image_dispose;
    object_class->finalize = osm_gps_map_image_finalize;

    g_object_class_install_property (object_class,
                                     PROP_PIXBUF,
                                     g_param_spec_object ("pixbuf",
                                                          "pixbuf",
                                                          "the pixbuf for this image",
                                                          GDK_TYPE_PIXBUF,
                                                          G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_X_ALIGN,
                                     g_param_spec_float ("x-align",
                                                         "x-align",
                                                         "image x-alignment",
                                                         0.0, /* minimum property value */
                                                         1.0, /* maximum property value */
                                                         0.5,
                                                         G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_Y_ALIGN,
                                     g_param_spec_float ("y-align",
                                                         "y-align",
                                                         "image y-alignment",
                                                         0.0, /* minimum property value */
                                                         1.0, /* maximum property value */
                                                         0.5,
                                                         G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_POINT,
                                     g_param_spec_boxed ("point",
                                                         "point",
                                                         "location point",
                                                         OSM_TYPE_GPS_MAP_POINT,
                                                         G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));

    g_object_class_install_property (object_class,
                                     PROP_Z_ORDER,
                                     g_param_spec_int ("z-order",
                                                       "z-order",
                                                       "image z-order",
                                                       -100, /* minimum property value */
                                                        100, /* maximum property value */
                                                          0,
                                                        G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT));
}

static void
osm_gps_map_image_init (OsmGpsMapImage *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE ((self), OSM_TYPE_GPS_MAP_IMAGE, OsmGpsMapImagePrivate);
}

OsmGpsMapImage *
osm_gps_map_image_new (void)
{
    return g_object_new (OSM_TYPE_GPS_MAP_IMAGE, NULL);
}

void
osm_gps_map_image_draw (OsmGpsMapImage *object, GdkDrawable *drawable, GdkGC *gc, GdkRectangle *rect)
{
    OsmGpsMapImagePrivate *priv;
    int xoffset, yoffset;

    g_return_if_fail (OSM_IS_GPS_MAP_IMAGE (object));
    priv = OSM_GPS_MAP_IMAGE(object)->priv;
    xoffset =  priv->xalign * priv->w;
    yoffset =  priv->yalign * priv->h;

    gdk_draw_pixbuf (
                     drawable,
                     gc,
                     priv->pixbuf,
                     0,0,
                     rect->x - xoffset,
                     rect->y - yoffset,
                     priv->w,
                     priv->h,
                     GDK_RGB_DITHER_NONE, 0, 0);
    rect->width = priv->w;
    rect->height = priv->h;
}

const OsmGpsMapPoint *
osm_gps_map_image_get_point(OsmGpsMapImage *object)
{
    g_return_val_if_fail (OSM_IS_GPS_MAP_IMAGE (object), NULL);
    return object->priv->pt;
}

gint
osm_gps_map_image_get_zorder(OsmGpsMapImage *object)
{
    g_return_val_if_fail (OSM_IS_GPS_MAP_IMAGE (object), 0);
    return object->priv->zorder;
}
