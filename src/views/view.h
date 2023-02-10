/*
    This file is part of darktable,
    Copyright (C) 2009-2021 darktable developers.

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

#include "common/act_on.h"
#include "common/action.h"
#include "common/history.h"
#include "common/image.h"
#ifdef HAVE_PRINT
#include "common/cups_print.h"
#include "common/printing.h"
#endif
#ifdef HAVE_MAP
#include "common/geo.h"
#include "common/map_locations.h"
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

typedef enum dt_lighttable_layout_t
{
  DT_LIGHTTABLE_LAYOUT_FIRST = -1,
  DT_LIGHTTABLE_LAYOUT_ZOOMABLE = 0,
  DT_LIGHTTABLE_LAYOUT_FILEMANAGER = 1,
  DT_LIGHTTABLE_LAYOUT_CULLING = 2,
  DT_LIGHTTABLE_LAYOUT_CULLING_DYNAMIC = 3,
  DT_LIGHTTABLE_LAYOUT_PREVIEW = 4,
  DT_LIGHTTABLE_LAYOUT_LAST = 5
} dt_lighttable_layout_t;

typedef enum dt_darkroom_layout_t
{
  DT_DARKROOM_LAYOUT_FIRST = -1,
  DT_DARKROOM_LAYOUT_EDITING = 0,
  DT_DARKROOM_LAYOUT_COLOR_ASSESMENT = 1,
  DT_DARKROOM_LAYOUT_LAST = 3
} dt_darkroom_layout_t;

// mouse actions struct
typedef enum dt_mouse_action_type_t
{
  DT_MOUSE_ACTION_LEFT = 0,
  DT_MOUSE_ACTION_RIGHT,
  DT_MOUSE_ACTION_MIDDLE,
  DT_MOUSE_ACTION_SCROLL,
  DT_MOUSE_ACTION_DOUBLE_LEFT,
  DT_MOUSE_ACTION_DOUBLE_RIGHT,
  DT_MOUSE_ACTION_DRAG_DROP,
  DT_MOUSE_ACTION_LEFT_DRAG,
  DT_MOUSE_ACTION_RIGHT_DRAG
} dt_mouse_action_type_t;

// flags that a view can set in flags()
typedef enum dt_view_surface_value_t
{
  DT_VIEW_SURFACE_OK = 0,
  DT_VIEW_SURFACE_KO,
  DT_VIEW_SURFACE_SMALLER
} dt_view_surface_value_t;

typedef struct dt_mouse_action_t
{
  GdkModifierType mods;
  dt_mouse_action_type_t action;
  gchar name[256];
} dt_mouse_action_t;

#define DT_VIEW_ALL                                                                              \
  (DT_VIEW_LIGHTTABLE | DT_VIEW_DARKROOM | DT_VIEW_TETHERING | DT_VIEW_MAP | DT_VIEW_SLIDESHOW | \
   DT_VIEW_PRINT | DT_VIEW_KNIGHT)

/* maximum zoom factor for the lighttable */
#define DT_LIGHTTABLE_MAX_ZOOM 25

/**
 * main dt view module (as lighttable or darkroom)
 */
struct dt_view_t;
typedef struct dt_view_t
{
  dt_action_t actions; // !!! NEEDS to be FIRST (to be able to cast convert)

#define INCLUDE_API_FROM_MODULE_H
#include "views/view_api.h"

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
} dt_view_t;

typedef enum dt_view_image_over_t
{
  DT_VIEW_ERR     = -1,
  DT_VIEW_DESERT  =  0,
  DT_VIEW_STAR_1  =  1,
  DT_VIEW_STAR_2  =  2,
  DT_VIEW_STAR_3  =  3,
  DT_VIEW_STAR_4  =  4,
  DT_VIEW_STAR_5  =  5,
  DT_VIEW_REJECT  =  6,
  DT_VIEW_GROUP   =  7,
  DT_VIEW_AUDIO   =  8,
  DT_VIEW_ALTERED =  9,
  DT_VIEW_END     = 10, // placeholder for the end of the list
} dt_view_image_over_t;

/** returns an uppercase string of file extension **plus** some flag information **/
char* dt_view_extend_modes_str(const char * name, const gboolean is_hdr, const gboolean is_bw, const gboolean is_bw_flow);
/** expose an image and return a cair0_surface. */
dt_view_surface_value_t dt_view_image_get_surface(int imgid, int width, int height, cairo_surface_t **surface,
                                                  const gboolean quality);


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

  // images currently active in the main view (there can be more than 1 in culling)
  GSList *active_images;

  // copy/paste history structure
  dt_history_copy_item_t copy_paste;

  struct
  {
    GtkWidget *window;
    GtkWidget *sticky_btn;
    GtkWidget *flow_box;
    gboolean sticky;
    gboolean prevent_refresh;
  } accels_window;

  // cached list of images to act on
  dt_act_on_cache_t act_on_cache_all;
  dt_act_on_cache_t act_on_cache_visible;

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

  struct
  {
    GPid audio_player_pid;   // the pid of the child process
    int32_t audio_player_id; // the imgid of the image the audio is played for
    guint audio_player_event_source;
  } audio;

  // toggle button for guides (in the module toolbox)
  GtkWidget *guides_toggle, *guides, *guides_colors, *guides_contrast, *guides_popover;

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
      GtkWidget *(*get_filter_box)(struct dt_lib_module_t *);
      GtkWidget *(*get_sort_box)(struct dt_lib_module_t *);
      GtkWidget *(*get_count)(struct dt_lib_module_t *);
    } filter;

    /* module collection proxy object */
    struct
    {
      struct dt_lib_module_t *module;
      void (*update)(struct dt_lib_module_t *);
      void (*update_history_visibility)(struct dt_lib_module_t *);
    } module_collect;

    /* module recent collection proxy object */
    struct
    {
      struct dt_lib_module_t *module;
      void (*update_visibility)(struct dt_lib_module_t *);
    } module_recentcollect;

    /* module filtering proxy object */
    struct
    {
      struct dt_lib_module_t *module;
      void (*update)(struct dt_lib_module_t *);
      void (*set_sort)(struct dt_lib_module_t *, int sort, gboolean asc);
      void (*reset_filter)(struct dt_lib_module_t *, gboolean smart_filter);
      void (*show_pref_menu)(struct dt_lib_module_t *, GtkWidget *bt);
    } module_filtering;

    /* filmstrip proxy object */
    struct
    {
      struct dt_lib_module_t *module;
    } filmstrip;

    /* darkroom view proxy object */
    struct
    {
      struct dt_view_t *view;
      dt_darkroom_layout_t (*get_layout)(struct dt_view_t *view);
    } darkroom;

    /* lighttable view proxy object */
    struct
    {
      struct dt_lib_module_t *module;
      struct dt_view_t *view;
      void (*set_zoom)(struct dt_lib_module_t *module, gint zoom);
      gint (*get_zoom)(struct dt_lib_module_t *module);
      dt_lighttable_layout_t (*get_layout)(struct dt_lib_module_t *module);
      void (*set_layout)(struct dt_lib_module_t *module, dt_lighttable_layout_t layout);
      void (*culling_init_mode)(struct dt_view_t *view);
      void (*culling_preview_refresh)(struct dt_view_t *view);
      void (*culling_preview_reload_overlays)(struct dt_view_t *view);
      gboolean (*get_preview_state)(struct dt_view_t *view);
      void (*set_preview_state)(struct dt_view_t *view, gboolean state, gboolean sticky, gboolean focus);
      void (*change_offset)(struct dt_view_t *view, gboolean reset, gint imgid);
    } lighttable;

    /* tethering view proxy object */
    struct
    {
      struct dt_view_t *view;
      const char *(*get_job_code)(const dt_view_t *view);
      void (*set_job_code)(const dt_view_t *view, const char *name);
      int32_t (*get_selected_imgid)(const dt_view_t *view);
    } tethering;

    /* timeline module proxy */
    struct
    {
      struct dt_lib_module_t *module;
    } timeline;


/* map view proxy object */
#ifdef HAVE_MAP
    struct
    {
      struct dt_view_t *view;
      void (*center_on_location)(const dt_view_t *view, gdouble lon, gdouble lat, double zoom);
      void (*center_on_bbox)(const dt_view_t *view, gdouble lon1, gdouble lat1, gdouble lon2, gdouble lat2);
      void (*show_osd)(const dt_view_t *view);
      void (*set_map_source)(const dt_view_t *view, OsmGpsMapSource_t map_source);
      GObject *(*add_marker)(const dt_view_t *view, dt_geo_map_display_t type, GList *points);
      gboolean (*remove_marker)(const dt_view_t *view, dt_geo_map_display_t type, GObject *marker);
      void (*add_location)(const dt_view_t *view, dt_map_location_data_t *p, const guint posid);
      void (*location_action)(const dt_view_t *view, const int action);
      void (*drag_set_icon)(const dt_view_t *view, GdkDragContext *context, const int imgid, const int count);
      gboolean (*redraw)(gpointer user_data);
      gboolean (*display_selected)(gpointer user_data);
    } map;
#endif

    /* map view proxy object */
#ifdef HAVE_PRINT
    struct
    {
      struct dt_view_t *view;
      void (*print_settings)(const dt_view_t *view, dt_print_info_t *pinfo, dt_images_box *imgs);
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
void dt_view_manager_configure(dt_view_manager_t *vm, int width, int height);
void dt_view_manager_scrolled(dt_view_manager_t *vm, double x, double y, int up, int state);
void dt_view_manager_scrollbar_changed(dt_view_manager_t *vm, double x, double y);

/** add widget to the current view toolbox */
void dt_view_manager_view_toolbox_add(dt_view_manager_t *vm, GtkWidget *tool, dt_view_type_flags_t view);

/** add widget to the current module toolbox */
void dt_view_manager_module_toolbox_add(dt_view_manager_t *vm, GtkWidget *tool, dt_view_type_flags_t view);

/** set scrollbar positions, gui method. */
void dt_view_set_scrollbar(dt_view_t *view, float hpos, float hscroll_lower, float hsize, float hwinsize,
                           float vpos, float vscroll_lower, float vsize, float vwinsize);

/** add mouse action record to list of mouse actions */
GSList *dt_mouse_action_create_simple(GSList *actions, dt_mouse_action_type_t type, GdkModifierType accel,
                                      const char *const description);
GSList *dt_mouse_action_create_format(GSList *actions, dt_mouse_action_type_t type, GdkModifierType accel,
                                      const char *const format_string, const char *const replacement);

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
void dt_view_filtering_set_sort(const dt_view_manager_t *vm, int sort, gboolean asc);
void dt_view_collection_update_history_state(const dt_view_manager_t *vm);

/*
 * Filter dropdown proxy
 */
void dt_view_filtering_reset(const dt_view_manager_t *vm, gboolean smart_filter);
void dt_view_filtering_show_pref_menu(const dt_view_manager_t *vm, GtkWidget *bt);
GtkWidget *dt_view_filter_get_filters_box(const dt_view_manager_t *vm);
GtkWidget *dt_view_filter_get_sort_box(const dt_view_manager_t *vm);
GtkWidget *dt_view_filter_get_count(const dt_view_manager_t *vm);

// active images functions
void dt_view_active_images_reset(gboolean raise);
void dt_view_active_images_add(int imgid, gboolean raise);
GSList *dt_view_active_images_get();

/** get the lighttable current layout */
dt_lighttable_layout_t dt_view_lighttable_get_layout(dt_view_manager_t *vm);
/** get the darkroom current layout */
dt_darkroom_layout_t dt_view_darkroom_get_layout(dt_view_manager_t *vm);
/** get the lighttable full preview state */
gboolean dt_view_lighttable_preview_state(dt_view_manager_t *vm);
/** set the lighttable full preview state */
void dt_view_lighttable_set_preview_state(dt_view_manager_t *vm, gboolean state, gboolean sticky, gboolean focus);
/** sets the lighttable image in row zoom */
void dt_view_lighttable_set_zoom(dt_view_manager_t *vm, gint zoom);
/** gets the lighttable image in row zoom */
gint dt_view_lighttable_get_zoom(dt_view_manager_t *vm);
/** reinit culling for new mode */
void dt_view_lighttable_culling_init_mode(dt_view_manager_t *vm);
/** force refresh of culling and/or preview */
void dt_view_lighttable_culling_preview_refresh(dt_view_manager_t *vm);
/** force refresh of culling and/or preview overlays */
void dt_view_lighttable_culling_preview_reload_overlays(dt_view_manager_t *vm);
/** sets the offset image (for culling and full preview) */
void dt_view_lighttable_change_offset(dt_view_manager_t *vm, gboolean reset, gint imgid);

/* accel window */
void dt_view_accels_show(dt_view_manager_t *vm);
void dt_view_accels_hide(dt_view_manager_t *vm);
void dt_view_accels_refresh(dt_view_manager_t *vm);

/* audio */
void dt_view_audio_start(dt_view_manager_t *vm, int imgid);
void dt_view_audio_stop(dt_view_manager_t *vm);

/*
 * Map View Proxy
 */
#ifdef HAVE_MAP
void dt_view_map_center_on_location(const dt_view_manager_t *vm, gdouble lon, gdouble lat, gdouble zoom);
void dt_view_map_center_on_bbox(const dt_view_manager_t *vm, gdouble lon1, gdouble lat1, gdouble lon2, gdouble lat2);
void dt_view_map_show_osd(const dt_view_manager_t *vm);
void dt_view_map_set_map_source(const dt_view_manager_t *vm, OsmGpsMapSource_t map_source);
GObject *dt_view_map_add_marker(const dt_view_manager_t *vm, dt_geo_map_display_t type, GList *points);
gboolean dt_view_map_remove_marker(const dt_view_manager_t *vm, dt_geo_map_display_t type, GObject *marker);
void dt_view_map_add_location(const dt_view_manager_t *vm, dt_map_location_data_t *p, const guint posid);
void dt_view_map_location_action(const dt_view_manager_t *vm, const int action);
void dt_view_map_drag_set_icon(const dt_view_manager_t *vm, GdkDragContext *context, const int imgid, const int count);
#endif

/*
 * Print View Proxy
 */
#ifdef HAVE_PRINT
void dt_view_print_settings(const dt_view_manager_t *vm, dt_print_info_t *pinfo, dt_images_box *imgs);
#endif

/*
 * Paint buffer (size processed_width x processed_height) in cairo (as a surface)
 * on the viewport (size width x height).
 */

typedef enum _window_t
{
  DT_WINDOW_MAIN,
  DT_WINDOW_SECOND,
  DT_WINDOW_SLIDESHOW
} dt_window_t;

void dt_view_paint_buffer(
  cairo_t *cr,
  const size_t width,
  const size_t height,
  uint8_t *buffer,
  const size_t processed_width,
  const size_t processed_height,
  const dt_window_t window);

void dt_view_paint_pixbuf(
  cairo_t *cr,
  const size_t width,
  const size_t height,
  uint8_t *buffer,
  const size_t processed_width,
  const size_t processed_height,
  const dt_window_t window);

cairo_surface_t *dt_view_create_surface(
  uint8_t *buffer,
  const size_t processed_width,
  const size_t processed_height);

void dt_view_paint_surface(
  cairo_t *cr,
  const size_t width,
  const size_t height,
  cairo_surface_t *surface,
  const size_t processed_width,
  const size_t processed_height,
  const dt_window_t window);

typedef uint64_t dt_view_context_t;

dt_view_context_t dt_view_get_view_context(void);

gboolean dt_view_check_view_context(dt_view_context_t *ctx);

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
