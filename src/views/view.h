/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.
    copyright (c) 2010--2014 henrik andersson.
    copyright (c) 2012 tobias ellinghaus.

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

#pragma once

#include "common/image.h"
#ifdef HAVE_PRINT
#include "common/cups_print.h"
#endif
#ifdef HAVE_MAP
#include "common/geo.h"
#include <osm-gps-map.h>
#endif
#include <cairo.h>
#include <gmodule.h>
#include <gui/gtk.h>
#include <inttypes.h>
#include <sqlite3.h>
#ifdef USE_LUA
#include "lua/call.h"
#include "lua/events.h"
#include "lua/modules.h"
#include "lua/types.h"
#include "lua/view.h"
#endif

/** available views flags, a view should return its type and
    is also used in modules flags available in src/libs to
    control which view the module should be available in also
    which placement in the panels the module have.
*/
typedef enum 
{
  DT_VIEW_LIGHTTABLE = 1,
  DT_VIEW_DARKROOM = 2,
  DT_VIEW_TETHERING = 4,
  DT_VIEW_MAP = 8,
  DT_VIEW_SLIDESHOW = 16,
  DT_VIEW_PRINT = 32,
  DT_VIEW_KNIGHT = 64
} dt_view_type_flags_t;

// flags that a view can set in flags()
typedef enum dt_view_flags_t
{
  VIEW_FLAGS_NONE = 0,
  VIEW_FLAGS_HIDDEN = 1 << 0,       // Hide the view from userinterface
} dt_view_flags_t;

#define DT_VIEW_ALL                                                                              \
  (DT_VIEW_LIGHTTABLE | DT_VIEW_DARKROOM | DT_VIEW_TETHERING | DT_VIEW_MAP | DT_VIEW_SLIDESHOW | \
   DT_VIEW_PRINT | DT_VIEW_KNIGHT)

/**
 * main dt view module (as lighttable or darkroom)
 */
struct dt_view_t;
typedef struct dt_view_t
{
  // !!! MUST BE KEPT IN SYNC WITH src/views/view_api.h !!!

  char module_name[64];
  // dlopened module
  GModule *module;
  // custom data for module
  void *data;
  // width and height of allocation
  uint32_t width, height;
  // scroll bar control
  float vscroll_size, vscroll_lower, vscroll_viewport_size, vscroll_pos;
  float hscroll_size, hscroll_lower, hscroll_viewport_size, hscroll_pos;
  const char *(*name)(struct dt_view_t *self);    // get translatable name
  uint32_t (*view)(const struct dt_view_t *self); // get the view type
  uint32_t (*flags)();                            // get the view flags
  void (*init)(struct dt_view_t *self);           // init *data
  void (*gui_init)(struct dt_view_t *self);       // create gtk elements, called after libs are created
  void (*cleanup)(struct dt_view_t *self);        // cleanup *data
  void (*expose)(struct dt_view_t *self, cairo_t *cr, int32_t width, int32_t height, int32_t pointerx,
                 int32_t pointery);         // expose the module (gtk callback)
  int (*try_enter)(struct dt_view_t *self); // test if enter can succeed.
  void (*enter)(struct dt_view_t *self); // mode entered, this module got focus. return non-null on failure.
  void (*leave)(struct dt_view_t *self); // mode left (is called after the new try_enter has succeeded).
  void (*reset)(struct dt_view_t *self); // reset default appearance

  // event callbacks:
  void (*mouse_enter)(struct dt_view_t *self);
  void (*mouse_leave)(struct dt_view_t *self);
  void (*mouse_moved)(struct dt_view_t *self, double x, double y, double pressure, int which);

  int (*button_released)(struct dt_view_t *self, double x, double y, int which, uint32_t state);
  int (*button_pressed)(struct dt_view_t *self, double x, double y, double pressure, int which, int type,
                        uint32_t state);
  int (*key_pressed)(struct dt_view_t *self, guint key, guint state);
  int (*key_released)(struct dt_view_t *self, guint key, guint state);
  void (*configure)(struct dt_view_t *self, int width, int height);
  void (*scrolled)(struct dt_view_t *self, double x, double y, int up, int state); // mouse scrolled in view
  void (*scrollbar_changed)(struct dt_view_t *self, double x, double y); // scrollbar changed in view

  // keyboard accel callbacks
  void (*init_key_accels)(struct dt_view_t *self);
  void (*connect_key_accels)(struct dt_view_t *self);

  GSList *accel_closures;
} dt_view_t;

typedef enum dt_view_image_over_t
{
  DT_VIEW_DESERT = 0,
  DT_VIEW_STAR_1 = 1,
  DT_VIEW_STAR_2 = 2,
  DT_VIEW_STAR_3 = 3,
  DT_VIEW_STAR_4 = 4,
  DT_VIEW_STAR_5 = 5,
  DT_VIEW_REJECT = 6,
  DT_VIEW_GROUP = 7,
  DT_VIEW_AUDIO = 8
} dt_view_image_over_t;

/** returns -1 if the action has to be applied to the selection,
    or the imgid otherwise */
int32_t dt_view_get_image_to_act_on();

/** expose an image, set image over flags. return != 0 if thumbnail wasn't loaded yet. */
int dt_view_image_expose(dt_view_image_over_t *image_over, uint32_t index, cairo_t *cr, int32_t width,
                         int32_t height, int32_t zoom, int32_t px, int32_t py, gboolean full_preview, gboolean image_only);

/* expose only the image imgid at position (offsetx,offsety) into the cairo surface occupying width/height pixels.
   this routine does not output any meta-data as the version above.
 */
void
dt_view_image_only_expose(
  uint32_t imgid,
  cairo_t *cr,
  int32_t width,
  int32_t height,
  int32_t offsetx,
  int32_t offsety);


/** Set the selection bit to a given value for the specified image */
void dt_view_set_selection(int imgid, int value);
/** toggle selection of given image. */
void dt_view_toggle_selection(int imgid);

/**
 * holds all relevant data needed to manage the view
 * modules.
 */
typedef struct dt_view_manager_t
{
  GList *views;
  dt_view_t *current_view;

  /* reusable db statements
   * TODO: reconsider creating a common/database helper API
   *       instead of having this spread around in sources..
   */
  struct
  {
    /* select num from history where imgid = ?1*/
    sqlite3_stmt *have_history;
    /* select * from selected_images where imgid = ?1 */
    sqlite3_stmt *is_selected;
    /* delete from selected_images where imgid = ?1 */
    sqlite3_stmt *delete_from_selected;
    /* insert into selected_images values (?1) */
    sqlite3_stmt *make_selected;
    /* select color from color_labels where imgid=?1 */
    sqlite3_stmt *get_color;
    /* select images in group from images where imgid=?1 (also bind to ?2) */
    sqlite3_stmt *get_grouped;
  } statements;


  /*
   * Proxy
   */
  struct
  {

    /* view toolbox proxy object */
    struct
    {
      struct dt_lib_module_t *module;
      void (*add)(struct dt_lib_module_t *, GtkWidget *, dt_view_type_flags_t );
    } view_toolbox;

    /* module toolbox proxy object */
    struct
    {
      struct dt_lib_module_t *module;
      void (*add)(struct dt_lib_module_t *, GtkWidget *, dt_view_type_flags_t);
    } module_toolbox;

    /* filter toolbox proxy object */
    struct
    {
      struct dt_lib_module_t *module;
      void (*reset_filter)(struct dt_lib_module_t *, gboolean smart_filter);
    } filter;

    /* module collection proxy object */
    struct
    {
      struct dt_lib_module_t *module;
      void (*update)(struct dt_lib_module_t *);
    } module_collect;

    /* filmstrip proxy object */
    struct
    {
      struct dt_lib_module_t *module;
      void (*scroll_to_image)(struct dt_lib_module_t *, gint imgid, gboolean activate);
      int32_t (*activated_image)(struct dt_lib_module_t *);
      GtkWidget *(*widget)(struct dt_lib_module_t *);
    } filmstrip;

    /* lighttable view proxy object */
    struct
    {
      struct dt_lib_module_t *module;
      struct dt_view_t *view;
      void (*set_zoom)(struct dt_lib_module_t *module, gint zoom);
      void (*set_position)(struct dt_view_t *view, uint32_t pos);
      uint32_t (*get_position)(struct dt_view_t *view);
      int (*get_images_in_row)(struct dt_view_t *view);
      int (*get_full_preview_id)(struct dt_view_t *view);
    } lighttable;

    /* tethering view proxy object */
    struct
    {
      struct dt_view_t *view;
      const char *(*get_job_code)(const dt_view_t *view);
      void (*set_job_code)(const dt_view_t *view, const char *name);
      uint32_t (*get_selected_imgid)(const dt_view_t *view);
    } tethering;

    /* more module window proxy */
    struct
    {
      struct dt_lib_module_t *module;
      void (*update)(struct dt_lib_module_t *);
    } more_module;


/* map view proxy object */
#ifdef HAVE_MAP
    struct
    {
      struct dt_view_t *view;
      void (*center_on_location)(const dt_view_t *view, gdouble lon, gdouble lat, double zoom);
      void (*center_on_bbox)(const dt_view_t *view, gdouble lon1, gdouble lat1, gdouble lon2, gdouble lat2);
      void (*show_osd)(const dt_view_t *view, gboolean enabled);
      void (*set_map_source)(const dt_view_t *view, OsmGpsMapSource_t map_source);
      GObject *(*add_marker)(const dt_view_t *view, dt_geo_map_display_t type, GList *points);
      gboolean (*remove_marker)(const dt_view_t *view, dt_geo_map_display_t type, GObject *marker);
    } map;
#endif

    /* map view proxy object */
#ifdef HAVE_PRINT
    struct
    {
      struct dt_view_t *view;
      void (*print_settings)(const dt_view_t *view, dt_print_info_t *pinfo);
    } print;
#endif
  } proxy;


} dt_view_manager_t;

void dt_view_manager_init(dt_view_manager_t *vm);
void dt_view_manager_gui_init(dt_view_manager_t *vm);
void dt_view_manager_cleanup(dt_view_manager_t *vm);

/** return translated name. */
const char *dt_view_manager_name(dt_view_manager_t *vm);
/** switch to this module. returns non-null if the module fails to change. */
int dt_view_manager_switch(dt_view_manager_t *vm, const char *view_name);
int dt_view_manager_switch_by_view(dt_view_manager_t *vm, const dt_view_t *new_view);
/** expose current module. */
void dt_view_manager_expose(dt_view_manager_t *vm, cairo_t *cr, int32_t width, int32_t height,
                            int32_t pointerx, int32_t pointery);
/** reset current view. */
void dt_view_manager_reset(dt_view_manager_t *vm);
/** get current view of the view manager. */
const dt_view_t *dt_view_manager_get_current_view(dt_view_manager_t *vm);

void dt_view_manager_mouse_enter(dt_view_manager_t *vm);
void dt_view_manager_mouse_leave(dt_view_manager_t *vm);
void dt_view_manager_mouse_moved(dt_view_manager_t *vm, double x, double y, double pressure, int which);
int dt_view_manager_button_released(dt_view_manager_t *vm, double x, double y, int which, uint32_t state);
int dt_view_manager_button_pressed(dt_view_manager_t *vm, double x, double y, double pressure, int which,
                                   int type, uint32_t state);
int dt_view_manager_key_pressed(dt_view_manager_t *vm, guint key, guint state);
int dt_view_manager_key_released(dt_view_manager_t *vm, guint key, guint state);
void dt_view_manager_configure(dt_view_manager_t *vm, int width, int height);
void dt_view_manager_scrolled(dt_view_manager_t *vm, double x, double y, int up, int state);
void dt_view_manager_scrollbar_changed(dt_view_manager_t *vm, double x, double y);

/** add widget to the current view toolbox */
void dt_view_manager_view_toolbox_add(dt_view_manager_t *vm, GtkWidget *tool, dt_view_type_flags_t view);

/** add widget to the current module toolbox */
void dt_view_manager_module_toolbox_add(dt_view_manager_t *vm, GtkWidget *tool, dt_view_type_flags_t view);

/** set scrollbar positions, gui method. */
void dt_view_set_scrollbar(dt_view_t *view, float hpos, float vscroll_lower, float hsize, float hwinsize,
                           float vpos, float hscroll_lower, float vsize, float vwinsize);

/*
 * Tethering View PROXY
 */
/** get the current selected image id for tethering session */
int32_t dt_view_tethering_get_selected_imgid(const dt_view_manager_t *vm);
/** set the current jobcode for tethering session */
void dt_view_tethering_set_job_code(const dt_view_manager_t *vm, const char *name);
/** get the current jobcode for tethering session */
const char *dt_view_tethering_get_job_code(const dt_view_manager_t *vm);

/** update the collection module */
void dt_view_collection_update(const dt_view_manager_t *vm);

/*
 * Filter dropdown proxy
 */
void dt_view_filter_reset(const dt_view_manager_t *vm, gboolean smart_filter);

/*
 * NEW filmstrip api
 */
/*** scrolls filmstrip to the image in position 'diff' from the current one
 *** offset to be provided is the offset of the current image, as given by
 *** dt_collection_image_offset. Getting this data before changing flags allows
 *** for using this function with images disappearing from the current collection  */
void dt_view_filmstrip_scroll_relative(const int diff, int offset);
/** scrolls filmstrip to the specified image */
void dt_view_filmstrip_scroll_to_image(dt_view_manager_t *vm, const int imgid, gboolean activate);
/** get the imageid from last filmstrip activate request */
int32_t dt_view_filmstrip_get_activated_imgid(dt_view_manager_t *vm);

/** sets the lighttable image in row zoom */
void dt_view_lighttable_set_zoom(dt_view_manager_t *vm, gint zoom);
/** set first visible image offset */
void dt_view_lighttable_set_position(dt_view_manager_t *vm, uint32_t pos);
/** read first visible image offset */
uint32_t dt_view_lighttable_get_position(dt_view_manager_t *vm);

/** set active image */
void dt_view_filmstrip_set_active_image(dt_view_manager_t *vm, int iid);
/** prefetch the next few images in film strip, from selected on.
    TODO: move to control ?
*/
void dt_view_filmstrip_prefetch();

/*
 * Map View Proxy
 */
#ifdef HAVE_MAP
void dt_view_map_center_on_location(const dt_view_manager_t *vm, gdouble lon, gdouble lat, gdouble zoom);
void dt_view_map_center_on_bbox(const dt_view_manager_t *vm, gdouble lon1, gdouble lat1, gdouble lon2, gdouble lat2);
void dt_view_map_show_osd(const dt_view_manager_t *vm, gboolean enabled);
void dt_view_map_set_map_source(const dt_view_manager_t *vm, OsmGpsMapSource_t map_source);
GObject *dt_view_map_add_marker(const dt_view_manager_t *vm, dt_geo_map_display_t type, GList *points);
gboolean dt_view_map_remove_marker(const dt_view_manager_t *vm, dt_geo_map_display_t type, GObject *marker);
#endif

/*
 * Print View Proxy
 */
#ifdef HAVE_PRINT
void dt_view_print_settings(const dt_view_manager_t *vm, dt_print_info_t *pinfo);
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
