/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4; tab-width: 4 -*- */
/* vim:set et sw=4 ts=4 */
/*
 * Copyright (C) 2013 John Stowers <john.stowers@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <math.h>
#include <cairo.h>
#include "osd-utils.h"

#include "private.h"

#include "osm-gps-map-layer.h"
#include "osm-gps-map-osd.h"

static void osm_gps_map_osd_interface_init (OsmGpsMapLayerIface *iface);

G_DEFINE_TYPE_WITH_CODE (OsmGpsMapOsd, osm_gps_map_osd, G_TYPE_OBJECT,
         G_IMPLEMENT_INTERFACE (OSM_TYPE_GPS_MAP_LAYER,
                                osm_gps_map_osd_interface_init));

enum
{
	PROP_0,
    PROP_OSD_X,
    PROP_OSD_Y,
	PROP_DPAD_RADIUS,
	PROP_SHOW_SCALE,
	PROP_SHOW_COORDINATES,
	PROP_SHOW_CROSSHAIR,
	PROP_SHOW_DPAD,
	PROP_SHOW_ZOOM,
	PROP_SHOW_GPS_IN_DPAD,
	PROP_SHOW_GPS_IN_ZOOM
};

typedef struct _OsdScale {
    cairo_surface_t *surface;
    int zoom;
    float lat;
} OsdScale_t;

typedef struct _OsdCoordinates {
    cairo_surface_t *surface;
    float lat, lon;
} OsdCoordinates_t;

typedef struct _OsdCorosshair {
    cairo_surface_t *surface;
    gboolean rendered;
} OsdCrosshair_t;

typedef struct _OsdControls {
    cairo_surface_t *surface;
    gboolean rendered;
    gint gps_enabled;
} OsdControls_t;

struct _OsmGpsMapOsdPrivate
{
    OsdScale_t          *scale;
    OsdCoordinates_t    *coordinates;
    OsdCrosshair_t      *crosshair;
    OsdControls_t       *controls;
    guint               osd_w;
    guint               osd_h;
    guint               osd_shadow;
    guint               osd_pad;
    guint               zoom_w;
    guint               zoom_h;

    /* properties */
    gint                osd_x;
    gint                osd_y;
	guint               dpad_radius;
	gboolean            show_scale;
	gboolean            show_coordinates;
	gboolean            show_crosshair;
	gboolean            show_dpad;
	gboolean            show_zoom;
	gboolean            show_gps_in_dpad;
	gboolean            show_gps_in_zoom;
};

static void                 osm_gps_map_osd_render       (OsmGpsMapLayer *osd, OsmGpsMap *map);
static void                 osm_gps_map_osd_draw         (OsmGpsMapLayer *osd, OsmGpsMap *map, cairo_t *cr);
static gboolean             osm_gps_map_osd_busy         (OsmGpsMapLayer *osd);
static gboolean             osm_gps_map_osd_button_press (OsmGpsMapLayer *osd, OsmGpsMap *map, GdkEventButton *event);

static void                 scale_render                         (OsmGpsMapOsd *self, OsmGpsMap *map);
static void                 scale_draw                           (OsmGpsMapOsd *self, GtkAllocation *allocation, cairo_t *cr);
static void                 coordinates_render                   (OsmGpsMapOsd *self, OsmGpsMap *map);
static void                 coordinates_draw                     (OsmGpsMapOsd *self, GtkAllocation *allocation, cairo_t *cr);
static void                 crosshair_render                     (OsmGpsMapOsd *self, OsmGpsMap *map);
static void                 crosshair_draw                       (OsmGpsMapOsd *self, GtkAllocation *allocation, cairo_t *cr);
static void                 controls_render                      (OsmGpsMapOsd *self, OsmGpsMap *map);
static void                 controls_draw                        (OsmGpsMapOsd *self, GtkAllocation *allocation, cairo_t *cr);

#define OSD_MAX_SHADOW (4)

#define OSD_SCALE_FONT_SIZE (12.0)
#define OSD_SCALE_W   (10*OSD_SCALE_FONT_SIZE)
#define OSD_SCALE_H   (5*OSD_SCALE_FONT_SIZE/2)

#define OSD_SCALE_H2   (OSD_SCALE_H/2)
#define OSD_SCALE_TICK (2*OSD_SCALE_FONT_SIZE/3)
#define OSD_SCALE_M    (OSD_SCALE_H2 - OSD_SCALE_TICK)
#define OSD_SCALE_I    (OSD_SCALE_H2 + OSD_SCALE_TICK)
#define OSD_SCALE_FD   (OSD_SCALE_FONT_SIZE/4)

#define OSD_COORDINATES_FONT_SIZE (12.0)
#define OSD_COORDINATES_OFFSET (OSD_COORDINATES_FONT_SIZE/6)
#define OSD_COORDINATES_W  (8*OSD_COORDINATES_FONT_SIZE+2*OSD_COORDINATES_OFFSET)
#define OSD_COORDINATES_H  (2*OSD_COORDINATES_FONT_SIZE+2*OSD_COORDINATES_OFFSET+OSD_COORDINATES_FONT_SIZE/4)

#define OSD_CROSSHAIR_RADIUS 10
#define OSD_CROSSHAIR_TICK  (OSD_CROSSHAIR_RADIUS/2)
#define OSD_CROSSHAIR_BORDER (OSD_CROSSHAIR_TICK + OSD_CROSSHAIR_RADIUS/4)
#define OSD_CROSSHAIR_W  ((OSD_CROSSHAIR_RADIUS+OSD_CROSSHAIR_BORDER)*2)
#define OSD_CROSSHAIR_H  ((OSD_CROSSHAIR_RADIUS+OSD_CROSSHAIR_BORDER)*2)

static void
osm_gps_map_osd_interface_init (OsmGpsMapLayerIface *iface)
{
    iface->render = osm_gps_map_osd_render;
    iface->draw = osm_gps_map_osd_draw;
    iface->busy = osm_gps_map_osd_busy;
    iface->button_press = osm_gps_map_osd_button_press;
}

static void
osm_gps_map_osd_get_property (GObject    *object,
                                      guint       property_id,
                                      GValue     *value,
                                      GParamSpec *pspec)
{
	switch (property_id) {
	case PROP_OSD_X:
		g_value_set_int (value, OSM_GPS_MAP_OSD (object)->priv->osd_x);
		break;
	case PROP_OSD_Y:
		g_value_set_int (value, OSM_GPS_MAP_OSD (object)->priv->osd_y);
		break;
	case PROP_DPAD_RADIUS:
		g_value_set_uint (value, OSM_GPS_MAP_OSD (object)->priv->dpad_radius);
		break;
	case PROP_SHOW_SCALE:
		g_value_set_boolean (value, OSM_GPS_MAP_OSD (object)->priv->show_scale);
		break;
	case PROP_SHOW_COORDINATES:
		g_value_set_boolean (value, OSM_GPS_MAP_OSD (object)->priv->show_coordinates);
		break;
	case PROP_SHOW_CROSSHAIR:
		g_value_set_boolean (value, OSM_GPS_MAP_OSD (object)->priv->show_crosshair);
		break;
	case PROP_SHOW_DPAD:
		g_value_set_boolean (value, OSM_GPS_MAP_OSD (object)->priv->show_dpad);
		break;
	case PROP_SHOW_ZOOM:
		g_value_set_boolean (value, OSM_GPS_MAP_OSD (object)->priv->show_zoom);
		break;
	case PROP_SHOW_GPS_IN_DPAD:
		g_value_set_boolean (value, OSM_GPS_MAP_OSD (object)->priv->show_gps_in_dpad);
		break;
	case PROP_SHOW_GPS_IN_ZOOM:
		g_value_set_boolean (value, OSM_GPS_MAP_OSD (object)->priv->show_gps_in_zoom);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
	}
}

static void
osm_gps_map_osd_set_property (GObject      *object,
                                      guint         property_id,
                                      const GValue *value,
                                      GParamSpec   *pspec)
{
	switch (property_id) {
	case PROP_OSD_X:
		OSM_GPS_MAP_OSD (object)->priv->osd_x = g_value_get_int (value);
		break;
	case PROP_OSD_Y:
		OSM_GPS_MAP_OSD (object)->priv->osd_y = g_value_get_int (value);
		break;
	case PROP_DPAD_RADIUS:
		OSM_GPS_MAP_OSD (object)->priv->dpad_radius = g_value_get_uint (value);
		break;
	case PROP_SHOW_SCALE:
		OSM_GPS_MAP_OSD (object)->priv->show_scale = g_value_get_boolean (value);
		break;
	case PROP_SHOW_COORDINATES:
		OSM_GPS_MAP_OSD (object)->priv->show_coordinates = g_value_get_boolean (value);
		break;
	case PROP_SHOW_CROSSHAIR:
		OSM_GPS_MAP_OSD (object)->priv->show_crosshair = g_value_get_boolean (value);
		break;
	case PROP_SHOW_DPAD:
		OSM_GPS_MAP_OSD (object)->priv->show_dpad = g_value_get_boolean (value);
		break;
	case PROP_SHOW_ZOOM:
		OSM_GPS_MAP_OSD (object)->priv->show_zoom = g_value_get_boolean (value);
		break;
	case PROP_SHOW_GPS_IN_DPAD:
		OSM_GPS_MAP_OSD (object)->priv->show_gps_in_dpad = g_value_get_boolean (value);
		break;
	case PROP_SHOW_GPS_IN_ZOOM:
		OSM_GPS_MAP_OSD (object)->priv->show_gps_in_zoom = g_value_get_boolean (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
	}
}

static GObject *
osm_gps_map_osd_constructor (GType gtype, guint n_properties, GObjectConstructParam *properties)
{
    GObject *object;
    OsmGpsMapOsdPrivate *priv;

    /* Always chain up to the parent constructor */
    object = G_OBJECT_CLASS(osm_gps_map_osd_parent_class)->constructor(gtype, n_properties, properties);
    priv = OSM_GPS_MAP_OSD(object)->priv;

    /* shadow also depends on control size */
    priv->osd_shadow = MAX(priv->dpad_radius/8, OSD_MAX_SHADOW);

    /* distance between dpad and zoom */
    priv->osd_pad = priv->dpad_radius/4;

    /* size of zoom pad is wrt. the dpad size */
    priv->zoom_w = 2*priv->dpad_radius;
    priv->zoom_h = priv->dpad_radius;

    /* total width and height of controls incl. shadow */
    priv->osd_w = 2*priv->dpad_radius + priv->osd_shadow + priv->zoom_w;
    priv->osd_h = 2*priv->dpad_radius + priv->osd_pad + priv->zoom_h + 2*priv->osd_shadow;

    priv->scale = g_new0(OsdScale_t, 1);
    priv->scale->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, OSD_SCALE_W, OSD_SCALE_H);
    priv->scale->zoom = -1;
    priv->scale->lat = 360.0; /* init to an invalid lat so we get re-rendered */

    priv->coordinates = g_new0(OsdCoordinates_t, 1);
    priv->coordinates->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, OSD_COORDINATES_W, OSD_COORDINATES_H);
    priv->coordinates->lat = priv->coordinates->lon = OSM_GPS_MAP_INVALID;

    priv->crosshair = g_new0(OsdCrosshair_t, 1);
    priv->crosshair->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, OSD_CROSSHAIR_W, OSD_CROSSHAIR_H);
    priv->crosshair->rendered = FALSE;

    priv->controls = g_new0(OsdControls_t, 1);
    //FIXME: SIZE DEPENDS ON IF DPAD AND ZOOM IS THERE OR NOT
    priv->controls->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, priv->osd_w+2, priv->osd_h+2);
    priv->controls->rendered = FALSE;
    priv->controls->gps_enabled = -1;

    return object;
}

#define OSD_STRUCT_DESTROY(_x)                                  \
    if ((_x)) {                                                 \
        if ((_x)->surface)                                      \
            cairo_surface_destroy((_x)->surface);               \
        g_free((_x));                                           \
    }

static void
osm_gps_map_osd_finalize (GObject *object)
{
    OsmGpsMapOsdPrivate *priv = OSM_GPS_MAP_OSD(object)->priv;

    OSD_STRUCT_DESTROY(priv->scale)
    OSD_STRUCT_DESTROY(priv->coordinates)
    OSD_STRUCT_DESTROY(priv->crosshair)
    OSD_STRUCT_DESTROY(priv->controls)

	G_OBJECT_CLASS (osm_gps_map_osd_parent_class)->finalize (object);
}

static void
osm_gps_map_osd_class_init (OsmGpsMapOsdClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	g_type_class_add_private (klass, sizeof (OsmGpsMapOsdPrivate));

	object_class->get_property = osm_gps_map_osd_get_property;
	object_class->set_property = osm_gps_map_osd_set_property;
	object_class->constructor = osm_gps_map_osd_constructor;
	object_class->finalize     = osm_gps_map_osd_finalize;

	/**
	 * OsmGpsMapOsd:osd-x:
	 *
	 * The osd x property.
	 */
	g_object_class_install_property (object_class,
	                                 PROP_OSD_X,
	                                 g_param_spec_int ("osd-x",
	                                                     "osd-x",
	                                                     "osd-x",
	                                                     G_MININT,
	                                                     G_MAXINT,
	                                                     10,
	                                                     G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	/**
	 * OsmGpsMapOsd:osd-y:
	 *
	 * The osd y property.
	 */
	g_object_class_install_property (object_class,
	                                 PROP_OSD_Y,
	                                 g_param_spec_int ("osd-y",
	                                                     "osd-y",
	                                                     "osd-y",
	                                                     G_MININT,
	                                                     G_MAXINT,
	                                                     10,
	                                                     G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	/**
	 * OsmGpsMapOsd:dpad-radius:
	 *
	 * The dpad radius property.
	 */
	g_object_class_install_property (object_class,
	                                 PROP_DPAD_RADIUS,
	                                 g_param_spec_uint ("dpad-radius",
	                                                     "dpad-radius",
	                                                     "dpad radius",
	                                                     0,
	                                                     G_MAXUINT,
	                                                     30,
	                                                     G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	/**
	 * OsmGpsMapOsd:show-scale:
	 *
	 * The show scale on the map property.
	 */
	g_object_class_install_property (object_class,
	                                 PROP_SHOW_SCALE,
	                                 g_param_spec_boolean ("show-scale",
	                                                       "show-scale",
	                                                       "show scale on the map",
	                                                       TRUE,
	                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	/**
	 * OsmGpsMapOsd:show-coordinates:
	 *
	 * The show coordinates of map centre property.
	 */
	g_object_class_install_property (object_class,
	                                 PROP_SHOW_COORDINATES,
	                                 g_param_spec_boolean ("show-coordinates",
	                                                       "show-coordinates",
	                                                       "show coordinates of map centre",
	                                                       TRUE,
	                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	/**
	 * OsmGpsMapOsd:show-crosshair:
	 *
	 * The show crosshair at map centre property.
	 */
	g_object_class_install_property (object_class,
	                                 PROP_SHOW_CROSSHAIR,
	                                 g_param_spec_boolean ("show-crosshair",
	                                                       "show-crosshair",
	                                                       "show crosshair at map centre",
	                                                       FALSE,
	                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	/**
	 * OsmGpsMapOsd:show-dpad:
	 *
	 * The show dpad for map navigation property.
	 */
	g_object_class_install_property (object_class,
	                                 PROP_SHOW_DPAD,
	                                 g_param_spec_boolean ("show-dpad",
	                                                       "show-dpad",
	                                                       "show dpad for map navigation",
	                                                       FALSE,
	                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	/**
	 * OsmGpsMapOsd:show-zoom:
	 *
	 * The show zoom control for map navigation property.
	 */
	g_object_class_install_property (object_class,
	                                 PROP_SHOW_ZOOM,
	                                 g_param_spec_boolean ("show-zoom",
	                                                       "show-zoom",
	                                                       "show zoom control for map navigation",
	                                                       FALSE,
	                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	/**
	 * OsmGpsMapOsd:show-gps-in-dpad:
	 *
	 * The show gps indicator in middle of dpad property.
	 */
	g_object_class_install_property (object_class,
	                                 PROP_SHOW_GPS_IN_DPAD,
	                                 g_param_spec_boolean ("show-gps-in-dpad",
	                                                       "show-gps-in-dpad",
	                                                       "show gps indicator in middle of dpad",
	                                                       FALSE,
	                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
	/**
	 * OsmGpsMapOsd:show-gps-in-zoom:
	 *
	 * The show gps indicator in middle of zoom control property.
	 */
	g_object_class_install_property (object_class,
	                                 PROP_SHOW_GPS_IN_ZOOM,
	                                 g_param_spec_boolean ("show-gps-in-zoom",
	                                                       "show-gps-in-zoom",
	                                                       "show gps indicator in middle of zoom control",
	                                                       FALSE,
	                                                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT));
}

static void
osm_gps_map_osd_init (OsmGpsMapOsd *self)
{
	self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
	                                          OSM_TYPE_GPS_MAP_OSD,
	                                          OsmGpsMapOsdPrivate);
}

static void
osm_gps_map_osd_render (OsmGpsMapLayer *osd,
                                OsmGpsMap *map)
{
    OsmGpsMapOsd *self;
    OsmGpsMapOsdPrivate *priv;

    g_return_if_fail(OSM_IS_GPS_MAP_OSD(osd));

    self = OSM_GPS_MAP_OSD(osd);
    priv = self->priv;

    if (priv->show_scale)
        scale_render(self, map);
    if (priv->show_coordinates)
        coordinates_render(self, map);
    if (priv->show_crosshair)
        crosshair_render(self, map);
    if (priv->show_zoom || priv->show_dpad)
        controls_render(self, map);

}

static void
osm_gps_map_osd_draw (OsmGpsMapLayer *osd,
                              OsmGpsMap *map,
                              cairo_t *cr)
{
    OsmGpsMapOsd *self;
    OsmGpsMapOsdPrivate *priv;
    GtkAllocation allocation;

    g_return_if_fail(OSM_IS_GPS_MAP_OSD(osd));

    self = OSM_GPS_MAP_OSD(osd);
    priv = self->priv;

    gtk_widget_get_allocation(GTK_WIDGET(map), &allocation);

    if (priv->show_scale)
        scale_draw(self, &allocation, cr);
    if (priv->show_coordinates)
        coordinates_draw(self, &allocation, cr);
    if (priv->show_crosshair)
        crosshair_draw(self, &allocation, cr);
    if (priv->show_zoom || priv->show_dpad)
        controls_draw(self, &allocation, cr);

}

static gboolean
osm_gps_map_osd_busy (OsmGpsMapLayer *osd)
{
	return FALSE;
}

static gboolean
osm_gps_map_osd_button_press (OsmGpsMapLayer *osd,
                                      OsmGpsMap *map,
                                      GdkEventButton *event)
{
    gboolean handled = FALSE;
    OsdControlPress_t but = OSD_NONE;
    OsmGpsMapOsd *self;
    OsmGpsMapOsdPrivate *priv;
    GtkAllocation allocation;

    g_return_val_if_fail(OSM_IS_GPS_MAP_OSD(osd), FALSE);

    self = OSM_GPS_MAP_OSD(osd);
    priv = self->priv;
    gtk_widget_get_allocation(GTK_WIDGET(map), &allocation);

    if ((event->button == 1) && (event->type == GDK_BUTTON_PRESS)) {
        gint mx = event->x - priv->osd_x;
        gint my = event->y - priv->osd_y;

        if(priv->osd_x < 0)
            mx -= (allocation.width - priv->osd_w);
    
        if(priv->osd_y < 0)
            my -= (allocation.height - priv->osd_h);

        /* first do a rough test for the OSD area. */
        /* this is just to avoid an unnecessary detailed test */
        if(mx > 0 && mx < priv->osd_w && my > 0 && my < priv->osd_h) {
            if (priv->show_dpad) {
                but = osd_check_dpad(mx, my, priv->dpad_radius, priv->show_gps_in_dpad);
                my -= (2*priv->dpad_radius);
                my -= priv->osd_pad;
            }
            if (but == OSD_NONE && priv->show_zoom)
                but = osd_check_zoom(mx, my, priv->zoom_w, priv->zoom_h, 0 /*show gps*/);
        }
    }

    switch (but) {
        case OSD_LEFT:
            osm_gps_map_scroll(map, -5, 0);
            handled = TRUE;
            break;
        case OSD_RIGHT:
            osm_gps_map_scroll(map, 5, 0);
            handled = TRUE;
            break;
        case OSD_UP:
            osm_gps_map_scroll(map, 0, -5);
            handled = TRUE;
            break;
        case OSD_DOWN:
            osm_gps_map_scroll(map, 0, 5);
            handled = TRUE;
            break;
        case OSD_OUT:
            osm_gps_map_zoom_out(map);
            handled = TRUE;
            break;
        case OSD_IN:
            osm_gps_map_zoom_in(map);
            handled = TRUE;
            break;
        case OSD_NONE:
        case OSD_GPS:
        default:
            handled = FALSE;
            break;
    }

    return handled;
}

/**
 * osm_gps_map_osd_new:
 *
 * Creates a new instance of #OsmGpsMapOsd.
 *
 * Return value: the newly created #OsmGpsMapOsd instance
 */
OsmGpsMapOsd*
osm_gps_map_osd_new (void)
{
	return g_object_new (OSM_TYPE_GPS_MAP_OSD, NULL);
}

static void
scale_render(OsmGpsMapOsd *self, OsmGpsMap *map)
{
    OsdScale_t *scale = self->priv->scale;

    if(!scale->surface)
        return;

    /* this only needs to be rendered if the zoom or latitude has changed */
    gint zoom;
    gfloat lat;
    g_object_get(G_OBJECT(map), "zoom", &zoom, "latitude", &lat, NULL);
    if(zoom == scale->zoom && lat == scale->lat)
        return;

    scale->zoom = zoom;
    scale->lat = lat;

    float m_per_pix = osm_gps_map_get_scale(map);

    /* first fill with transparency */
    g_assert(scale->surface);
    cairo_t *cr = cairo_create(scale->surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 0.0);
    // pink for testing:    cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 0.2);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* determine the size of the scale width in meters */
    float width = (OSD_SCALE_W-OSD_SCALE_FONT_SIZE/6) * m_per_pix;

    /* scale this to useful values */
    int exp = logf(width)*M_LOG10E;
    int mant = width/pow(10,exp);
    int width_metric = mant * pow(10,exp);
    char *dist_str = NULL;
    if(width_metric<1000)
        dist_str = g_strdup_printf("%u m", width_metric);
    else
        dist_str = g_strdup_printf("%u km", width_metric/1000);
    width_metric /= m_per_pix;

    /* and now the hard part: scale for useful imperial values :-( */
    /* try to convert to feet, 1ft == 0.3048 m */
    width /= 0.3048;
    float imp_scale = 0.3048;
    char *dist_imp_unit = "ft";

    if(width >= 100) {
        /* 1yd == 3 feet */
        width /= 3.0;
        imp_scale *= 3.0;
        dist_imp_unit = "yd";

        if(width >= 1760.0) {
            /* 1mi == 1760 yd */
            width /= 1760.0;
            imp_scale *= 1760.0;
            dist_imp_unit = "mi";
        }
    }

    /* also convert this to full tens/hundreds */
    exp = logf(width)*M_LOG10E;
    mant = width/pow(10,exp);
    int width_imp = mant * pow(10,exp);
    char *dist_str_imp = g_strdup_printf("%u %s", width_imp, dist_imp_unit);

    /* convert back to pixels */
    width_imp *= imp_scale;
    width_imp /= m_per_pix;

    cairo_select_font_face (cr, "Sans",
                            CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size (cr, OSD_SCALE_FONT_SIZE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);

    cairo_text_extents_t extents;
    cairo_text_extents (cr, dist_str, &extents);

    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_set_line_width (cr, OSD_SCALE_FONT_SIZE/6);
    cairo_move_to (cr, 2*OSD_SCALE_FD, OSD_SCALE_H2-OSD_SCALE_FD);
    cairo_text_path (cr, dist_str);
    cairo_stroke (cr);
    cairo_move_to (cr, 2*OSD_SCALE_FD,
                   OSD_SCALE_H2+OSD_SCALE_FD + extents.height);
    cairo_text_path (cr, dist_str_imp);
    cairo_stroke (cr);

    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_move_to (cr, 2*OSD_SCALE_FD, OSD_SCALE_H2-OSD_SCALE_FD);
    cairo_show_text (cr, dist_str);
    cairo_move_to (cr, 2*OSD_SCALE_FD,
                   OSD_SCALE_H2+OSD_SCALE_FD + extents.height);
    cairo_show_text (cr, dist_str_imp);

    g_free(dist_str);
    g_free(dist_str_imp);

    /* draw white line */
    cairo_set_line_cap  (cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    cairo_set_line_width (cr, OSD_SCALE_FONT_SIZE/3);
    cairo_move_to (cr, OSD_SCALE_FONT_SIZE/6, OSD_SCALE_M);
    cairo_rel_line_to (cr, 0,  OSD_SCALE_TICK);
    cairo_rel_line_to (cr, width_metric, 0);
    cairo_rel_line_to (cr, 0, -OSD_SCALE_TICK);
    cairo_stroke(cr);
    cairo_move_to (cr, OSD_SCALE_FONT_SIZE/6, OSD_SCALE_I);
    cairo_rel_line_to (cr, 0, -OSD_SCALE_TICK);
    cairo_rel_line_to (cr, width_imp, 0);
    cairo_rel_line_to (cr, 0, +OSD_SCALE_TICK);
    cairo_stroke(cr);

    /* draw black line */
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 1.0);
    cairo_set_line_width (cr, OSD_SCALE_FONT_SIZE/6);
    cairo_move_to (cr, OSD_SCALE_FONT_SIZE/6, OSD_SCALE_M);
    cairo_rel_line_to (cr, 0,  OSD_SCALE_TICK);
    cairo_rel_line_to (cr, width_metric, 0);
    cairo_rel_line_to (cr, 0, -OSD_SCALE_TICK);
    cairo_stroke(cr);
    cairo_move_to (cr, OSD_SCALE_FONT_SIZE/6, OSD_SCALE_I);
    cairo_rel_line_to (cr, 0, -OSD_SCALE_TICK);
    cairo_rel_line_to (cr, width_imp, 0);
    cairo_rel_line_to (cr, 0, +OSD_SCALE_TICK);
    cairo_stroke(cr);

    cairo_destroy(cr);
}

static void
scale_draw(OsmGpsMapOsd *self, GtkAllocation *allocation, cairo_t *cr)
{
    OsmGpsMapOsdPrivate *priv = self->priv;
    OsdScale_t *scale = self->priv->scale;

    gint x, y;

    x =  priv->osd_x;
    y = -priv->osd_y;
    if(x < 0) x += allocation->width - OSD_SCALE_W;
    if(y < 0) y += allocation->height - OSD_SCALE_H;

    cairo_set_source_surface(cr, scale->surface, x, y);
    cairo_paint(cr);
}

static void
coordinates_render(OsmGpsMapOsd *self, OsmGpsMap *map)
{
    OsdCoordinates_t *coordinates = self->priv->coordinates;

    if(!coordinates->surface)
        return;

    /* get current map position */
    gfloat lat, lon;
    g_object_get(G_OBJECT(map), "latitude", &lat, "longitude", &lon, NULL);

    /* check if position has changed enough to require redraw */
    if(!isnan(coordinates->lat) && !isnan(coordinates->lon))
        /* 1/60000 == 1/1000 minute */
        if((fabsf(lat - coordinates->lat) < 1/60000) &&
           (fabsf(lon - coordinates->lon) < 1/60000))
            return;

    coordinates->lat = lat;
    coordinates->lon = lon;

    /* first fill with transparency */

    g_assert(coordinates->surface);
    cairo_t *cr = cairo_create(coordinates->surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    //    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_select_font_face (cr, "Sans",
                            CAIRO_FONT_SLANT_NORMAL,
                            CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size (cr, OSD_COORDINATES_FONT_SIZE);

    char *latitude = osd_latitude_str(lat);
    char *longitude = osd_longitude_str(lon);
    
    int y = OSD_COORDINATES_OFFSET;
    y = osd_render_centered_text(cr, y, OSD_COORDINATES_W, OSD_COORDINATES_FONT_SIZE, latitude);
    y = osd_render_centered_text(cr, y, OSD_COORDINATES_W, OSD_COORDINATES_FONT_SIZE, longitude);
    
    g_free(latitude);
    g_free(longitude);

    cairo_destroy(cr);
}

static void
coordinates_draw(OsmGpsMapOsd *self, GtkAllocation *allocation, cairo_t *cr)
{
    OsmGpsMapOsdPrivate *priv = self->priv;
    OsdCoordinates_t *coordinates = self->priv->coordinates;
    gint x,y;

    x = -priv->osd_x;
    y = -priv->osd_y;
    if(x < 0) x += allocation->width - OSD_COORDINATES_W;
    if(y < 0) y += allocation->height - OSD_COORDINATES_H;

    cairo_set_source_surface(cr, coordinates->surface, x, y);
    cairo_paint(cr);
}


static void
crosshair_render(OsmGpsMapOsd *self, OsmGpsMap *map)
{
    OsdCrosshair_t *crosshair = self->priv->crosshair;

    if(!crosshair->surface || crosshair->rendered)
        return;

    crosshair->rendered = TRUE;

    /* first fill with transparency */
    g_assert(crosshair->surface);
    cairo_t *cr = cairo_create(crosshair->surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_set_line_cap  (cr, CAIRO_LINE_CAP_ROUND);

    cairo_set_source_rgba (cr, 1.0, 1.0, 1.0, 0.5);
    cairo_set_line_width (cr, OSD_CROSSHAIR_RADIUS/2);
    osd_render_crosshair_shape(cr, OSD_CROSSHAIR_W, OSD_CROSSHAIR_H, OSD_CROSSHAIR_RADIUS, OSD_CROSSHAIR_TICK);

    cairo_set_source_rgba (cr, 0.0, 0.0, 0.0, 0.5);
    cairo_set_line_width (cr, OSD_CROSSHAIR_RADIUS/4);
    osd_render_crosshair_shape(cr, OSD_CROSSHAIR_W, OSD_CROSSHAIR_H, OSD_CROSSHAIR_RADIUS, OSD_CROSSHAIR_TICK);

    cairo_destroy(cr);
}


static void
crosshair_draw(OsmGpsMapOsd *self, GtkAllocation *allocation, cairo_t *cr)
{
    OsdCrosshair_t *crosshair = self->priv->crosshair;
    gint x,y;

    x = (allocation->width - OSD_CROSSHAIR_W)/2;
    y = (allocation->height - OSD_CROSSHAIR_H)/2;

    cairo_set_source_surface(cr, crosshair->surface, x, y);
    cairo_paint(cr);
}

static void
controls_render(OsmGpsMapOsd *self, OsmGpsMap *map)
{
    GdkRGBA fg, bg;
    OsmGpsMapOsdPrivate *priv = self->priv;
    OsdControls_t *controls = self->priv->controls;

    if(!controls->surface || controls->rendered)
        return;

    controls->rendered = TRUE;

    gdk_rgba_parse (&fg, "black");
    gdk_rgba_parse (&bg, "grey80");

    /* first fill with transparency */
    g_assert(controls->surface);
    cairo_t *cr = cairo_create(controls->surface);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 1.0, 0.0, 0.0, 0.0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    gint x = 1;
    gint y = 1;

    /* --------- draw dpad ----------- */
    if (priv->show_dpad) {
        gint gps_w = (priv->show_gps_in_dpad ? priv->dpad_radius/2 : 0);
        osd_render_dpad(cr, x, y, priv->dpad_radius, gps_w, priv->osd_shadow, &bg, &fg);
        if (priv->show_gps_in_dpad) {
            gint gps_x = x+priv->dpad_radius-(gps_w/2);
            gint gps_y = y+priv->dpad_radius-(gps_w/2);
            osd_render_gps(cr, gps_x, gps_y, gps_w, &bg, &fg);
        }
        y += (2*priv->dpad_radius);
        y += priv->osd_pad;
    }

    /* --------- draw zoom ----------- */
    if (priv->show_zoom) {
        gint gps_w = (priv->show_gps_in_zoom ? priv->dpad_radius/2 : 0);
        osd_render_zoom(cr, x, y, priv->zoom_w, priv->zoom_h, gps_w, priv->osd_shadow, &bg, &fg);
        if (priv->show_gps_in_zoom) {
            gint gps_x = x+(priv->zoom_w/2);
            gint gps_y = y+(priv->zoom_h/2)-(gps_w/2);
            osd_render_gps(cr, gps_x, gps_y, gps_w, &bg, &fg);
        }
        y += priv->zoom_h;
    }

}

static void
controls_draw(OsmGpsMapOsd *self, GtkAllocation *allocation, cairo_t *cr)
{
    OsmGpsMapOsdPrivate *priv = self->priv;
    OsdControls_t *controls = self->priv->controls;

    gint x,y;

    x = priv->osd_x;
    if(x < 0)
        x += allocation->width - priv->osd_w;

    y = priv->osd_y;
    if(y < 0)
        y += allocation->height - priv->osd_h;

    cairo_set_source_surface(cr, controls->surface, x, y);
    cairo_paint(cr);
}

