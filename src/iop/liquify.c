/*
    This file is part of darktable,
    Copyright (C) 2014-2021 darktable developers.

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "bauhaus/bauhaus.h"
#include "common/interpolation.h"
#include "common/opencl.h"
#include "common/math.h"
#include "common/collection.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/imageop.h"
#include "develop/imageop_gui.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "iop/iop_api.h"
#include <assert.h>
#include <cairo.h>
#include <complex.h>
#include <math.h>
#include <stdlib.h>

#ifdef _OPENMP
#include <omp.h>
#endif

// this is the version of the modules parameters, and includes version information about compile-time dt
DT_MODULE_INTROSPECTION(1, dt_iop_liquify_params_t)

#pragma GCC diagnostic ignored "-Wshadow"

#define MAX_NODES 100 // max of nodes in one instance

const int   LOOKUP_OVERSAMPLE = 10;
const int   INTERPOLATION_POINTS = 100; // when interpolating bezier
const float STAMP_RELOCATION = 0.1;     // how many radii to move stamp forward when following a path

#define CONF_RADIUS "plugins/darkroom/liquify/radius"
#define CONF_ANGLE "plugins/darkroom/liquify/angle"
#define CONF_STRENGTH "plugins/darkroom/liquify/strength"

// enum of layers. sorted back to front.

typedef enum
{
  DT_LIQUIFY_LAYER_BACKGROUND,
  DT_LIQUIFY_LAYER_RADIUS,
  DT_LIQUIFY_LAYER_HARDNESS1,
  DT_LIQUIFY_LAYER_HARDNESS2,
  DT_LIQUIFY_LAYER_WARPS,
  DT_LIQUIFY_LAYER_PATH,
  DT_LIQUIFY_LAYER_CTRLPOINT1_HANDLE,
  DT_LIQUIFY_LAYER_CTRLPOINT2_HANDLE,
  DT_LIQUIFY_LAYER_RADIUSPOINT_HANDLE,
  DT_LIQUIFY_LAYER_HARDNESSPOINT1_HANDLE,
  DT_LIQUIFY_LAYER_HARDNESSPOINT2_HANDLE,
  DT_LIQUIFY_LAYER_STRENGTHPOINT_HANDLE,
  DT_LIQUIFY_LAYER_CENTERPOINT,
  DT_LIQUIFY_LAYER_CTRLPOINT1,
  DT_LIQUIFY_LAYER_CTRLPOINT2,
  DT_LIQUIFY_LAYER_RADIUSPOINT,
  DT_LIQUIFY_LAYER_HARDNESSPOINT1,
  DT_LIQUIFY_LAYER_HARDNESSPOINT2,
  DT_LIQUIFY_LAYER_STRENGTHPOINT,
  DT_LIQUIFY_LAYER_LAST
} dt_liquify_layer_enum_t;

typedef enum
{
  DT_LIQUIFY_LAYER_FLAG_HIT_TEST      =  1,   ///< include layer in hit testing
  DT_LIQUIFY_LAYER_FLAG_PREV_SELECTED =  2,   ///< show if previous node is selected
  DT_LIQUIFY_LAYER_FLAG_NODE_SELECTED =  4,   ///< show if node is selected
  DT_LIQUIFY_LAYER_FLAG_POINT_TOOL    =  8,   ///< show if point tool active
  DT_LIQUIFY_LAYER_FLAG_LINE_TOOL     = 16,   ///< show if line tool active
  DT_LIQUIFY_LAYER_FLAG_CURVE_TOOL    = 32,   ///< show if line tool active
  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL     = 64,   ///< show if node tool active
  DT_LIQUIFY_LAYER_FLAG_ANY_TOOL      = 8 + 16 + 32 + 64,
} dt_liquify_layer_flag_enum_t;

typedef struct
{
  float red, green, blue, alpha;
} dt_liquify_rgba_t;

#define COLOR_NULL                 { 0.0, 0.0, 0.0, 0.8 }
#define GREY                       { 0.3, 0.3, 0.3, 0.8 }
#define LGREY                      { 0.8, 0.8, 0.8, 1.0 }
#define COLOR_DEBUG                { 0.9, 0.9, 0.0, 1.0 }
static const dt_liquify_rgba_t DT_LIQUIFY_COLOR_SELECTED = { 1.0, 1.0, 1.0, 1.0 };
static const dt_liquify_rgba_t DT_LIQUIFY_COLOR_HOVER    = { 1.0, 1.0, 1.0, 0.8 };

typedef struct
{
  dt_liquify_layer_enum_t hover_master;    ///< hover whenever master layer hovers, eg. to
  dt_liquify_rgba_t fg;                    ///< the foreground color for this layer
  dt_liquify_rgba_t bg;                    ///< the background color for this layer
  float opacity;                           ///< the opacity of this layer
                                           ///  highlight the whole radius when only the
                                           ///  radius point is hovered
  dt_liquify_layer_flag_enum_t flags;      ///< various flags for layer
  const char *hint;                        ///< hint displayed when hovering
} dt_liquify_layer_t;

dt_liquify_layer_t dt_liquify_layers[] =
{
  { DT_LIQUIFY_LAYER_BACKGROUND,     COLOR_NULL,  COLOR_NULL, 0.0,  0,                                                                                                      },
  { DT_LIQUIFY_LAYER_RADIUS,         COLOR_DEBUG, COLOR_NULL, 0.25, DT_LIQUIFY_LAYER_FLAG_ANY_TOOL,                                                                         },
  { DT_LIQUIFY_LAYER_HARDNESS1,      COLOR_DEBUG, COLOR_NULL, 1.0,  0,                                                                                                      },
  { DT_LIQUIFY_LAYER_HARDNESS2,      COLOR_DEBUG, COLOR_NULL, 1.0,  0,                                                                                                      },
  { DT_LIQUIFY_LAYER_WARPS,          COLOR_DEBUG, LGREY,      0.5,  DT_LIQUIFY_LAYER_FLAG_ANY_TOOL,                                                                         },
  { DT_LIQUIFY_LAYER_PATH,           GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_ANY_TOOL  | DT_LIQUIFY_LAYER_FLAG_HIT_TEST,                                       },
  { DT_LIQUIFY_LAYER_CTRLPOINT1,     GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL,                                                                        },
  { DT_LIQUIFY_LAYER_CTRLPOINT2,     GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL,                                                                        },
  { DT_LIQUIFY_LAYER_RADIUSPOINT,    GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL,                                                                        },
  { DT_LIQUIFY_LAYER_HARDNESSPOINT1, GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL | DT_LIQUIFY_LAYER_FLAG_NODE_SELECTED,                                  },
  { DT_LIQUIFY_LAYER_HARDNESSPOINT2, GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL | DT_LIQUIFY_LAYER_FLAG_NODE_SELECTED,                                  },
  { DT_LIQUIFY_LAYER_STRENGTHPOINT,  GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_ANY_TOOL,                                                                         },
  { DT_LIQUIFY_LAYER_CENTERPOINT,    GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_ANY_TOOL  | DT_LIQUIFY_LAYER_FLAG_HIT_TEST,                                       },
  { DT_LIQUIFY_LAYER_CTRLPOINT1,     GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL | DT_LIQUIFY_LAYER_FLAG_HIT_TEST,                                       },
  { DT_LIQUIFY_LAYER_CTRLPOINT2,     GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL | DT_LIQUIFY_LAYER_FLAG_HIT_TEST,                                       },
  { DT_LIQUIFY_LAYER_RADIUSPOINT,    GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL | DT_LIQUIFY_LAYER_FLAG_HIT_TEST,                                       },
  { DT_LIQUIFY_LAYER_HARDNESSPOINT1, GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL | DT_LIQUIFY_LAYER_FLAG_NODE_SELECTED | DT_LIQUIFY_LAYER_FLAG_HIT_TEST, },
  { DT_LIQUIFY_LAYER_HARDNESSPOINT2, GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_NODE_TOOL | DT_LIQUIFY_LAYER_FLAG_NODE_SELECTED | DT_LIQUIFY_LAYER_FLAG_HIT_TEST, },
  { DT_LIQUIFY_LAYER_STRENGTHPOINT,  GREY,        LGREY,      1.0,  DT_LIQUIFY_LAYER_FLAG_ANY_TOOL  | DT_LIQUIFY_LAYER_FLAG_HIT_TEST,                                       }
};

typedef enum
{
  DT_LIQUIFY_UI_WIDTH_THINLINE,
  DT_LIQUIFY_UI_WIDTH_THICKLINE,
  DT_LIQUIFY_UI_WIDTH_DOUBLELINE,
  DT_LIQUIFY_UI_WIDTH_GIZMO,
  DT_LIQUIFY_UI_WIDTH_GIZMO_SMALL,
  DT_LIQUIFY_UI_WIDTH_DEFAULT_RADIUS,
  DT_LIQUIFY_UI_WIDTH_DEFAULT_STRENGTH,
  DT_LIQUIFY_UI_WIDTH_MIN_DRAG,
  DT_LIQUIFY_UI_WIDTH_LAST
} dt_liquify_ui_width_enum_t;

float dt_liquify_ui_widths [] =
{
  // value in 1/96 inch (that is: in pixels on a standard 96 dpi screen)
    2.0, // DT_LIQUIFY_UI_WIDTH_THINLINE
    3.0, // DT_LIQUIFY_UI_WIDTH_THICKLINE
    3.0, // DT_LIQUIFY_UI_WIDTH_DOUBLELINE
    9.0, // DT_LIQUIFY_UI_WIDTH_GIZMO
    7.0, // DT_LIQUIFY_UI_WIDTH_GIZMO_SMALL
  100.0, // DT_LIQUIFY_UI_WIDTH_DEFAULT_RADIUS,
   50.0, // DT_LIQUIFY_UI_WIDTH_DEFAULT_STRENGTH,
    3.0  // DT_LIQUIFY_UI_WIDTH_MIN_DRAG
};

typedef enum
{
  DT_LIQUIFY_WARP_TYPE_LINEAR,        // $DESCRIPTION: "linear" A linear warp originating from one point.
  DT_LIQUIFY_WARP_TYPE_RADIAL_GROW,   // $DESCRIPTION: "radial grow" A radial warp originating from one point.
  DT_LIQUIFY_WARP_TYPE_RADIAL_SHRINK, // $DESCRIPTION: "radial shrink"
  DT_LIQUIFY_WARP_TYPE_LAST
} dt_liquify_warp_type_enum_t;

typedef enum
{
  DT_LIQUIFY_NODE_TYPE_CUSP,        // $DESCRIPTION: "cusp"
  DT_LIQUIFY_NODE_TYPE_SMOOTH,      // $DESCRIPTION: "smooth"
  DT_LIQUIFY_NODE_TYPE_SYMMETRICAL, // $DESCRIPTION: "symmetrical"
  DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH,  // $DESCRIPTION: "autosmooth"
  DT_LIQUIFY_NODE_TYPE_LAST
} dt_liquify_node_type_enum_t;

typedef enum
{
  DT_LIQUIFY_STATUS_NONE = 0,         // $DESCRIPTION: "none"
  DT_LIQUIFY_STATUS_NEW = 1,          // $DESCRIPTION: "new"
  DT_LIQUIFY_STATUS_INTERPOLATED = 2, // $DESCRIPTION: "interpolated"
  DT_LIQUIFY_STATUS_PREVIEW = 4,      // $DESCRIPTION: "preview"
  DT_LIQUIFY_STATUS_LAST
} dt_liquify_status_enum_t;

// enumerates the shapes types we use.

typedef enum
{
  DT_LIQUIFY_PATH_INVALIDATED = 0, // $DESCRIPTION: "invalidated"
  DT_LIQUIFY_PATH_MOVE_TO_V1,      // $DESCRIPTION: "move"
  DT_LIQUIFY_PATH_LINE_TO_V1,      // $DESCRIPTION: "line"
  DT_LIQUIFY_PATH_CURVE_TO_V1,     // $DESCRIPTION: "curve"
} dt_liquify_path_data_enum_t;

typedef struct
{
  dt_liquify_path_data_enum_t type;
  dt_liquify_node_type_enum_t node_type;
  dt_liquify_layer_enum_t selected;
  dt_liquify_layer_enum_t hovered;
  int8_t prev;
  int8_t idx;
  int8_t next;
} dt_liquify_path_header_t;

// Scalars and vectors are represented here as points because the only
// thing we can reasonably distort_transform are points.

typedef struct
{
  float complex point;
  float complex strength;   ///< a point (the effective strength vector is: strength - point)
  float complex radius;     ///< a point (the effective radius scalar is: cabs(radius - point))
  float control1;           ///< range 0.0 .. 1.0 == radius
  float control2;           ///< range 0.0 .. 1.0 == radius
  dt_liquify_warp_type_enum_t type;
  dt_liquify_status_enum_t status;
} dt_liquify_warp_t;

typedef struct
{
  float complex ctrl1;
  float complex ctrl2;
} dt_liquify_node_t;

// set up lots of alternative ways to get at the popular members.

typedef struct
{
  dt_liquify_path_header_t header;
  dt_liquify_warp_t        warp;
  dt_liquify_node_t        node; // extended node data
} dt_liquify_path_data_t;

typedef struct
{
  dt_liquify_layer_enum_t layer;
  dt_liquify_path_data_t *elem;
} dt_liquify_hit_t;

static const dt_liquify_hit_t NOWHERE = { DT_LIQUIFY_LAYER_BACKGROUND, NULL };

typedef struct
{
  dt_liquify_path_data_t nodes[MAX_NODES];
} dt_iop_liquify_params_t;

typedef struct
{
  int warp_kernel;
} dt_iop_liquify_global_data_t;

typedef struct
{
  int node_index; // last node index inserted

  float complex last_mouse_pos;
  float complex last_button1_pressed_pos;
  GdkModifierType last_mouse_mods;  ///< GDK modifiers at the time mouse button was pressed.

  dt_liquify_hit_t last_hit;      ///< Element last hit with mouse button.
  dt_liquify_hit_t dragging;      ///< Element being dragged with mouse button.

  dt_liquify_path_data_t *temp;    ///< Points to the element under construction or NULL.
  dt_liquify_status_enum_t status; ///< Various flags.

  GtkLabel *label;
  GtkToggleButton *btn_point_tool, *btn_line_tool, *btn_curve_tool, *btn_node_tool;

  gboolean creation_continuous;
  gboolean just_started;
} dt_iop_liquify_gui_data_t;


// this returns a translatable name
const char *name()
{
  return _("liquify");
}

const char **description(struct dt_iop_module_t *self)
{
  return dt_iop_set_description(self, _("distort parts of the image"),
                                      _("creative"),
                                      _("linear, RGB, scene-referred"),
                                      _("geometric, RGB"),
                                      _("linear, RGB, scene-referred"));
}


int default_group()
{
  return IOP_GROUP_CORRECT | IOP_GROUP_EFFECTS;
}

int flags()
{
  return IOP_FLAGS_SUPPORTS_BLENDING | IOP_FLAGS_GUIDES_WIDGET;
}

int operation_tags()
{
   return IOP_TAG_DISTORT;
}

int default_colorspace(dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  return IOP_CS_RGB;
}

/******************************************************************************/
/* Code common to op-engine and gui.                                          */
/******************************************************************************/

static inline float get_rot(const dt_liquify_warp_type_enum_t warp_type)
{
  if(warp_type == DT_LIQUIFY_WARP_TYPE_RADIAL_SHRINK)
    return DT_M_PI_F;
  else
    return 0.0f;
}

static dt_liquify_path_data_t *node_alloc(dt_iop_liquify_params_t *p, int *node_index)
{
  for(int k=0; k<MAX_NODES; k++)
    if(p->nodes[k].header.type == DT_LIQUIFY_PATH_INVALIDATED)
    {
      *node_index = k;
      p->nodes[k].header.idx = k;
      p->nodes[k].header.next = p->nodes[k].header.prev = -1;
      p->nodes[k].header.selected = p->nodes[k].header.hovered = 0;
      return &p->nodes[k];
    }
  return NULL;
}

static dt_liquify_path_data_t *node_prev(dt_iop_liquify_params_t *p, const dt_liquify_path_data_t *n)
{
  if(n->header.prev == -1)
    return NULL;
  else
    return &p->nodes[n->header.prev];
}

static dt_liquify_path_data_t *node_get(dt_iop_liquify_params_t *p, const int index)
{
  if(index > -1 && index < MAX_NODES)
    return &p->nodes[index];
  else
    return NULL;
}

static dt_liquify_path_data_t *node_next(dt_iop_liquify_params_t *p, const dt_liquify_path_data_t *n)
{
  if(n->header.next == -1)
    return NULL;
  else
    return &p->nodes[n->header.next];
}

static void node_insert_before(dt_iop_liquify_params_t *p, dt_liquify_path_data_t *this, dt_liquify_path_data_t *new)
{
  new->header.next  = this->header.idx;
  new->header.prev  = this->header.prev;
  if(this->header.prev != -1)
    p->nodes[this->header.prev].header.next = new->header.idx;
  this->header.prev = new->header.idx;
}

static void node_gc(dt_iop_liquify_params_t *p)
{
  int last=0;
  for(last=MAX_NODES-1; last>0; last--)
    if(p->nodes[last].header.type != DT_LIQUIFY_PATH_INVALIDATED)
      break;
  int k = 0;

  while(k<=last)
  {
    if(p->nodes[k].header.type == DT_LIQUIFY_PATH_INVALIDATED)
    {
      for(int e=0; e<last; e++)
      {
        //  then move slot if above position k
        if(e >= k)                       p->nodes[e] = p->nodes[e+1];
        //  update all pointers above position k
        if(e >= k)                       p->nodes[e].header.idx--;
        if(p->nodes[e].header.prev >= k) p->nodes[e].header.prev--;
        if(p->nodes[e].header.next >= k) p->nodes[e].header.next--;
      }
      last--;
    }
    else
      k++;
  }
  //  invalidate all nodes beyond the last moved one
  for(int k=last+1; k<MAX_NODES; k++)
    p->nodes[k].header.type = DT_LIQUIFY_PATH_INVALIDATED;
}

static void node_delete(dt_iop_liquify_params_t *p, dt_liquify_path_data_t *this)
{
  dt_liquify_path_data_t *prev = node_prev(p, this);
  dt_liquify_path_data_t *next = node_next(p, this);

  if(!prev && next)
  {
    next->header.prev = -1;
    next->header.type = DT_LIQUIFY_PATH_MOVE_TO_V1;
  }
  else if(prev)
  {
    prev->header.next = this->header.next;

    if(next)
      next->header.prev = prev->header.idx;
  }

  this->header.prev = this->header.next = - 1;
  this->header.type = DT_LIQUIFY_PATH_INVALIDATED;
  node_gc(p);
}

static void path_delete(dt_iop_liquify_params_t *p, dt_liquify_path_data_t *this)
{
  dt_liquify_path_data_t *n = this;

  // clear next
  while(n)
  {
    n->header.type = DT_LIQUIFY_PATH_INVALIDATED;
    n = node_next(p, n);
  }

  // clear prev
  n = this;
  while(n)
  {
    n->header.type = DT_LIQUIFY_PATH_INVALIDATED;
    n = node_prev(p, n);
  }
  node_gc(p);
}

/**
 * The functions in this group help transform between coordinate
 * systems.  (In darktable nomenclature this kind of transform is
 * called 'distort').
 *
 * The transforms between coordinate systems are not necessarily
 * perspective transforms (eg. lensfun), therefore no transformation
 * matrix can be specified for them, instead all points to be
 * transformed have to be passed through a darktable function.
 *
 * Note: only points may be sensibly 'distorted'. Vectors and scalars
 * don't have a meaningful 'distort'.
 *
 *
 * Explanation of the coordinate systems used by this module:
 *
 * RAW: These are sensor coordinates. They go from x=0, y=0 to x=<sensor
 * width>, y=<sensor height>. In a landscape picture (rotated 0°) x=0,
 * y=0 will be top left. In a portrait picture (rotated 90°
 * counter-clockwise) x=0, y=0 will be bottom left.
 *
 * The user probably wants liquified regions to be anchored to the
 * motive when more transformations are added, eg. a different
 * cropping of the image.  For this to work, all coordinates we store
 * or pass between gui and pipe are RAW sensor coordinates.
 *
 *
 * PIECE: These are coordinates based on the size of our pipe piece.
 * They go from x=0, y=0 to x=<width of piece>, y=<height of piece>.
 * PIECE coordinates should only be used while processing an image.
 *
 * Note: Currently (as of darktable 1.7) there are no geometry
 * transforms between RAW and PIECE (our module coming very early in
 * the pipe), but this may change in a later release. By allowing for
 * them now, we are prepared for pipe order re-shuffeling.
 *
 *
 * CAIRO: These are coordinates based on the cairo view.  The extent
 * of the longest side of the cooked picture is normalized to 1.0.
 * x=0, y=0 is the top left of the cooked picture.  x=u, y=v is the
 * bottom right of a cooked picture with u<=1, v<=1 and either u==1 or
 * v==1 depending on orientation.  Note that depending on pan and zoom
 * cairo view borders and cooked picture borders may intersect in many
 * ways.
 *
 * The normalized scale helps in choosing default values for vectors and
 * radii.
 *
 * VIEW: These are coordinates based on the cairo view. x=0, y=0 being
 * top left and x=<view width>, y=<view height> being bottom right.
 * The parameters to the mouse_moved, button_pressed, and
 * button_released functions are in this system.
 *
 * This system is also used for sizing ui-elements. They cannot be
 * expressed in CAIRO coordinates because they should not change size
 * when zooming the picture.
 *
 * To get sensible sizes for ui elements and default warps use this
 * relation between the scales: CAIRO * get_zoom_scale () == VIEW.
 *
 */

typedef struct
{
  dt_develop_t *develop;
  dt_dev_pixelpipe_t *pipe;
  float from_scale;
  float to_scale;
  int transf_direction;
  gboolean from_distort_transform;
} distort_params_t;

static void _distort_paths(const struct dt_iop_module_t *module,
                           const distort_params_t *params, const dt_iop_liquify_params_t *p)
{
  int len = 0;

  // count nodes

  for(int k = 0; k < MAX_NODES; k++)
  {
    dt_liquify_path_data_t *data = (dt_liquify_path_data_t *) &p->nodes[k];
    if(data->header.type == DT_LIQUIFY_PATH_INVALIDATED)
      break;

    switch(data->header.type)
    {
    case DT_LIQUIFY_PATH_CURVE_TO_V1:
      len += 2;
      // fall thru
    case DT_LIQUIFY_PATH_MOVE_TO_V1:
    case DT_LIQUIFY_PATH_LINE_TO_V1:
      len += 3;
      break;
    default:
      break;
    }
  }

  // create buffer with all points

  float *buffer = malloc(sizeof(float) * 2 * len);
  float *b = buffer;

  for(int k = 0; k < MAX_NODES; k++)
  {
    dt_liquify_path_data_t *data = (dt_liquify_path_data_t *) &p->nodes[k];
    if(data->header.type == DT_LIQUIFY_PATH_INVALIDATED)
      break;

    switch(data->header.type)
    {
    case DT_LIQUIFY_PATH_CURVE_TO_V1:
      *b++ = crealf(data->node.ctrl1) / params->from_scale;
      *b++ = cimagf(data->node.ctrl1) / params->from_scale;
      *b++ = crealf(data->node.ctrl2) / params->from_scale;
      *b++ = cimagf(data->node.ctrl2) / params->from_scale;
      // fall thru
    case DT_LIQUIFY_PATH_MOVE_TO_V1:
    case DT_LIQUIFY_PATH_LINE_TO_V1:
      *b++ = crealf(data->warp.point) / params->from_scale;
      *b++ = cimagf(data->warp.point) / params->from_scale;
      *b++ = crealf(data->warp.strength) / params->from_scale;
      *b++ = cimagf(data->warp.strength) / params->from_scale;
      *b++ = crealf(data->warp.radius) / params->from_scale;
      *b++ = cimagf(data->warp.radius) / params->from_scale;
      break;
    default:
      break;
    }
  }
  if(params->from_distort_transform)
  {
    if(params->transf_direction == DT_DEV_TRANSFORM_DIR_ALL)
    {
      dt_dev_distort_transform_locked(params->develop, params->pipe, module->iop_order,
                                      DT_DEV_TRANSFORM_DIR_BACK_EXCL, buffer, len);
      dt_dev_distort_transform_locked(params->develop, params->pipe, module->iop_order,
                                      DT_DEV_TRANSFORM_DIR_FORW_EXCL, buffer, len);
    }
    else
      dt_dev_distort_transform_locked(params->develop, params->pipe, module->iop_order,
                                      params->transf_direction, buffer, len);
  }
  else
  {
    if(params->transf_direction == DT_DEV_TRANSFORM_DIR_ALL)
    {
      dt_dev_distort_transform_plus(params->develop, params->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL, buffer, len);
      dt_dev_distort_transform_plus(params->develop, params->pipe, module->iop_order, DT_DEV_TRANSFORM_DIR_FORW_EXCL, buffer, len);
    }
    else
      dt_dev_distort_transform_plus(params->develop, params->pipe, module->iop_order, params->transf_direction, buffer, len);
  }

  // record back the transformed points

  b = buffer;

  for(int k = 0; k < MAX_NODES; k++)
  {
    dt_liquify_path_data_t *data = (dt_liquify_path_data_t *) &p->nodes[k];
    if(data->header.type == DT_LIQUIFY_PATH_INVALIDATED)
      break;

    switch(data->header.type)
    {
       case DT_LIQUIFY_PATH_CURVE_TO_V1:
         data->node.ctrl1 = (b[0] + b[1] * I) * params->to_scale;
         b += 2;
         data->node.ctrl2 = (b[0] + b[1] * I) * params->to_scale;
         b += 2;
         // fall thru
       case DT_LIQUIFY_PATH_MOVE_TO_V1:
       case DT_LIQUIFY_PATH_LINE_TO_V1:
         data->warp.point = (b[0] + b[1] * I) * params->to_scale;
         b += 2;
         data->warp.strength = (b[0] + b[1] * I) * params->to_scale;
         b += 2;
         data->warp.radius = (b[0] + b[1] * I) * params->to_scale;
         b += 2;
         break;
       default:
         break;
    }
  }

  free(buffer);
}

static void distort_paths_raw_to_piece(const struct dt_iop_module_t *module,
                                       dt_dev_pixelpipe_t *pipe,
                                       const float roi_in_scale,
                                       dt_iop_liquify_params_t *p,
                                       const gboolean from_distort_transform)
{
  const distort_params_t params = { module->dev, pipe, pipe->iscale, roi_in_scale, DT_DEV_TRANSFORM_DIR_BACK_EXCL, from_distort_transform };
  _distort_paths(module, &params, p);
}

// op-engine code

static inline float complex normalize(const float complex v)
{
  if(cabsf(v) < 0.000001f)
    return 1.0f;
  return v / cabsf(v);
}

// calculate the linear blend of scalars a and b

static inline float mix(const float a, const float b, const float t)
{
  return a + (b - a) * t;
}


// calculate the linear blend of points p0 and p1

static inline float complex cmix(const float complex p0, const float complex p1, const float t)
{
  return p0 + (p1 - p0) * t;
}

static void mix_warps(dt_liquify_warp_t *result,
                       const dt_liquify_warp_t *warp1,
                       const dt_liquify_warp_t *warp2,
                       const complex float pt,
                       const float t)
{
  result->type     = warp1->type;
  result->control1 = mix (warp1->control1, warp2->control1, t);
  result->control2 = mix (warp1->control2, warp2->control2, t);

  const float radius = mix(cabsf(warp1->radius - warp1->point), cabsf(warp2->radius - warp2->point), t);
  result->radius     = pt + radius;

  const complex float p1 = warp1->strength - warp1->point;
  const complex float p2 = warp2->strength - warp2->point;
  float arg1 = cargf(p1);
  float arg2 = cargf(p2);
  gboolean invert = FALSE;

  if(arg1 > .0f && arg2 < -(M_PI_F / 2.f))
  {
    invert = TRUE;
    arg1 = M_PI_F - arg1;
    arg2 = -M_PI_F - arg2;
  }
  else if(arg1 < -(M_PI_F / 2.f) && arg2 > .0f)
  {
    invert = TRUE;
    arg1 = -M_PI_F - arg1;
    arg2 = M_PI_F - arg2;
  }

  const float r    = mix(cabsf(p1), cabsf(p2), t);
  const float phi  = invert ? M_PI_F - mix(arg1, arg2, t) : mix(arg1, arg2, t);

  result->strength = pt + r * cexpf(phi * I);
  result->point    = pt;
}

// Interpolate a cubic bezier spline into a series of points.

static void interpolate_cubic_bezier(const float complex p0,
                                      const float complex p1,
                                      const float complex p2,
                                      const float complex p3,
                                      float complex buffer[],
                                      const int n)
{
  // convert from bernstein basis to polynomial basis to get faster math
  // See: http://www.tinaja.com/glib/cubemath.pdf
  const float complex A = p3 - 3 * p2 + 3 * p1 -     p0;
  const float complex B =      3 * p2 - 6 * p1 + 3 * p0;
  const float complex C =               3 * p1 - 3 * p0;
  const float complex D =                            p0;

  float complex *buf = buffer;
  const float step = 1.0f / n;
  float t = step;
  *buf++ = p0;

  for(int i = 1; i < n - 1; ++i)
  {
    *buf++ = ((A * t + B) * t + C) * t + D;
    t += step;
  }
  *buf = p3;
}

static GList *interpolate_paths(dt_iop_liquify_params_t *p);

/*
  Get approx. arc length of a curve.

  Used to approximate the arc length of a bezier curve.
*/

static float get_arc_length(const float complex points[], const int n_points)
{
  float length = 0.0f;
  for(int i = 1; i < n_points; i++)
    length += cabsf(points[i-1] - points[i]);
  return length;
}

typedef struct
{
  int i;
  float length;
} restart_cookie_t;

/*
  Interpolate a point on a curve at a specified arc length.

  In a bezier curve the parameter t usually does not correspond to
  the arc length.
*/

static float complex point_at_arc_length(const float complex points[], const int n_points,
                                          const float arc_length, restart_cookie_t *restart)
{
  float length = restart ? restart->length : 0.0f;

  for(int i = restart ? restart->i : 1; i < n_points; i++)
  {
    const float prev_length = length;
    length += cabsf(points[i-1] - points[i]);

    if(length >= arc_length)
    {
      const float t = (arc_length - prev_length) / (length - prev_length);
      if(restart)
      {
        restart->i = i;
        restart->length = prev_length;
      }
      return cmix(points[i - 1], points[i], t);
    }
  }

  return points[n_points - 1];
}

/*
  Build a lookup table for the warp intensity.

  Lookup table for the warp intensity function: f(x). The warp
  intensity function determines how much a pixel is influenced by the
  warp depending from its distance from a central point.

  Boundary conditions: f(0) must be 1 and f(@a distance) must be 0.
  f'(0) and f'(@a distance) must both be 0 or we'll get artifacts on
  the picture.

  Implementation: a bezier curve with p0 = 0, 1 and p3 = 1, 0. p1 is
  defined by @a control1, 1 and p2 by @a control1, 0.  Because a
  bezier is parameterized on t, we have to reparameterize on x, which
  we do by linear interpolation.

  Octave code:

  t = linspace(0,1,100);
  grid;
  hold on;
  for steps = 0:0.1:1
    cpoints = [0,1; steps,1; steps,0; 1,0];
    bezier = cbezier2poly(cpoints);
    x = polyval(bezier(1,:), t);
    y = polyval(bezier(2,:), t);
    plot(t, interp1(x, y, t));
  end
  hold off;
*/

static float *build_lookup_table(const int distance, const float control1, const float control2)
{
  float complex *clookup = dt_alloc_align(64, sizeof(float complex) * (distance + 2));

  interpolate_cubic_bezier(I, control1 + I, control2, 1.0, clookup, distance + 2);

  // reparameterize bezier by x and keep only y values
  float *lookup = dt_alloc_align_float((size_t)(distance + 2));
  float *ptr = lookup;
  float complex *cptr = clookup + 1;
  const float complex *cptr_end = cptr + distance;
  const float step = 1.0f / (float) distance;
  float x = 0.0f;

  *ptr++ = 1.0f;
  for(int i = 1; i < distance && cptr < cptr_end; i++)
  {
    x += step;
    while(crealf(*cptr) < x && cptr < cptr_end)
      cptr++;
    const float dx1 = crealf(cptr[0] - cptr[-1]);
    const float dx2 = x - crealf(cptr[-1]);
    *ptr++ = cimagf(cptr[0]) +(dx2 / dx1) * (cimagf(cptr[0]) - cimagf(cptr[-1]));
  }
  *ptr++ = 0.0f;

  dt_free_align(clookup);
  return lookup;
}

static void compute_round_stamp_extent(cairo_rectangle_int_t *const restrict stamp_extent,
                                        const dt_liquify_warp_t *const restrict warp)
{

  const int iradius = round(cabsf(warp->radius - warp->point));
  assert(iradius > 0);

  stamp_extent->x = stamp_extent->y = -iradius;
  stamp_extent->x += crealf(warp->point);
  stamp_extent->y += cimagf(warp->point);
  stamp_extent->width = stamp_extent->height = 2 * iradius + 1;
}

/*
  Compute a round(circular) stamp.

  The stamp is a vector field of warp vectors around a center point.

  In a linear warp the center point gets a warp of @a strength, while
  points on the circumference of the circle get no warp at all.
  Between center and circumference the warp magnitude tapers off
  following a curve (see: build_lookup_table()).

  Note that when applying a linear stamp to a path, we will first rotate its
  vectors into the direction of the path.

  In a radial warp the center point and the points on the
  circumference get no warp. Between center and circumference the
  warp magnitude follows a curve with maximum at radius / 0.5

  Our stamp is stored in a rectangular region.
*/

static void build_round_stamp(float complex **pstamp,
                               cairo_rectangle_int_t *const restrict stamp_extent,
                               const dt_liquify_warp_t *const restrict warp)
{
  const int iradius = round(cabsf(warp->radius - warp->point));
  assert(iradius > 0);

  stamp_extent->x = stamp_extent->y = -iradius;
  stamp_extent->width = stamp_extent->height = 2 * iradius + 1;

  // 0.5 is factored in so the warp starts to degenerate when the
  // strength arrow crosses the warp radius.
  float complex strength = 0.5f * (warp->strength - warp->point);
  strength = (warp->status & DT_LIQUIFY_STATUS_INTERPOLATED) ?
    (strength * STAMP_RELOCATION) : strength;
  const float abs_strength = cabsf(strength);

  float complex *restrict stamp =
    calloc(sizeof(float complex), (size_t)stamp_extent->width * stamp_extent->height);

  // lookup table: map of distance from center point => warp
  const int table_size = iradius * LOOKUP_OVERSAMPLE;
  const float *const restrict lookup_table = build_lookup_table(table_size, warp->control1, warp->control2);

  // points into buffer at the center of the circle
  float complex *const center = stamp + 2 * iradius * iradius + 2 * iradius;

  // The expensive operation here is hypotf ().  By dividing the
  // circle in quadrants and doing only the inside we have to calculate
  // hypotf only for PI / 16 = 0.196 of the stamp area.
  // We don't do octants to avoid false sharing of cache lines between threads.
  
  #if defined(_OPENMP)
  #pragma omp parallel for schedule(static) default(none) \
    dt_omp_firstprivate(iradius, strength, abs_strength, table_size)   \
    dt_omp_sharedconst(center, warp, stamp_extent, lookup_table, LOOKUP_OVERSAMPLE)
  #endif

  for(int y = 0; y <= iradius; y++)
  {
    for(int x = 0; x <= iradius; x++)
    {
      const float dist = sqrtf(x*x + y*y); // faster than hypotf(), and we know we won't have overflow or denormals
      const int idist = round(dist * LOOKUP_OVERSAMPLE);
      if(idist >= table_size)
        // idist will only grow bigger in this row
        break;

      // pointers into the 4 quadrants of the circle
      // quadrant count is ccw from positive x-axis
      float complex *const q1 = center - y * stamp_extent->width + x;
      float complex *const q2 = center - y * stamp_extent->width - x;
      float complex *const q3 = center + y * stamp_extent->width - x;
      float complex *const q4 = center + y * stamp_extent->width + x;

      float abs_lookup = abs_strength * lookup_table[idist] / iradius;

      switch(warp->type)
      {
         case DT_LIQUIFY_WARP_TYPE_RADIAL_GROW:
           *q1 = abs_lookup * ( x - y * I);
           *q2 = abs_lookup * (-x - y * I);
           *q3 = abs_lookup * (-x + y * I);
           *q4 = abs_lookup * ( x + y * I);
           break;

         case DT_LIQUIFY_WARP_TYPE_RADIAL_SHRINK:
           *q1 = -abs_lookup * ( x - y * I);
           *q2 = -abs_lookup * (-x - y * I);
           *q3 = -abs_lookup * (-x + y * I);
           *q4 = -abs_lookup * ( x + y * I);
           break;

         default:
           *q1 = *q2 = *q3 = *q4 = strength * lookup_table[idist];
           break;
      }
    }
  }

  dt_free_align((void *) lookup_table);
  *pstamp = stamp;
}

/*
  Applies a stamp at a specified position.

  Applies a stamp at the position specified by @a point and adds the
  resulting vector field to the global distortion map @a global_map.

  The global distortion map is a map of relative pixel displacements
  encompassing all our paths.
*/

static void add_to_global_distortion_map(float complex *global_map,
                                          const cairo_rectangle_int_t *const restrict global_map_extent,
                                          const dt_liquify_warp_t *const restrict warp,
                                          const float complex *const restrict stamp,
                                          const cairo_rectangle_int_t *stamp_extent)
{
  cairo_rectangle_int_t mmext = *stamp_extent;
  mmext.x += (int) round(crealf(warp->point));
  mmext.y += (int) round(cimagf(warp->point));
  cairo_rectangle_int_t cmmext = mmext;
  cairo_region_t *mmreg = cairo_region_create_rectangle(&mmext);
  cairo_region_intersect_rectangle(mmreg, global_map_extent);
  cairo_region_get_extents(mmreg, &cmmext);
  free(mmreg);

  #ifdef _OPENMP
  #pragma omp parallel for schedule (static) default (shared)
  #endif

  for(int y = cmmext.y; y < cmmext.y + cmmext.height; y++)
  {
    const float complex *const srcrow = stamp + ((y - mmext.y) * mmext.width);
    float complex *const destrow = global_map + ((y - global_map_extent->y) * global_map_extent->width);

    for(int x = cmmext.x; x < cmmext.x + cmmext.width; x++)
    {
      destrow[x - global_map_extent->x] -= srcrow[x - mmext.x];
    }
  }
}

/*
  Applies the global distortion map to the picture.  The distortion
  map maps points to the position from where the new color of the
  point should be sampled from.  The distortion map is in relative
  device coords.
*/

static void _apply_global_distortion_map(struct dt_iop_module_t *module,
                                         dt_dev_pixelpipe_iop_t *piece,
                                         const float *const restrict in,
                                         float *const restrict out,
                                         const dt_iop_roi_t *const roi_in,
                                         const dt_iop_roi_t *const roi_out,
                                         const float complex *const map,
                                         const cairo_rectangle_int_t *extent)
{
  const int ch = piece->colors;
  const int ch_width = ch * roi_in->width;
  const struct dt_interpolation * const interpolation =
    dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);

  #ifdef _OPENMP
  #pragma omp parallel for schedule (static) default (shared)
  #endif

  for(int y = extent->y; y < extent->y + extent->height; y++)
  {
    // point inside roi_out ?
    if(y >= roi_out->y && y < roi_out->y + roi_out->height)
    {
      const float complex *row = map + (y - extent->y) * extent->width;
      float* out_sample = out + ((y - roi_out->y) * roi_out->width +
                               extent->x - roi_out->x) * ch;
      for(int x = extent->x; x < extent->x + extent->width; x++)
      {
        if(
          // point inside roi_out ?
          (x >= roi_out->x && x < roi_out->x + roi_out->width) &&
          // point actually warped ?
          (*row != 0))
        {
          if(ch == 1)
            *out_sample = dt_interpolation_compute_sample(interpolation,
                                                          in,
                                                          x + crealf(*row) - roi_in->x,
                                                          y + cimagf(*row) - roi_in->y,
                                                          roi_in->width,
                                                          roi_in->height,
                                                          ch,
                                                          ch_width);
          else
            dt_interpolation_compute_pixel4c(
              interpolation,
              in,
              out_sample,
              x + crealf(*row) - roi_in->x,
              y + cimagf(*row) - roi_in->y,
              roi_in->width,
              roi_in->height,
              ch_width);

        }
        ++row;
        out_sample += ch;
      }
    }
  }
}

// calculate the map extent.

static GSList *_get_map_extent(const dt_iop_roi_t *roi_out,
                               const GList *interpolated,
                               cairo_rectangle_int_t *map_extent)
{
  const cairo_rectangle_int_t roi_out_rect = { roi_out->x, roi_out->y, roi_out->width, roi_out->height };
  cairo_region_t *roi_out_region = cairo_region_create_rectangle(&roi_out_rect);
  cairo_region_t *map_region = cairo_region_create();
  GSList *in_roi = NULL;

  for(const GList *i = interpolated; i; i = g_list_next(i))
  {
    const dt_liquify_warp_t *warp = ((dt_liquify_warp_t *) i->data);
    cairo_rectangle_int_t r;
    compute_round_stamp_extent(&r, warp);
    // add extent if not entirely outside the roi
    if(cairo_region_contains_rectangle(roi_out_region, &r) != CAIRO_REGION_OVERLAP_OUT)
    {
      cairo_region_union_rectangle(map_region, &r);
      in_roi = g_slist_prepend(in_roi, i->data);
    }
  }

  // return the paths and the extent of all paths
  cairo_region_get_extents(map_region, map_extent);
  cairo_region_destroy(map_region);
  cairo_region_destroy(roi_out_region);

  return g_slist_reverse(in_roi);
}

static float complex *create_global_distortion_map(const cairo_rectangle_int_t *map_extent,
                                                   const GSList *interpolated,
                                                   gboolean inverted)
{
  const int mapsize = map_extent->width * map_extent->height;
  if(mapsize == 0)
  {
    // there are no pixels for which we need distortion info, so return right away
    // caller will see the NULL and bypass any further processing of the points it wants to distort
    return NULL;
  }

  // allocate distortion map big enough to contain all paths
  float complex *map = dt_alloc_align(64, sizeof(float complex) * mapsize);
  memset(map, 0, sizeof(float complex) * mapsize);

  // build map
  for(const GSList *i = interpolated; i; i = g_slist_next(i))
  {
    const dt_liquify_warp_t *warp = ((dt_liquify_warp_t *) i->data);
    float complex *stamp = NULL;
    cairo_rectangle_int_t r;
    build_round_stamp(&stamp, &r, warp);
    add_to_global_distortion_map(map, map_extent, warp, stamp, &r);
    free((void *) stamp);
  }

  if(inverted)
  {
    float complex * const imap = dt_alloc_align(64, sizeof(float complex) * mapsize);
    memset(imap, 0, sizeof(float complex) * mapsize);

    // copy map into imap(inverted map).
    // imap [ n + dx(map[n]) , n + dy(map[n]) ] = -map[n]

    #ifdef _OPENMP
    #pragma omp parallel for schedule (static) default (shared)
    #endif

    for(int y = 0; y <  map_extent->height; y++)
    {
      const float complex *const row = map + y * map_extent->width;
      for(int x = 0; x < map_extent->width; x++)
      {
        const float complex d = row[x];
        // compute new position (nx,ny) given the displacement d
        const int nx = x + (int)crealf(d);
        const int ny = y + (int)cimagf(d);

        // if the point falls into the extent, set it
        if(nx>0 && nx<map_extent->width && ny>0 && ny<map_extent->height)
          imap[nx + ny * map_extent->width] = -d;
      }
    }

    dt_free_align((void *) map);

    // now just do a pass to avoid gap with a displacement of zero, note that we do not need high
    // precision here as the inverted distortion mask is only used to compute a final displacement
    // of points.

    #ifdef _OPENMP
    #pragma omp parallel for schedule (static) default (shared)
    #endif

    for(int y = 0; y <  map_extent->height; y++)
    {
      float complex *const row = imap + y * map_extent->width;
      float complex last[2] = { 0, 0 };
      for(int x = 0; x < map_extent->width / 2 + 1; x++)
      {
        float complex *cl = row + x;
        float complex *cr = row + map_extent->width - x;
        if(x!=0)
        {
          if(*cl == 0) *cl = last[0];
          if(*cr == 0) *cr = last[1];
        }
        last[0] = *cl; last[1] = *cr;
      }
    }

    map = imap;
  }
  return map;
}

static void _build_global_distortion_map(struct dt_iop_module_t *module,
                                         const dt_dev_pixelpipe_iop_t *piece,
                                         const float scale,
                                         const gboolean from_distort_transform,
                                         const dt_iop_roi_t *roi,
                                         cairo_rectangle_int_t *map_extent,
                                         const gboolean inverted,
                                         float complex **map)
{
  // copy params
  dt_iop_liquify_params_t copy_params;
  memcpy(&copy_params, (dt_iop_liquify_params_t *)piece->data, sizeof(dt_iop_liquify_params_t));

  distort_paths_raw_to_piece(module, piece->pipe, scale, &copy_params, from_distort_transform);

  GList *interpolated = interpolate_paths(&copy_params);
  GSList *interpolated_in_roi = _get_map_extent(roi, interpolated, map_extent);

  if(map)
    *map = create_global_distortion_map(map_extent, interpolated_in_roi, inverted);

  g_slist_free(interpolated_in_roi);
  g_list_free_full(interpolated, free);
}

// 1st pass: how large would the output be, given this input roi?
// this is always called with the full buffer before processing.
void modify_roi_out(struct dt_iop_module_t *module,
                     struct dt_dev_pixelpipe_iop_t *piece,
                     dt_iop_roi_t *roi_out,
                     const dt_iop_roi_t *roi_in)
{
  // output is same size as input
  *roi_out = *roi_in;
}

// 2nd pass: which roi would this operation need as input to fill the given output region?
void modify_roi_in(struct dt_iop_module_t *module,
                    struct dt_dev_pixelpipe_iop_t *piece,
                    const dt_iop_roi_t *roi_out,
                    dt_iop_roi_t *roi_in)
{
  // Because we move pixels, and we may have to sample a pixel from
  // outside roi_in, we need to expand roi_in to contain all our
  // paths.  But we may ignore paths completely outside of roi_out.

  *roi_in = *roi_out;

  cairo_rectangle_int_t extent;
  _build_global_distortion_map(module, piece, roi_in->scale, FALSE, roi_out, &extent, FALSE, NULL);
  cairo_rectangle_int_t pipe_rect =
    {
      0,
      0,
      lroundf(piece->buf_in.width * roi_in->scale),
      lroundf(piece->buf_in.height * roi_in->scale)
    };

  cairo_rectangle_int_t roi_in_rect =
    {
      roi_in->x,
      roi_in->y,
      roi_in->width,
      roi_in->height
    };
  cairo_region_t *roi_in_region = cairo_region_create_rectangle(&roi_in_rect);

  // (eventually) extend roi_in
  cairo_region_union_rectangle(roi_in_region, &extent);
  // and clamp to pipe extent
  cairo_region_intersect_rectangle(roi_in_region, &pipe_rect);

  // write new extent to roi_in
  cairo_region_get_extents(roi_in_region, &roi_in_rect);
  roi_in->x = roi_in_rect.x;
  roi_in->y = roi_in_rect.y;
  roi_in->width  = roi_in_rect.width;
  roi_in->height = roi_in_rect.height;

  // cleanup
  cairo_region_destroy(roi_in_region);
}

static int _distort_xtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *const restrict points, const size_t points_count,
                               const gboolean inverted)
{
  const float scale = piece->iscale;

  // compute the extent of all points (all computations are done in RAW coordinate)
  float xmin = FLT_MAX, xmax = FLT_MIN, ymin = FLT_MAX, ymax = FLT_MIN;

#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(points_count, points, scale) \
    schedule(simd:static) if(points_count > 100)          \
    reduction(min:xmin, ymin) reduction(max:xmax, ymax)
#endif
  for(size_t i = 0; i < points_count * 2; i += 2)
  {
    const float x = points[i] * scale;
    const float y = points[i + 1] * scale;
    xmin = fmin(xmin, x);
    xmax = fmax(xmax, x);
    ymin = fmin(ymin, y);
    ymax = fmax(ymax, y);
  }

  cairo_rectangle_int_t extent = { .x = (int)(xmin - .5), .y = (int)(ymin - .5),
                                   .width = (int)(xmax - xmin + 2.5), .height = (int)(ymax - ymin + 2.5) };

  if(extent.width > 0 && extent.height > 0)
  {
    // we need to adjust the extent to be the union enclosing all the
    // points (currently in extent) and the warps that are in
    // (possibly partly) in this same region.

    dt_iop_roi_t roi_in = { .x = extent.x, .y = extent.y, .width = extent.width, .height = extent.height };

    float complex *map = NULL;
    _build_global_distortion_map(self, piece, scale, TRUE, &roi_in, &extent, inverted, &map);

    if(map == NULL) return 0;

    const int map_size =  extent.width * extent.height;
    const int x_last = extent.x + extent.width;
    const int y_last = extent.y + extent.height;

    // apply distortion to all points (this is a simple displacement given by a vector at this same point in the map)
#ifdef _OPENMP
#pragma omp parallel for default(none) \
    dt_omp_firstprivate(points_count, points, scale, extent, map, map_size, y_last, x_last) \
    schedule(static) if(points_count > 100)
#endif
    for(size_t i = 0; i < points_count; i++)
    {
      float *px = &points[i*2];
      float *py = &points[i*2+1];
      const float x = *px * scale;
      const float y = *py * scale;
      const int map_offset = ((int)(x - 0.5) - extent.x) + ((int)(y - 0.5) - extent.y) * extent.width;

      if(x >= extent.x && x < x_last && y >= extent.y && y < y_last && map_offset >= 0 && map_offset < map_size)
      {
        const float complex dist = map[map_offset] / scale;
        *px += crealf(dist);
        *py += cimagf(dist);
      }
    }

    dt_free_align((void *) map);
  }

  return 1;
}

static void start_drag(dt_iop_liquify_gui_data_t *g, dt_liquify_layer_enum_t layer, dt_liquify_path_data_t *elem)
{
  g->dragging.layer = layer;
  g->dragging.elem = elem;
}

static void end_drag(dt_iop_liquify_gui_data_t *g)
{
  g->dragging = NOWHERE;
}

static gboolean is_dragging(const dt_iop_liquify_gui_data_t *g)
{
  return g->dragging.elem != NULL;
}

int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *const restrict points, size_t points_count)
{
  return _distort_xtransform(self, piece, points, points_count, TRUE);
}

int distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *const restrict points, size_t points_count)
{
  return _distort_xtransform(self, piece, points, points_count, FALSE);
}

void distort_mask(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const float *const in,
                  float *const out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // 1. copy the whole image (we'll change only a small part of it)

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(in, out, roi_in, roi_out) \
  schedule(static)
#endif
  for(int i = 0; i < roi_out->height; i++)
  {
    float *destrow = out + (size_t) i * roi_out->width;
    const float *srcrow = in + (size_t) (roi_in->width * (i + roi_out->y - roi_in->y) + roi_out->x - roi_in->x);

    memcpy(destrow, srcrow, sizeof(float) * roi_out->width);
  }

  // 2. build the distortion map

  cairo_rectangle_int_t map_extent;
  float complex *map = NULL;
  _build_global_distortion_map(self, piece, roi_in->scale, FALSE, roi_out, &map_extent, FALSE, &map);

  if(map == NULL)
    return;

  // 3. apply the map

  if(map_extent.width != 0 && map_extent.height != 0)
  {
    const int ch = piece->colors;
    piece->colors = 1;
    _apply_global_distortion_map(self, piece, in, out, roi_in, roi_out, map, &map_extent);
    piece->colors = ch;
  }

  dt_free_align((void *) map);

}

void process(struct dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, const void *const in,
             void *const out, const dt_iop_roi_t *const roi_in, const dt_iop_roi_t *const roi_out)
{
  // 1. copy the whole image (we'll change only a small part of it)

  const int ch = piece->colors;
  assert(ch == 4);

  const int height = MIN(roi_in->height, roi_out->height);
  const int width = MIN(roi_in->width, roi_out->width);

#ifdef _OPENMP
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(ch, height, in, out, roi_in, roi_out, width) \
  schedule(static)
#endif
  for(int i = 0; i < height; i++)
  {
    float *destrow = (float *)out + (size_t)ch * i * roi_out->width;
    const float *srcrow = (float *)in + (size_t)ch * (roi_in->width * (i + roi_out->y - roi_in->y) +
                                                       roi_out->x - roi_in->x);

    memcpy(destrow, srcrow, sizeof(float) * ch * width);
  }

  // 2. build the distortion map

  cairo_rectangle_int_t map_extent;
  float complex *map = NULL;

  _build_global_distortion_map(module, piece, roi_in->scale, FALSE, roi_out, &map_extent, FALSE, &map);

  if(map == NULL)
    return;

  // 3. apply the map

  if(map_extent.width != 0 && map_extent.height != 0)
    _apply_global_distortion_map(module, piece, in, out, roi_in, roi_out, map, &map_extent);

  dt_free_align((void *)map);
}

#ifdef HAVE_OPENCL

// compute lanczos kernel. See: https://en.wikipedia.org/wiki/Lanczos_resampling#Lanczos_kernel

static inline float lanczos(const float a, const float x)
{
  if(fabsf(x) >= a) return 0.0f;
  if(fabsf(x) < FLT_EPSILON) return 1.0f;

  return (a * sinf(DT_M_PI_F * x) * sinf(DT_M_PI_F * x / a)) / (DT_M_PI_F * DT_M_PI_F * x * x);
}

// compute bicubic kernel. See: https://en.wikipedia.org/wiki/Bicubic_interpolation#Bicubic_convolution_algorithm

static inline float bicubic(const float a, const float x)
{
  const float absx = fabsf(x);
  if(absx <= 1) return ((a + 2) * absx - (a + 3)) * absx * absx + 1;
  if(absx < 2) return ((a * absx - 5 * a) * absx + 8 * a) * absx - 4 * a;
  return 0.0f;
}

typedef struct
{
  int size;
  int resolution;
} dt_liquify_kernel_descriptor_t;

typedef cl_mem cl_mem_t;
typedef cl_int cl_int_t;

static cl_int_t _apply_global_distortion_map_cl(struct dt_iop_module_t *module,
                                                dt_dev_pixelpipe_iop_t *piece,
                                                const cl_mem_t dev_in,
                                                const cl_mem_t dev_out,
                                                const dt_iop_roi_t *roi_in,
                                                const dt_iop_roi_t *roi_out,
                                                const float complex *map,
                                                const cairo_rectangle_int_t *map_extent)
{
  cl_int_t err = CL_MEM_OBJECT_ALLOCATION_FAILURE;

  dt_iop_liquify_global_data_t *gd = (dt_iop_liquify_global_data_t *)module->global_data;
  const int devid = piece->pipe->devid;

  const struct dt_interpolation* interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF_WARP);
  dt_liquify_kernel_descriptor_t kdesc = { .size = 0, .resolution = 100 };
  float *k = NULL;

  switch(interpolation->id)
  {
     case DT_INTERPOLATION_BILINEAR:
       kdesc.size = 1;
       kdesc.resolution = 1;
       k = malloc(sizeof(float) * 2);
       k[0] = 1.0f;
       k[1] = 0.0f;
       break;
     case DT_INTERPOLATION_BICUBIC:
       kdesc.size = 2;
       k = malloc(sizeof(float) * ((size_t)kdesc.size * kdesc.resolution + 1));
       for(int i = 0; i <= kdesc.size * kdesc.resolution; ++i)
         k[i] = bicubic(0.5f, (float) i / kdesc.resolution);
       break;
     case DT_INTERPOLATION_LANCZOS2:
       kdesc.size = 2;
       k = malloc(sizeof(float) * ((size_t)kdesc.size * kdesc.resolution + 1));
       for(int i = 0; i <= kdesc.size * kdesc.resolution; ++i)
         k[i] = lanczos(2, (float) i / kdesc.resolution);
       break;
     case DT_INTERPOLATION_LANCZOS3:
       kdesc.size = 3;
       k = malloc(sizeof(float) * ((size_t)kdesc.size * kdesc.resolution + 1));
       for(int i = 0; i <= kdesc.size * kdesc.resolution; ++i)
         k[i] = lanczos(3, (float) i / kdesc.resolution);
       break;
     default:
       return FALSE;
  }

  cl_mem_t dev_roi_in = dt_opencl_copy_host_to_device_constant
    (devid, sizeof(dt_iop_roi_t), (void *) roi_in);

  cl_mem_t dev_roi_out = dt_opencl_copy_host_to_device_constant
    (devid, sizeof(dt_iop_roi_t), (void *) roi_out);

  cl_mem_t dev_map = dt_opencl_copy_host_to_device_constant
    (devid, sizeof(float complex) * map_extent->width * map_extent->height, (void *) map);

  cl_mem_t dev_map_extent = dt_opencl_copy_host_to_device_constant
    (devid, sizeof(cairo_rectangle_int_t), (void *) map_extent);

  cl_mem_t dev_kdesc = dt_opencl_copy_host_to_device_constant
    (devid, sizeof(dt_liquify_kernel_descriptor_t), (void *) &kdesc);

  cl_mem_t dev_kernel = dt_opencl_copy_host_to_device_constant
    (devid, sizeof(float) * (kdesc.size * kdesc.resolution  + 1), (void *) k);

  if(dev_roi_in == NULL || dev_roi_out == NULL || dev_map == NULL || dev_map_extent == NULL
      || dev_kdesc == NULL || dev_kernel == NULL)
    goto error;

  err = dt_opencl_enqueue_kernel_2d_args(devid, gd->warp_kernel, map_extent->width, map_extent->height,
    CLARG(dev_in), CLARG(dev_out), CLARG(dev_roi_in), CLARG(dev_roi_out), CLARG(dev_map), CLARG(dev_map_extent),
    CLARG(dev_kdesc), CLARG(dev_kernel));

error:

  dt_opencl_release_mem_object(dev_kernel);
  dt_opencl_release_mem_object(dev_kdesc);
  dt_opencl_release_mem_object(dev_map_extent);
  dt_opencl_release_mem_object(dev_map);
  dt_opencl_release_mem_object(dev_roi_out);
  dt_opencl_release_mem_object(dev_roi_in);
  if(k) free(k);

  return err;
}

int process_cl(struct dt_iop_module_t *module,
                dt_dev_pixelpipe_iop_t *piece,
                const cl_mem_t dev_in,
                const cl_mem_t dev_out,
                const dt_iop_roi_t *roi_in,
                const dt_iop_roi_t *roi_out)
{
  cl_int_t err = DT_OPENCL_DEFAULT_ERROR;
  const int devid = piece->pipe->devid;
  const int height = MIN(roi_in->height, roi_out->height);
  const int width = MIN(roi_in->width, roi_out->width);

  // 1. copy the whole image (we'll change only a small part of it)
  {
    size_t src[]    = { roi_out->x - roi_in->x, roi_out->y - roi_in->y, 0 };
    size_t dest[]   = { 0, 0, 0 };
    size_t extent[] = { width, height, 1 };
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, src, dest, extent);
    if(err != CL_SUCCESS) goto error;
  }

  // 2. build the distortion map
  cairo_rectangle_int_t map_extent;
  float complex *map = NULL;
  _build_global_distortion_map(module, piece, roi_in->scale, FALSE, roi_out, &map_extent, FALSE, &map);

  if(map == NULL)
    return TRUE;

  // 3. apply the map
  if(map_extent.width != 0 && map_extent.height != 0)
    err = _apply_global_distortion_map_cl(module, piece, dev_in, dev_out, roi_in, roi_out, map, &map_extent);
  dt_free_align((void *) map);
  if(err != CL_SUCCESS) goto error;

  return TRUE;

error:
  dt_print(DT_DEBUG_OPENCL, "[opencl_liquify] couldn't enqueue kernel! %s\n", cl_errstr(err));
  return FALSE;
}

#endif

void init_global(dt_iop_module_so_t *module)
{
  // called once at startup
  const int program = 17; // from programs.conf
  dt_iop_liquify_global_data_t *gd = (dt_iop_liquify_global_data_t *) malloc(sizeof(dt_iop_liquify_global_data_t));
  module->data = gd;
  gd->warp_kernel = dt_opencl_create_kernel(program, "warp_kernel");
}

void cleanup_global(dt_iop_module_so_t *module)
{
  // called once at shutdown
  dt_iop_liquify_global_data_t *gd = (dt_iop_liquify_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->warp_kernel);
  free(module->data);
  module->data = NULL;
}

// calculate the dot product of 2 vectors.

static float cdot(const float complex p0, const float complex p1)
{
#ifdef FP_FAST_FMA
  return fma(crealf(p0), crealf(p1), cimagf(p0) * cimagf(p1));
#else
  return crealf(p0) * crealf(p1) + cimagf(p0) * cimagf(p1);
#endif
}

static void draw_rectangle(cairo_t *cr, const float complex pt, const double theta, const double size)
{
  const double x = creal(pt), y = cimag(pt);
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_rotate(cr, theta);
  cairo_rectangle(cr, -size / 2.0, -size / 2.0, size, size);
  cairo_restore(cr);
}

static void draw_triangle(cairo_t *cr, const float complex pt, const double theta, const double size)
{
  const double x = creal(pt), y = cimag(pt);
  cairo_save(cr);
  cairo_translate(cr, x, y);
  cairo_rotate(cr, theta);
  cairo_move_to(cr, -size, -size / 2.0);
  cairo_line_to(cr, 0,     0          );
  cairo_line_to(cr, -size, +size / 2.0);
  cairo_close_path(cr);
  cairo_restore(cr);
}

static void draw_circle(cairo_t *cr, const float complex pt, const double diameter)
{
  const double x = creal(pt), y = cimag(pt);
  cairo_save(cr);
  cairo_new_sub_path(cr);
  cairo_arc(cr, x, y, diameter / 2.0, 0, 2 * DT_M_PI);
  cairo_restore(cr);
}

static void set_source_rgba(cairo_t *cr, dt_liquify_rgba_t rgba)
{
  cairo_set_source_rgba(cr, rgba.red, rgba.green, rgba.blue, rgba.alpha);
}

static float get_ui_width(const float scale, const dt_liquify_ui_width_enum_t w)
{
  assert(w >= 0 && w < DT_LIQUIFY_UI_WIDTH_LAST);
  return scale * DT_PIXEL_APPLY_DPI(dt_liquify_ui_widths[w]);
}

#define GET_UI_WIDTH(a) (get_ui_width(scale, DT_LIQUIFY_UI_WIDTH_##a))

static void set_line_width(cairo_t *cr, double scale, dt_liquify_ui_width_enum_t w)
{
  const double width = get_ui_width(scale, w);
  cairo_set_line_width(cr, width);
}

static gboolean detect_drag(const dt_iop_liquify_gui_data_t *g, const double scale, const float complex pt)
{
  const float pr_d = darktable.develop->preview_downsampling;

  // g->last_button1_pressed_pos is valid only while BUTTON1 is down
  return g->last_button1_pressed_pos != -1.0 &&
    cabsf(pt - g->last_button1_pressed_pos) >= (GET_UI_WIDTH(MIN_DRAG) * pr_d / scale);
}

static void update_warp_count(struct dt_iop_module_t *module)
{
  const dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *)module->gui_data;
  const dt_iop_liquify_params_t *p = (dt_iop_liquify_params_t *)module->params;

  guint warp = 0, node = 0;
  for(int k=0; k<MAX_NODES; k++)
    if(p->nodes[k].header.type == DT_LIQUIFY_PATH_INVALIDATED)
      break;
    else
    {
      node++;
      if(p->nodes[k].header.type == DT_LIQUIFY_PATH_MOVE_TO_V1)
        warp++;
    }
  char str[10];
  snprintf(str, sizeof(str), "%d | %d", warp, node);
  gtk_label_set_text(g->label, str);
}

static GList *interpolate_paths(dt_iop_liquify_params_t *p)
{
  GList *l = NULL;
  for(int k=0; k<MAX_NODES; k++)
  {
    const dt_liquify_path_data_t *data = &p->nodes[k];
    if(data->header.type == DT_LIQUIFY_PATH_INVALIDATED)
      break;

    const float complex *p2 = &data->warp.point;
    const dt_liquify_warp_t *warp2 = &data->warp;

    if(data->header.type == DT_LIQUIFY_PATH_MOVE_TO_V1)
    {
      if(data->header.next == -1)
      {
        dt_liquify_warp_t *w = malloc(sizeof(dt_liquify_warp_t));
        *w = *warp2;
        l = g_list_append(l, w);
      }
      continue;
    }

    dt_liquify_path_data_t *prev = node_prev(p, data);
    const dt_liquify_warp_t *warp1 = &prev->warp;
    const float complex *p1 = &prev->warp.point;
    if(data->header.type == DT_LIQUIFY_PATH_LINE_TO_V1)
    {
      const float total_length = cabsf(*p1 - *p2);
      float arc_length = 0.0f;
      while(arc_length < total_length)
      {
        dt_liquify_warp_t *w = malloc(sizeof(dt_liquify_warp_t));
        const float t = arc_length / total_length;
        const float complex pt = cmix(*p1, *p2, t);
        mix_warps(w, warp1, warp2, pt, t);
        w->status = DT_LIQUIFY_STATUS_INTERPOLATED;
        arc_length += cabsf(w->radius - w->point) * STAMP_RELOCATION;
        l = g_list_append(l, w);
      }
      continue;
    }

    if(data->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
    {
      float complex *buffer = malloc(sizeof(float complex) * INTERPOLATION_POINTS);
      interpolate_cubic_bezier(*p1,
                                data->node.ctrl1,
                                data->node.ctrl2,
                                *p2,
                                buffer,
                                INTERPOLATION_POINTS);
      const float total_length = get_arc_length(buffer, INTERPOLATION_POINTS);
      float arc_length = 0.0f;
      restart_cookie_t restart = { 1, 0.0 };

      while(arc_length < total_length)
      {
        dt_liquify_warp_t *w = malloc(sizeof(dt_liquify_warp_t));
        const float t = arc_length / total_length;
        const float complex pt = point_at_arc_length(buffer, INTERPOLATION_POINTS, arc_length, &restart);
        mix_warps(w, warp1, warp2, pt, t);
        w->status = DT_LIQUIFY_STATUS_INTERPOLATED;
        arc_length += cabsf(w->radius - w->point) * STAMP_RELOCATION;
        l = g_list_append(l, w);
      }
      free((void *) buffer);
      continue;
    }
  }
  return l;
}

#define FG_COLOR     set_source_rgba(cr, fg_color)
#define BG_COLOR     set_source_rgba(cr, bg_color)
#define VERYTHINLINE set_line_width (cr, scale / 2.0f, DT_LIQUIFY_UI_WIDTH_THINLINE)
#define THINLINE     set_line_width (cr, scale, DT_LIQUIFY_UI_WIDTH_THINLINE)
#define THICKLINE    set_line_width (cr, scale, DT_LIQUIFY_UI_WIDTH_THICKLINE)

static void _draw_paths(dt_iop_module_t *module,
                        cairo_t *cr,
                        const float scale,
                        dt_iop_liquify_params_t *p,
                        GList *layers)
{
  const dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *)module->gui_data;

  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);

  // do not display any iterpolated items as slow when:
  //   - we are dragging (pan)
  //   - the button one is pressed
  //   - exception for DT_LIQUIFY_LAYER_STRENGTHPOINT where we want to see the
  //     interpolated strength lines.
  GList *interpolated = (is_dragging(g) || g->last_button1_pressed_pos != -1)
    && (g->last_hit.layer != DT_LIQUIFY_LAYER_STRENGTHPOINT)
    ? NULL
    : interpolate_paths(p);

  for(const GList *l = layers; l; l = g_list_next(l))
  {
    const dt_liquify_layer_enum_t layer = (dt_liquify_layer_enum_t) GPOINTER_TO_INT(l->data);
    dt_liquify_rgba_t fg_color = dt_liquify_layers[layer].fg;
    dt_liquify_rgba_t bg_color = dt_liquify_layers[layer].bg;

    if(dt_liquify_layers[layer].opacity < 1.0)
      cairo_push_group(cr);

    for(int k=0; k<MAX_NODES; k++)
    {
      // this is an empty bin, old invalidated node, nothing more to do
      if(p->nodes[k].header.type == DT_LIQUIFY_PATH_INVALIDATED)
        break;

      dt_liquify_path_data_t *data = &p->nodes[k];
      const dt_liquify_path_data_t *prev = node_prev(p, data);

      if((dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_NODE_SELECTED)
         && !data->header.selected)
        continue;

      if((dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_PREV_SELECTED)
         && (!prev || !prev->header.selected))
        continue;

      fg_color = dt_liquify_layers[layer].fg;
      bg_color = dt_liquify_layers[layer].bg;

      if(data->header.selected == layer)
        fg_color = DT_LIQUIFY_COLOR_SELECTED;

      if(data->header.hovered == dt_liquify_layers[layer].hover_master)
        fg_color = DT_LIQUIFY_COLOR_HOVER;

      cairo_new_path(cr);

      const float complex point = data->warp.point;

      if(data->header.type == DT_LIQUIFY_PATH_MOVE_TO_V1)
        cairo_move_to(cr, crealf(point), cimagf(point));

      if(layer == DT_LIQUIFY_LAYER_RADIUS)
      {
        for(const GList *i = interpolated; i; i = g_list_next(i))
        {
          const dt_liquify_warp_t *pwarp = ((dt_liquify_warp_t *) i->data);
          draw_circle(cr, pwarp->point, 2.0f * cabsf(pwarp->radius - pwarp->point));
        }
        draw_circle(cr, point, 2.0f * cabsf(data->warp.radius - data->warp.point));
        FG_COLOR;
        cairo_fill(cr);
      }
      else if(layer == DT_LIQUIFY_LAYER_HARDNESS1)
      {
        for(const GList *i = interpolated; i; i = g_list_next(i))
        {
          const dt_liquify_warp_t *pwarp = ((dt_liquify_warp_t *) i->data);
          draw_circle(cr, pwarp->point, 2.0f * cabsf(pwarp->radius - pwarp->point) * pwarp->control1);
        }
        FG_COLOR;
        cairo_fill(cr);
      }
      else if(layer == DT_LIQUIFY_LAYER_HARDNESS2)
      {
        for(const GList *i = interpolated; i; i = g_list_next(i))
        {
          const dt_liquify_warp_t *pwarp = ((dt_liquify_warp_t *) i->data);
          draw_circle(cr, pwarp->point, 2.0f * cabsf(pwarp->radius - pwarp->point) * pwarp->control2);
        }
        FG_COLOR;
        cairo_fill(cr);
      }
      else if(layer == DT_LIQUIFY_LAYER_WARPS)
      {
        VERYTHINLINE; FG_COLOR;
        for(const GList *i = interpolated; i; i = g_list_next(i))
        {
          const dt_liquify_warp_t *pwarp = ((dt_liquify_warp_t *) i->data);
          cairo_move_to(cr, crealf(pwarp->point), cimagf(pwarp->point));
          cairo_line_to(cr, crealf(pwarp->strength), cimagf(pwarp->strength));
        }
        cairo_stroke(cr);

        for(const GList *i = interpolated; i; i = g_list_next(i))
        {
          const dt_liquify_warp_t *pwarp = ((dt_liquify_warp_t *) i->data);
          const float rot = get_rot(pwarp->type);
          draw_circle(cr, pwarp->point, GET_UI_WIDTH(GIZMO_SMALL));
          draw_triangle(cr, pwarp->strength,
                        cargf(pwarp->strength - pwarp->point) + rot,
                        GET_UI_WIDTH(GIZMO_SMALL) / 3.0);
        }
        BG_COLOR;
        cairo_fill_preserve(cr);
        FG_COLOR;
        cairo_stroke(cr);
      }
      else if(layer == DT_LIQUIFY_LAYER_PATH)
      {
        if((data->header.type == DT_LIQUIFY_PATH_LINE_TO_V1)
            || (data->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1))
        {
          assert(prev);
          cairo_move_to(cr, crealf(prev->warp.point), cimagf(prev->warp.point));
          if(data->header.type == DT_LIQUIFY_PATH_LINE_TO_V1)
            cairo_line_to(cr, crealf(point), cimagf(point));
          if(data->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
          {
            cairo_curve_to(cr, crealf(data->node.ctrl1), cimagf(data->node.ctrl1),
                            crealf(data->node.ctrl2), cimagf(data->node.ctrl2),
                            crealf(point), cimagf(point));
          }
          THICKLINE; FG_COLOR;
          cairo_stroke_preserve(cr);
          THINLINE; BG_COLOR;
          cairo_stroke(cr);
        }
      }
      else if(layer == DT_LIQUIFY_LAYER_CENTERPOINT)
      {
        if(data->header.type == DT_LIQUIFY_PATH_MOVE_TO_V1
            || data->header.type == DT_LIQUIFY_PATH_LINE_TO_V1
            || data->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
        {
          const float w = GET_UI_WIDTH(GIZMO);
          switch(data->header.node_type)
          {
             case DT_LIQUIFY_NODE_TYPE_CUSP:
               draw_triangle(cr, point - w / 2.0 * I, -DT_M_PI / 2.0, w);
               break;
             case DT_LIQUIFY_NODE_TYPE_SMOOTH:
               draw_rectangle(cr, point, DT_M_PI / 4.0, w);
               break;
             case DT_LIQUIFY_NODE_TYPE_SYMMETRICAL:
               draw_rectangle(cr, point, 0, w);
               break;
             case DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH:
               draw_circle(cr, point, w);
               break;
             default:
               break;
          }
          THINLINE; BG_COLOR;
          cairo_fill_preserve(cr);
          FG_COLOR;
          cairo_stroke(cr);
        }
      }

      if(data->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
      {
        if(layer == DT_LIQUIFY_LAYER_CTRLPOINT1_HANDLE &&
            !(prev && prev->header.node_type == DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH))
        {
          THINLINE; FG_COLOR;
          cairo_move_to(cr, crealf(prev->warp.point), cimagf(prev->warp.point));
          cairo_line_to(cr, crealf(data->node.ctrl1), cimagf(data->node.ctrl1));
          cairo_stroke(cr);
        }
        if(layer == DT_LIQUIFY_LAYER_CTRLPOINT2_HANDLE &&
            data->header.node_type != DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH)
        {
          THINLINE; FG_COLOR;
          cairo_move_to(cr, crealf(data->warp.point), cimagf(data->warp.point));
          cairo_line_to(cr, crealf(data->node.ctrl2), cimagf(data->node.ctrl2));
          cairo_stroke(cr);
        }
        if(layer == DT_LIQUIFY_LAYER_CTRLPOINT1 &&
            !(prev && prev->header.node_type == DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH))
        {
          THINLINE; BG_COLOR;
          draw_circle(cr, data->node.ctrl1, GET_UI_WIDTH(GIZMO_SMALL));
          cairo_fill_preserve(cr);
          FG_COLOR;
          cairo_stroke(cr);
        }
        if(layer == DT_LIQUIFY_LAYER_CTRLPOINT2 &&
            data->header.node_type != DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH)
        {
          THINLINE; BG_COLOR;
          draw_circle(cr, data->node.ctrl2, GET_UI_WIDTH(GIZMO_SMALL));
          cairo_fill_preserve(cr);
          FG_COLOR;
          cairo_stroke(cr);
        }
      }

      const dt_liquify_warp_t *warp  = &data->warp;

      if(layer == DT_LIQUIFY_LAYER_RADIUSPOINT_HANDLE)
      {
        draw_circle(cr, point, 2.0 * cabsf(warp->radius - point));
        THICKLINE; FG_COLOR;
        cairo_stroke_preserve(cr);
        THINLINE; BG_COLOR;
        cairo_stroke(cr);
      }

      if(layer == DT_LIQUIFY_LAYER_RADIUSPOINT)
      {
        THINLINE; BG_COLOR;
        draw_circle(cr, warp->radius, GET_UI_WIDTH(GIZMO_SMALL));
        cairo_fill_preserve(cr);
        FG_COLOR;
        cairo_stroke(cr);
      }

      if(layer == DT_LIQUIFY_LAYER_HARDNESSPOINT1_HANDLE)
      {
        draw_circle(cr, point, 2.0 * cabsf(warp->radius - point) * warp->control1);
        THICKLINE; FG_COLOR;
        cairo_stroke_preserve(cr);
        THINLINE; BG_COLOR;
        cairo_stroke(cr);
      }

      if(layer == DT_LIQUIFY_LAYER_HARDNESSPOINT2_HANDLE)
      {
        draw_circle(cr, point, 2.0 * cabsf(warp->radius - point) * warp->control2);
        THICKLINE; FG_COLOR;
        cairo_stroke_preserve(cr);
        THINLINE; BG_COLOR;
        cairo_stroke(cr);
      }

      if(layer == DT_LIQUIFY_LAYER_HARDNESSPOINT1)
      {
        draw_triangle(cr, cmix(point, warp->radius, warp->control1),
                      cargf(warp->radius - point),
                      GET_UI_WIDTH(GIZMO_SMALL));
        THINLINE; BG_COLOR;
        cairo_fill_preserve(cr);
        FG_COLOR;
        cairo_stroke(cr);
      }

      if(layer == DT_LIQUIFY_LAYER_HARDNESSPOINT2)
      {
        draw_triangle(cr, cmix(point, warp->radius, warp->control2),
                      cargf(-(warp->radius - point)),
                      GET_UI_WIDTH(GIZMO_SMALL));
        THINLINE; BG_COLOR;
        cairo_fill_preserve(cr);
        FG_COLOR;
        cairo_stroke(cr);
      }

      if(layer == DT_LIQUIFY_LAYER_STRENGTHPOINT_HANDLE)
      {
        cairo_move_to(cr, crealf(point), cimagf(point));
        if(warp->type == DT_LIQUIFY_WARP_TYPE_LINEAR)
        {
          const float complex pt = cmix(point, warp->strength,
                                        1.0 - 0.5
                                        * (GET_UI_WIDTH(GIZMO_SMALL)
                                           / cabsf(warp->strength - point)));
          cairo_line_to(cr, crealf(pt), cimagf(pt));
        }
        else
          draw_circle(cr, point, 2.0 * cabsf(warp->strength - warp->point));
        THICKLINE; FG_COLOR;
        cairo_stroke_preserve(cr);
        THINLINE; BG_COLOR;
        cairo_stroke(cr);
      }

      if(layer == DT_LIQUIFY_LAYER_STRENGTHPOINT)
      {
        cairo_move_to(cr, crealf(warp->strength), cimagf(warp->strength));
        const float rot = get_rot(warp->type);
        draw_triangle(cr, warp->strength,
                      cargf(warp->strength - warp->point) + rot,
                      GET_UI_WIDTH(GIZMO_SMALL));
        THINLINE; BG_COLOR;
        cairo_fill_preserve(cr);
        FG_COLOR;
        cairo_stroke(cr);
      }
    }

    if(dt_liquify_layers[layer].opacity < 1.0)
    {
      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, dt_liquify_layers[layer].opacity);
    }
  }

  g_list_free_full(interpolated, free);
}

/*
  Find the nearest point on a cubic bezier curve.

  Return the curve parameter t of the point on a cubic bezier curve
  that is nearest to another arbitrary point.  Uses interpolation.

  FIXME: Implement a faster method, see:
  http://tog.acm.org/resources/GraphicsGems/gems/NearestPoint.c
*/

static float find_nearest_on_curve_t(const float complex p0,
                                      const float complex p1,
                                      const float complex p2,
                                      const float complex p3,
                                      const float complex x,
                                      const int n)
{
  float min_t = 0.0f, min_dist = cabsf(x - p0);

  for(int i = 0; i < n; i++)
  {
    const float t = (1.0 * i) / n;
    const float t1 = 1.0 - t;
    const float complex ip =
          t1 * t1 * t1 * p0 +
      3 * t1 * t1 * t  * p1 +
      3 * t1 * t  * t  * p2 +
          t  * t  * t  * p3;

    const float dist = cabsf(x - ip);
    if(dist < min_dist)
    {
      min_dist = dist;
      min_t = t;
    }
  }
  return min_t;
}

/*
  Find the nearest point on a line.

  Return the line parameter t of the point on a line that is nearest
  to another arbitrary point.
*/

static float find_nearest_on_line_t(const float complex p0, const float complex p1, const float complex x)
{
  // scalar projection
  const float b     = cabsf(p1 - p0);         // |b|
  const float dotab = cdot(x - p0, p1 - p0);  // |a| * |b| * cos(phi)
  return dotab / (b * b);                     // |a| / |b| * cos(phi)
}

// split a cubic bezier at t into two cubic beziers.

static void casteljau(const float complex *p0, float complex *p1, float complex *p2, float complex *p3, const float t)
{
  const float complex p01 = *p0 + (*p1 - *p0) * t;
  const float complex p12 = *p1 + (*p2 - *p1) * t;
  const float complex p23 = *p2 + (*p3 - *p2) * t;

  const float complex p012 = p01 + (p12 - p01) * t;
  const float complex p123 = p12 + (p23 - p12) * t;

  const float complex p0123 = p012 + (p123 - p012) * t;

  *p1 = p01;
  *p2 = p012;
  *p3 = p0123;
}

#define CHECK_HIT_PT(point)             \
  const float d = cabsf(point - (*pt)); \
  if(d < distance)                      \
  {                                     \
    distance = d;                       \
    hit->layer = layer;                 \
    hit->elem = data;                   \
  }

void _hit_paths(dt_iop_module_t *module,
                dt_iop_liquify_params_t *p,
                GList *layers,
                const float complex *pt,
                dt_liquify_hit_t *hit)
{
  float distance = FLT_MAX;

  for(const GList *l = layers; l; l = g_list_next(l))
  {
    const dt_liquify_layer_enum_t layer = (dt_liquify_layer_enum_t)GPOINTER_TO_INT(l->data);

    if((dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_HIT_TEST) == 0)
      continue;

    for(int k=0; k<MAX_NODES; k++)
    {
      dt_liquify_path_data_t *data = &p->nodes[k];
      const dt_liquify_path_data_t *prev = node_prev(p, data);

      if(p->nodes[k].header.type == DT_LIQUIFY_PATH_INVALIDATED)
        break;

      if((dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_NODE_SELECTED)
          && !data->header.selected)
        continue;

      if((dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_PREV_SELECTED)
          && (!prev || !prev->header.selected))
        continue;

      const dt_liquify_warp_t *warp  = &data->warp;
      const float complex point = data->warp.point;

      if(layer == DT_LIQUIFY_LAYER_PATH)
      {
        if(data->header.type == DT_LIQUIFY_PATH_LINE_TO_V1)
        {
          // remove 5% from start and end of line as non sensible area
          // this is to avoid wrong interaction for center point on both
          // sides.
          const float complex deadzone = (point - prev->warp.point) / 20.0f;
          const float complex lp1 = prev->warp.point + deadzone;
          const float complex lp2 = point - deadzone;
          const float t = find_nearest_on_line_t(lp1, lp2, *pt);

          if(t > 0.0f && t < 1.0f)
          {
            const float complex linepoint = cmix(lp1, lp2, t);
            CHECK_HIT_PT(linepoint);
          }
        }
        else if(data->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
        {
          // remove 5% from start and end of line as non sensible area
          // this is to avoid wrong interaction for center point on both
          // sides.
          const float complex deadzone = (point - prev->warp.point) / 20.0f;
          const float complex lp1 = prev->warp.point + deadzone;
          const float complex lp2 = point - deadzone;
          const float t = find_nearest_on_curve_t(lp1, data->node.ctrl1, data->node.ctrl2, lp2, *pt, INTERPOLATION_POINTS);

          if(t > 0.0f && t < 1.0f)
          {
            float complex curvepoint = lp2;
            float complex p1 = data->node.ctrl1;
            float complex p2 = data->node.ctrl2;
            casteljau(&lp1, &p1, &p2, &curvepoint, t);

            CHECK_HIT_PT(curvepoint);
          }
        }
      }
      else if(layer == DT_LIQUIFY_LAYER_CENTERPOINT)
      {
        if(data->header.type == DT_LIQUIFY_PATH_MOVE_TO_V1
            || data->header.type == DT_LIQUIFY_PATH_LINE_TO_V1
            || data->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
        {
          CHECK_HIT_PT(point);
        }
      }
      else if(layer == DT_LIQUIFY_LAYER_RADIUSPOINT)
      {
        CHECK_HIT_PT(warp->radius);
      }
      else if(layer == DT_LIQUIFY_LAYER_HARDNESSPOINT1)
      {
        CHECK_HIT_PT(cmix(point, warp->radius, warp->control1));
      }
      else if(layer == DT_LIQUIFY_LAYER_HARDNESSPOINT2)
      {
        CHECK_HIT_PT(cmix(point, warp->radius, warp->control2));
      }
      else if(layer == DT_LIQUIFY_LAYER_STRENGTHPOINT)
      {
        const float complex p = warp->point - warp->strength;
        CHECK_HIT_PT(warp->strength + (float)DT_PIXEL_APPLY_DPI(5) * (p / cabsf(p)));
      }

      if(data->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
      {
        if(layer == DT_LIQUIFY_LAYER_CTRLPOINT1
           && !(prev && prev->header.node_type == DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH))
        {
          CHECK_HIT_PT(data->node.ctrl1);
        }
        if(layer == DT_LIQUIFY_LAYER_CTRLPOINT2
           && data->header.node_type != DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH)
        {
          CHECK_HIT_PT(data->node.ctrl2);
        }
      }
    }
  }

  if(distance > DT_PIXEL_APPLY_DPI(25))
  {
    memcpy(hit, &NOWHERE, sizeof(dt_liquify_hit_t));
  }
}

static void draw_paths(struct dt_iop_module_t *module, cairo_t *cr, const float scale, dt_iop_liquify_params_t *params)
{
  const dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *)module->gui_data;
  GList *layers = NULL;

  for(dt_liquify_layer_enum_t layer = 0; layer < DT_LIQUIFY_LAYER_LAST; ++layer)
  {
    if(gtk_toggle_button_get_active(g->btn_point_tool)
        && (dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_POINT_TOOL))
      layers = g_list_prepend(layers, GINT_TO_POINTER(layer));
    if(gtk_toggle_button_get_active(g->btn_line_tool)
        && (dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_LINE_TOOL))
      layers = g_list_prepend(layers, GINT_TO_POINTER(layer));
    if(gtk_toggle_button_get_active(g->btn_curve_tool)
        && (dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_CURVE_TOOL))
      layers = g_list_prepend(layers, GINT_TO_POINTER(layer));
    if(gtk_toggle_button_get_active(g->btn_node_tool)
        && (dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_NODE_TOOL))
      layers = g_list_prepend(layers, GINT_TO_POINTER(layer));
  }
  layers = g_list_reverse(layers); // list was built in reverse order, so un-reverse it

  _draw_paths(module, cr, scale, params, layers);

  g_list_free(layers);
}

void _hit_test_paths(struct dt_iop_module_t *module,
                     dt_iop_liquify_params_t *params,
                     float complex pt,
                     dt_liquify_hit_t *hit)
{
  GList *layers = NULL;

  for(dt_liquify_layer_enum_t layer = 0; layer < DT_LIQUIFY_LAYER_LAST; ++layer)
  {
    if(dt_liquify_layers[layer].flags & DT_LIQUIFY_LAYER_FLAG_HIT_TEST)
      layers = g_list_prepend(layers, GINT_TO_POINTER(layer));
  }
  layers = g_list_reverse(layers); // list was built in reverse order, so un-reverse it

  _hit_paths(module, params, layers, &pt, hit);
  g_list_free(layers);
}

/**
 * Smooth a bezier spline through prescribed points.
 *
 * Smooth a bezier spline through prescribed points by solving a
 * linear system.  First we build a tridiagonal matrix and then we
 * solve it using the Thomas algorithm.  (FIXME: A tridiagonal matrix
 * is easy to solve in O(n) but you cannot write a closed path as a
 * tridiagonal.  To solve closed paths we will have to use a different
 * solver. Use the GSL?)
 *
 * Here is an article that explains the math:
 * http://www.particleincell.com/blog/2012/bezier-splines/
 *
 * Basically we find all the ctrl1 points when we solve the linear
 * system, then we calculate each ctrl2 from the ctrl1.
 *
 * We build the linear system choosing for each segment of the path an
 * equation among following 9 equations.  "Straight" is a path that
 * goes straight in to the knot (2nd derivative == 0 at the knot).
 * "Smooth" means a path that goes smoothly through the knot, makes no
 * corner and curves the same amount just before and just after the
 * knot (1st and 2nd derivatives are constant around the knot.)
 * "Keep" means to keep the control point as the user set it.
 *
 * |    | start       |   end of path
 * | -- | ----------- | ---------------
 * | 1  | straight    | smooth
 * | 2  | smooth      | smooth
 * | 3  | smooth      | straight
 * | 4  | keep        | smooth
 * | 5  | keep        | keep
 * | 6  | smooth      | keep
 * | 7  | keep        | straight
 * | 8  | straight    | straight  (yields a line)
 * | 9  | straight    | keep
 *
 * The equations are (close your eyes):
 *
 * \f{eqnarray}{
 *                2P_{1,i} + P_{1,i+1} &=&  K_i + 2K_{i+1}  \label{1} \\
 *    P_{1,i-1} + 4P_{1,i} + P_{1,i+1} &=& 4K_i + 2K_{i+1}  \label{2} \\
 *   2P_{1,i-1} + 7P_{1,i}             &=& 8K_i +  K_{i+1}  \label{3} \\
 *                 P_{1,i}             &=& C1_i             \label{4} \\
 *                 P_{1,i}             &=& C1_i             \label{5} \\
 *    P_{1,i-1} + 4P_{1,i}             &=& C2_i + 4K_i      \label{6} \\
 *                 P_{1,i}             &=& C1_i             \label{7} \\
 *                3P_{1,i}             &=& 2K_i +  K_{i+1}  \label{8} \\
 *                2P_{1,i}             &=&  K_i +  C2_i     \label{9}
 * \f}
 *
 * Some of these are the same and differ only in the way we calculate
 * c2. (You may open your eyes again.)
 */

static void smooth_path_linsys(size_t n,
                                const float complex *k,
                                float complex *c1,
                                float complex *c2,
                                const int *equation)
{
  --n;
  float *a = malloc(sizeof(float) * n); // subdiagonal
  float *b = malloc(sizeof(float) * n); // main diagonal
  float *c = malloc(sizeof(float) * n); // superdiagonal
  float complex *d = malloc(sizeof(float complex) * n); // right hand side

  // Build the tridiagonal matrix.

  for(int i = 0; i < n; i++)
  {
    switch(equation[i])
    {
    #define ABCD(A,B,C,D) { { a[i] = A; b[i] = B; c[i] = C; d[i] = D; continue; } }
       case 1:  ABCD(0, 2, 1,       k[i] + 2 * k[i+1]   ); break;
       case 2:  ABCD(1, 4, 1,   4 * k[i] + 2 * k[i+1]   ); break;
       case 3:  ABCD(2, 7, 0,   8 * k[i] +     k[i+1]   ); break;
       case 4:  ABCD(0, 1, 0,                  c1[i]    ); break;
       case 5:  ABCD(0, 1, 0,                  c1[i]    ); break;
       case 6:  ABCD(1, 4, 0,   4 * k[i] +     c2[i]    ); break;
       case 7:  ABCD(0, 1, 0,                  c1[i]    ); break;
       case 8:  ABCD(0, 3, 0,   2 * k[i] +     k[i+1]   ); break;
       case 9:  ABCD(0, 2, 0,       k[i] +     c2[i]    ); break;
    #undef ABCD
    }
  }

  // Solve with the Thomas algorithm to compute c1's.  See:
  // http://en.wikipedia.org/wiki/Tridiagonal_matrix_algorithm

  for(int i = 1; i < n; i++)
  {
    const float m = a[i] / b[i-1];
    b[i] = b[i] - m * c[i-1];
    d[i] = d[i] - m * d[i-1];
  }

  c1[n-1] = d[n-1] / b[n-1];
  for(int i = n - 2; i >= 0; i--)
    c1[i] = (d[i] - c[i] * c1[i+1]) / b[i];

  // Now compute the c2's.

  for(int i = 0; i < n; i++)
  {
    switch(equation[i])
    {
       // keep end: c2 does not change
       case 5:
       case 6:
       case 9:  break;

       // straight end: put c2[i] halfway between c1[i] and k[i+1]
       case 3:
       case 7:
       case 8:  c2[i] = (c1[i] + k[i+1]) / 2;  break;

       // smooth end: c2 and c1 are symmetrical around the knot
       default: c2[i] = 2 * k[i+1] - c1[i+1];
    }
  }

  free(a);
  free(b);
  free(c);
  free(d);
}

static int path_length(dt_iop_liquify_params_t *p, dt_liquify_path_data_t *n)
{
  int count = 1;
  while(n->header.next != -1)
  {
    count++;
    n = &p->nodes[n->header.next];
  }
  return count;
}

static void smooth_paths_linsys(dt_iop_liquify_params_t *params)
{
  for(int k = 0; k < MAX_NODES; k++)
  {
    if(params->nodes[k].header.type == DT_LIQUIFY_PATH_INVALIDATED)
      break;

    if(params->nodes[k].header.prev != -1)
      continue;

    dt_liquify_path_data_t *node = &params->nodes[k];

    const size_t n = path_length(params, node);

    if(n < 2)
      continue;

    float complex *pt = calloc(n, sizeof(float complex));
    float complex *c1 = calloc(n, sizeof(float complex));
    float complex *c2 = calloc(n, sizeof(float complex));
    int *eqn          = calloc(n, sizeof(int));
    size_t idx = 0;

    while(node)
    {
      const dt_liquify_path_data_t *d = (dt_liquify_path_data_t *) node;
      const dt_liquify_path_data_t *p = node_prev(params, node);
      const dt_liquify_path_data_t *n = node_next(params, node);
      const dt_liquify_path_data_t *nn = n ? node_next(params, n) : NULL;

      pt[idx] = node->warp.point;
      if(d->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
      {
        c1[idx-1] = d->node.ctrl1;
        c2[idx-1] = d->node.ctrl2;
      }

      const int autosmooth      = d->header.node_type == DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH;
      const int next_autosmooth = n   &&  n->header.node_type == DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH;
      const int firstseg        = !p  ||  d->header.type != DT_LIQUIFY_PATH_CURVE_TO_V1;
      const int lastseg         = !nn ||  nn->header.type != DT_LIQUIFY_PATH_CURVE_TO_V1;
      const int lineseg         = n   &&  n->header.type == DT_LIQUIFY_PATH_LINE_TO_V1;

      // Program the linear system with equations:
      //
      //    START           END
      //    --------------------------
      // 1: straight        smooth
      // 2: smooth          smooth
      // 3: smooth          straight
      // 4: keep            smooth
      // 5: keep            keep
      // 6: smooth          keep
      // 7: keep            straight
      // 8: straight        straight   (== line)
      // 9: straight        keep

      if(lineseg)                                                    eqn[idx] = 5;
      else if(!autosmooth && !next_autosmooth)                       eqn[idx] = 5;
      else if(firstseg && lastseg && !autosmooth && next_autosmooth) eqn[idx] = 7;
      else if(firstseg && lastseg && autosmooth && next_autosmooth)  eqn[idx] = 8;
      else if(firstseg && lastseg && autosmooth && !next_autosmooth) eqn[idx] = 9;
      else if(firstseg && autosmooth && !next_autosmooth)            eqn[idx] = 5;
      else if(firstseg && autosmooth)                                eqn[idx] = 1;
      else if(lastseg && autosmooth && next_autosmooth)              eqn[idx] = 3;
      else if(lastseg && !autosmooth && next_autosmooth)             eqn[idx] = 7;
      else if(autosmooth && !next_autosmooth)                        eqn[idx] = 6;
      else if(!autosmooth && next_autosmooth)                        eqn[idx] = 4;
      else                                                           eqn[idx] = 2;

      ++idx;
      node = node_next(params, node);
    }

    smooth_path_linsys(n, pt, c1, c2, eqn);

    // write calculated control points back to list structure
    node = &params->nodes[k];
    node = node_next(params, node);
    idx = 0;
    while(node)
    {
      dt_liquify_path_data_t *d  = (dt_liquify_path_data_t *) node;
      if(d->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
      {
        d->node.ctrl1 = c1[idx];
        d->node.ctrl2 = c2[idx];
      }
      ++idx;
      node = node_next(params, node);
    }

    free(pt);
    free(c1);
    free(c2);
    free(eqn);
  }
}

static dt_liquify_path_data_t *_find_hovered(dt_iop_liquify_params_t *p)
{
  for(int k=0; k<MAX_NODES; k++)
    if(p->nodes[k].header.type == DT_LIQUIFY_PATH_INVALIDATED)
      break;
    else if(p->nodes[k].header.hovered)
      return &p->nodes[k];
  return NULL;
}

static void init_warp(dt_liquify_warp_t *warp, float complex point)
{
  warp->type     = DT_LIQUIFY_WARP_TYPE_LINEAR;
  warp->point    = point;
  warp->radius   = point;
  warp->strength = point;
  warp->control1 = 0.5;
  warp->control2 = 0.75;
  warp->status   = DT_LIQUIFY_STATUS_NONE;
}

static dt_liquify_path_data_t *alloc_move_to(dt_iop_module_t *module, float complex start_point)
{
  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *)module->gui_data;
  dt_iop_liquify_params_t *p = (dt_iop_liquify_params_t *)module->params;
  dt_liquify_path_data_t* m = (dt_liquify_path_data_t*)node_alloc(p, &g->node_index);
  if(m)
  {
    m->header.type = DT_LIQUIFY_PATH_MOVE_TO_V1;
    m->header.node_type = DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH;
    init_warp(&m->warp, start_point);
  }
  return (dt_liquify_path_data_t *)m;
}

static dt_liquify_path_data_t *alloc_line_to(dt_iop_module_t *module, float complex end_point)
{
  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *)module->gui_data;
  dt_iop_liquify_params_t *p = (dt_iop_liquify_params_t *)module->params;
  dt_liquify_path_data_t* l = (dt_liquify_path_data_t*)node_alloc(p, &g->node_index);
  if(l)
  {
    l->header.type = DT_LIQUIFY_PATH_LINE_TO_V1;
    l->header.node_type = DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH;
    init_warp(&l->warp, end_point);
  }
  return (dt_liquify_path_data_t *)l;
}

static dt_liquify_path_data_t *alloc_curve_to(dt_iop_module_t *module, float complex end_point)
{
  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *)module->gui_data;
  dt_iop_liquify_params_t *p = (dt_iop_liquify_params_t *)module->params;
  dt_liquify_path_data_t* c = (dt_liquify_path_data_t*)node_alloc(p, &g->node_index);
  if(c)
  {
    c->header.type = DT_LIQUIFY_PATH_CURVE_TO_V1;
    c->header.node_type = DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH;
    c->node.ctrl1 = c->node.ctrl2 = 0.0;
    init_warp(&c->warp, end_point);
  }
  return (dt_liquify_path_data_t *)c;
}

static void unselect_all(dt_iop_liquify_params_t *p)
{
  for(int k=0; k<MAX_NODES; k++)
    if(p->nodes[k].header.type == DT_LIQUIFY_PATH_INVALIDATED)
      break;
    else
      p->nodes[k].header.selected = 0;
}

static float get_zoom_scale(dt_develop_t *develop)
{
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  return dt_dev_get_zoom_scale(develop, zoom, 1<<closeup, 1);
}

void gui_post_expose(struct dt_iop_module_t *module,
                     cairo_t *cr,
                     int32_t width,
                     int32_t height,
                     int32_t pointerx,
                     int32_t pointery)
{
  dt_develop_t *develop = module->dev;
  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *)module->gui_data;
  dt_iop_liquify_params_t *p = (dt_iop_liquify_params_t *)module->params;
  if(!g)
    return;

  const float bb_width = develop->preview_pipe->backbuf_width;
  const float bb_height = develop->preview_pipe->backbuf_height;
  const float iscale = develop->preview_pipe->iscale;
  const float pr_d = develop->preview_downsampling;
  const float scale = pr_d * MAX(bb_width, bb_height);
  if(bb_width < 1.0 || bb_height < 1.0)
    return;

  // get a copy of all iop params
  dt_iop_gui_enter_critical_section(module);
  update_warp_count(module);
  smooth_paths_linsys(p);
  dt_iop_liquify_params_t copy_params;
  memcpy(&copy_params, p, sizeof(dt_iop_liquify_params_t));
  dt_iop_gui_leave_critical_section(module);

  // distort all points
  dt_pthread_mutex_lock(&develop->preview_pipe_mutex);
  const distort_params_t d_params = { develop, develop->preview_pipe, iscale, 1.0 / scale, DT_DEV_TRANSFORM_DIR_ALL, FALSE };
  _distort_paths(module, &d_params, &copy_params);
  dt_pthread_mutex_unlock(&develop->preview_pipe_mutex);

  // You're not supposed to understand this
  const float zoom_x = dt_control_get_dev_zoom_x();
  const float zoom_y = dt_control_get_dev_zoom_y();
  const float zoom_scale = get_zoom_scale(develop);

  // setup CAIRO coordinate system
  cairo_translate(cr, 0.5 * width, 0.5 * height); // origin @ center of view
  cairo_scale    (cr, zoom_scale, zoom_scale);    // the zoom
  cairo_translate(cr, -bb_width * (0.5 + zoom_x), -bb_height * (0.5 + zoom_y));
  cairo_scale(cr, scale, scale);

  draw_paths(module, cr, 1.0 / (scale * zoom_scale), &copy_params);
}

static gboolean btn_make_radio_callback(GtkToggleButton *btn, GdkEventButton *event, dt_iop_module_t *module);

void gui_focus(struct dt_iop_module_t *module, gboolean in)
{
  if(!in)
  {
    dt_collection_hint_message(darktable.collection);
    btn_make_radio_callback(NULL, NULL, module);
  }
}

static void sync_pipe(struct dt_iop_module_t *module, gboolean history)
{
  if(history)
  {
    dt_iop_liquify_params_t *p = (dt_iop_liquify_params_t *)module->params;

    // something definitive has happened like button release ... so
    // redraw pipe
    smooth_paths_linsys(p);
    dt_dev_add_history_item(darktable.develop, module, TRUE);
  }
  else
  {
    // only moving mouse around, pointing at things or dragging ... so
    // give some cairo feedback, but don't redraw pipe
    dt_control_queue_redraw_center();
  }
}

/*
  right-click on node:       Delete node.
  right-click on path:       Delete whole path.

  ctrl+click on node:        Cycle symmetrical, smooth, cusp, autosmooth
  ctrl+click on path:        Add node
  ctrl+alt+click on path:    Change line / bezier

  ctrl+click on strength:    Cycle linear, grow, shrink
*/

static void get_point_scale(struct dt_iop_module_t *module, float x, float y, float complex *pt, float *scale)
{
  const float pr_d = darktable.develop->preview_downsampling;

  float pzx = 0.0f, pzy = 0.0f;
  dt_dev_get_pointer_zoom_pos(darktable.develop, x, y, &pzx, &pzy);
  pzx += 0.5f;
  pzy += 0.5f;
  const float wd = darktable.develop->preview_pipe->backbuf_width;
  const float ht = darktable.develop->preview_pipe->backbuf_height;
  float pts[2] = { pzx * wd, pzy * ht };
  dt_dev_distort_backtransform_plus(darktable.develop, darktable.develop->preview_pipe,
                                    module->iop_order,DT_DEV_TRANSFORM_DIR_FORW_EXCL, pts, 1);
  dt_dev_distort_backtransform_plus(darktable.develop, darktable.develop->preview_pipe,
                                    module->iop_order,DT_DEV_TRANSFORM_DIR_BACK_EXCL, pts, 1);
  const float nx = pts[0] / darktable.develop->preview_pipe->iwidth;
  const float ny = pts[1] / darktable.develop->preview_pipe->iheight;

  *scale = darktable.develop->preview_pipe->iscale * (pr_d * get_zoom_scale(module->dev));
  *pt = (nx * darktable.develop->pipe->iwidth) +  (ny * darktable.develop->pipe->iheight) * I;
}

int mouse_moved(struct dt_iop_module_t *module,
                 double x,
                 double y,
                 double pressure,
                 int which)
{
  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *)module->gui_data;
  dt_iop_liquify_params_t *pa = (dt_iop_liquify_params_t *)module->params;
  gboolean handled = FALSE;
  float complex pt = 0.0f;
  float scale = 0.0f;

  get_point_scale(module, x, y, &pt, &scale);

  dt_iop_gui_enter_critical_section(module);

  g->last_mouse_pos = pt;

  // Don't hit test while dragging, you'd only hit the dragged thing
  // anyway.

  if(!is_dragging(g))
  {
    dt_liquify_hit_t hit = NOWHERE;
    _hit_test_paths(module, pa, pt, &hit);
    dt_liquify_path_data_t *last_hovered = _find_hovered(pa);
    if(hit.elem != last_hovered
       || (last_hovered && hit.elem
           && hit.elem->header.hovered != last_hovered->header.hovered))
    {
      if(hit.elem)
        hit.elem->header.hovered = hit.layer;
      if(last_hovered)
        last_hovered->header.hovered = 0;
      // change in hover display
      dt_control_hinter_message(darktable.control, dt_liquify_layers[hit.layer].hint);
      // also use when dragging later
      dt_liquify_layers[DT_LIQUIFY_LAYER_BACKGROUND].hint = dt_liquify_layers[hit.layer].hint;
      handled = TRUE;
      goto done;
    }

    const gboolean dragged = detect_drag(g, scale, pt);

    if(dragged && g->last_hit.elem)
    {
      // start dragging
      start_drag(g, g->last_hit.layer, g->last_hit.elem);
      // nothing more to do, we will refresh on the next call anyway
      // this makes the initial move of a node a bit more fluid.
      handled = TRUE;
      goto done;
    }

    if(g->last_hit.elem)
    {
      // an item is selected, so this movement is handled and must
      // not trigger any panning.
      handled = TRUE;
    }
    else if(hit.elem == DT_LIQUIFY_LAYER_BACKGROUND && gtk_toggle_button_get_active(g->btn_node_tool))
      dt_control_hinter_message(darktable.control, _("click to edit nodes"));
  }
  else // we are dragging
  {
    dt_control_hinter_message(darktable.control, dt_liquify_layers[DT_LIQUIFY_LAYER_BACKGROUND].hint);

    dt_liquify_path_data_t *d = g->dragging.elem;
    dt_liquify_path_data_t *n = node_next(pa, d);
    dt_liquify_path_data_t *p = node_prev(pa, d);

    const float complex *start_pt = &d->warp.point;

    switch(g->dragging.layer)
    {
       case DT_LIQUIFY_LAYER_CENTERPOINT:
         switch(d->header.type)
         {
            case DT_LIQUIFY_PATH_CURVE_TO_V1:
              d->node.ctrl2 += pt - d->warp.point;
              // fall thru
            case DT_LIQUIFY_PATH_MOVE_TO_V1:
            case DT_LIQUIFY_PATH_LINE_TO_V1:
              if(n && n->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
                n->node.ctrl1 += pt - d->warp.point;
              if(p && p->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
                p->node.ctrl2 += pt - d->warp.point;
              d->warp.radius   += pt - d->warp.point;
              d->warp.strength += pt - d->warp.point;
              d->warp.point = pt;
              break;
            default:
              break;
         }
         break;

       case DT_LIQUIFY_LAYER_CTRLPOINT1:
         switch(d->header.type)
         {
            case DT_LIQUIFY_PATH_CURVE_TO_V1:
              d->node.ctrl1 = pt;
              if(p && p->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
              {
                switch(p->header.node_type)
                {
                   case DT_LIQUIFY_NODE_TYPE_SMOOTH:
                     p->node.ctrl2 = p->warp.point +
                       cabsf(p->warp.point - p->node.ctrl2) *
                       cexpf(cargf(p->warp.point - pt) * I);
                     break;
                case DT_LIQUIFY_NODE_TYPE_SYMMETRICAL:
                  p->node.ctrl2 = 2 * p->warp.point - pt;
                  break;
                default:
                  break;
                }
              }
              break;
            default:
              break;
         }
         break;

       case DT_LIQUIFY_LAYER_CTRLPOINT2:
         switch(d->header.type)
         {
            case DT_LIQUIFY_PATH_CURVE_TO_V1:
              d->node.ctrl2 = pt;
              if(n && n->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
              {
                switch(d->header.node_type)
                {
                   case DT_LIQUIFY_NODE_TYPE_SMOOTH:
                     n->node.ctrl1 = d->warp.point +
                       cabsf(d->warp.point - n->node.ctrl1) *
                       cexpf(cargf(d->warp.point - pt) * I);
                     break;
                   case DT_LIQUIFY_NODE_TYPE_SYMMETRICAL:
                     n->node.ctrl1 = 2 * d->warp.point - pt;
                     break;
                   default:
                     break;
                }
              }
              break;
            default:
              break;
         }
         break;

       case DT_LIQUIFY_LAYER_RADIUSPOINT:
         d->warp.radius = pt;
         dt_conf_set_float(CONF_RADIUS, cabsf(d->warp.radius - d->warp.point));
         break;

       case DT_LIQUIFY_LAYER_STRENGTHPOINT:
         d->warp.strength = pt;
         dt_conf_set_float(CONF_STRENGTH, cabsf(d->warp.strength - d->warp.point));
         dt_conf_set_float(CONF_ANGLE, cargf(d->warp.strength - d->warp.point));
         break;

       case DT_LIQUIFY_LAYER_HARDNESSPOINT1:
         d->warp.control1 = MIN(1.0, cabsf(pt - *start_pt) / cabsf(d->warp.radius - *start_pt));
         break;

       case DT_LIQUIFY_LAYER_HARDNESSPOINT2:
         d->warp.control2 = MIN(1.0, cabsf(pt - *start_pt) / cabsf(d->warp.radius - *start_pt));
         break;

       default:
         break;
    }
    handled = TRUE;
  }

done:
  dt_iop_gui_leave_critical_section(module);
  if(handled)
  {
    sync_pipe(module, FALSE);
  }
  return handled;
}

static float dt_conf_get_sanitize_float(const char *name, float min, float max, float default_value)
{
  const float value = dt_conf_get_float(name);
  float new_value = CLAMP(value, min, max);

  if(default_value != 0.0f && new_value != value) new_value = 0.25f * default_value + 0.75f * value;

  dt_conf_set_float(name, new_value);
  return new_value;
}

static void get_stamp_params(dt_iop_module_t *module, float *radius, float *r_strength, float *phi)
{
  GtkWidget *widget = dt_ui_main_window(darktable.gui->ui);
  GtkAllocation allocation;
  gtk_widget_get_allocation(widget, &allocation);
  const int last_win_min = MIN(allocation.width, allocation.height);

  const dt_dev_pixelpipe_t *devpipe = darktable.develop->preview_pipe;
  const float iwd_min = MIN(devpipe->iwidth, devpipe->iheight);
  const float proc_wdht_min = MIN(devpipe->processed_width, devpipe->processed_height);
  const float pr_d = darktable.develop->preview_downsampling;
  const float scale = devpipe->iscale / (pr_d * get_zoom_scale(module->dev));
  const float im_scale = 0.09f * iwd_min * last_win_min * scale / proc_wdht_min;

  *radius = dt_conf_get_sanitize_float(CONF_RADIUS, 0.1f*im_scale, 3.0f*im_scale, im_scale);
  *r_strength = dt_conf_get_sanitize_float(CONF_STRENGTH, 0.5f * *radius, 2.0f * *radius, 1.5f * *radius);
  *phi = dt_conf_get_sanitize_float(CONF_ANGLE, -M_PI, M_PI, 0.0f);
}
/*
  add support for changing the radius and the strength vector for the temp node
 */
int scrolled(struct dt_iop_module_t *module, double x, double y, int up, uint32_t state)
{
  const dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *)module->gui_data;

  // add an option to allow skip mouse events while editing masks
  if(darktable.develop->darkroom_skip_mouse_events) return 0;
  const gboolean incr = dt_mask_scroll_increases(up);

  if(g->temp)
  {
    dt_liquify_warp_t *warp = &g->temp->warp;
    const float complex strength_v = warp->strength - warp->point;
    if(dt_modifier_is(state, 0))
    {
      //  change size
      float radius = 0.0f, r = 0.0f, phi = 0.0f;
      get_stamp_params(module, &radius, &r, &phi);

      float factor = 1.0f;
      if(incr)
        factor *= 1.0f / 0.97f;
      else if(!incr && cabsf(warp->radius - warp->point) > 10.0f)
        factor *= 0.97f;

      r *= factor;
      radius *= factor;

      warp->radius = warp->point + (radius * factor);
      warp->strength = warp->point + r * cexpf(phi * I);

      dt_conf_set_float(CONF_RADIUS, radius);
      dt_conf_set_float(CONF_STRENGTH, r);
      return 1;
    }
    else if(dt_modifier_is(state, GDK_CONTROL_MASK))
    {
      //  change the strength direction
      float phi = cargf(strength_v);
      const float r = cabsf(strength_v);

      if(incr)
        phi += DT_M_PI_F / 16.0f;
      else
        phi -= DT_M_PI_F / 16.0f;

      warp->strength = warp->point + r * cexpf(phi * I);
      dt_conf_set_float(CONF_STRENGTH, r);
      dt_conf_set_float(CONF_ANGLE, phi);
      return 1;
    }
    else if(dt_modifier_is(state, GDK_SHIFT_MASK))
    {
      //  change the strength
      const float phi = cargf(strength_v);
      float r = cabsf(strength_v);

      if(incr)
        r *= 1.0f / 0.97f;
      else
        r *= 0.97f;

      warp->strength = warp->point + r * cexpf(phi * I);
      dt_conf_set_float(CONF_STRENGTH, r);
      dt_conf_set_float(CONF_ANGLE, phi);
      return 1;
    }
  }

  return 0;
}

int button_pressed(struct dt_iop_module_t *module,
                    double x,
                    double y,
                    double pressure,
                    int which,
                    int type,
                    uint32_t state)
{
  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *)module->gui_data;
  dt_iop_liquify_params_t *p = (dt_iop_liquify_params_t *)module->params;

  int handled = 0;
  float complex pt = 0.0f;
  float scale = 0.0f;

  get_point_scale(module, x, y, &pt, &scale);

  dt_iop_gui_enter_critical_section(module);

  g->last_mouse_pos = pt;
  g->last_mouse_mods = state;
  if(which == 1)
    g->last_button1_pressed_pos = pt;

  if(!is_dragging(g))
    // while dragging you would always hit the dragged thing
    _hit_test_paths(module, p, pt, &g->last_hit);

  if(which == 2) goto done;

  // Point tool

  if(which == 1 && gtk_toggle_button_get_active(g->btn_point_tool))
  {
    // always end dragging before manipulating the path list to avoid
    // dangling pointers
    end_drag(g);

    if(!g->temp) goto done;
    g->status |= DT_LIQUIFY_STATUS_NEW;
    g->status &= ~DT_LIQUIFY_STATUS_PREVIEW;

    start_drag(g, DT_LIQUIFY_LAYER_STRENGTHPOINT, g->temp);
    g->last_hit = NOWHERE;
    handled = 1;
    goto done;
  }

  // Line tool or curve tool

  if(which == 1 && (gtk_toggle_button_get_active(g->btn_line_tool)
                    || gtk_toggle_button_get_active(g->btn_curve_tool)))
  {
    // always end dragging before manipulating the path list to avoid
    // dangling pointers
    end_drag(g);
    if(!g->temp)
    {
      if(g->last_hit.layer == DT_LIQUIFY_LAYER_CENTERPOINT)
      {
        // continue path
        g->temp = g->last_hit.elem;
      }
      else
      {
        if(!g->temp) goto done;
      }
    }
    g->last_hit = NOWHERE;
    if(gtk_toggle_button_get_active(g->btn_curve_tool))
    {
      start_drag(g, DT_LIQUIFY_LAYER_CTRLPOINT1, g->temp);
    }
    g->status |= DT_LIQUIFY_STATUS_NEW;
    g->status &= ~DT_LIQUIFY_STATUS_PREVIEW;
    handled = 1;
    goto done;
  }

done:
  dt_iop_gui_leave_critical_section(module);
  return handled;
}

static void _start_new_shape(dt_iop_module_t *module)
{
  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *)module->gui_data;

  //  create initial shape at the center
  float complex pt = 0.0f;
  float scale = 1.0f;
  get_point_scale(module, 0.5f * darktable.develop->width, 0.5f * darktable.develop->height, &pt, &scale);
  float radius = 0.0f, r = 1.0f, phi = 0.0f;
  get_stamp_params(module, &radius, &r, &phi);
  //  start a new path
  g->temp = alloc_move_to(module, pt);
  g->temp->warp.radius = pt + radius;
  g->temp->warp.strength = pt + r * cexpf(phi * I);
  g->status |= DT_LIQUIFY_STATUS_PREVIEW;
  g->status |= DT_LIQUIFY_STATUS_NEW;

  g->just_started = TRUE;


  start_drag(g, DT_LIQUIFY_LAYER_CENTERPOINT, g->temp);
  g->last_hit = NOWHERE;
}

int button_released(struct dt_iop_module_t *module,
                     double x,
                     double y,
                     int which,
                     uint32_t state)
{
  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *)module->gui_data;
  dt_iop_liquify_params_t *p = (dt_iop_liquify_params_t *)module->params;
  int handled = 0;
  float complex pt = 0.0f;
  float scale = 0.0f;

  get_point_scale(module, x, y, &pt, &scale);

  dt_iop_gui_enter_critical_section(module);

  g->last_mouse_pos = pt;

  const gboolean dragged = detect_drag(g, scale, pt);

  if(which == 1 && g->temp && (g->status & DT_LIQUIFY_STATUS_NEW))
  {
    end_drag(g);
    if(gtk_toggle_button_get_active(g->btn_point_tool))
    {
      g->temp = NULL; // a point is done

      if(g->creation_continuous)
        _start_new_shape(module);
      else
        btn_make_radio_callback(g->btn_node_tool, NULL, module);
      handled = 2;
    }
    else if(gtk_toggle_button_get_active(g->btn_line_tool))
    {
      const int prev_index = g->node_index;
      const float complex strength = (g->temp->warp.strength - g->temp->warp.point);
      const float radius = cabsf(g->temp->warp.radius - g->temp->warp.point);
      g->temp = alloc_line_to(module, pt);
      if(!g->temp) goto done;
      g->temp->warp.radius = pt + radius;
      g->temp->warp.strength = pt + strength;
      // links
      g->temp->header.prev = prev_index;
      node_get(p, prev_index)->header.next = g->node_index;
      start_drag(g, DT_LIQUIFY_LAYER_CENTERPOINT, g->temp);
      g->just_started = FALSE;
      handled = 1;
    }
    else if(gtk_toggle_button_get_active(g->btn_curve_tool))
    {
      const int prev_index = g->node_index;
      const float complex strength = (g->temp->warp.strength - g->temp->warp.point);
      const float radius = cabsf(g->temp->warp.radius - g->temp->warp.point);
      g->temp = alloc_curve_to(module, pt);
      if(!g->temp) goto done;
      g->temp->warp.radius = pt + radius;
      g->temp->warp.strength = pt + strength;
      // links
      g->temp->header.prev = prev_index;
      node_get(p, prev_index)->header.next = g->node_index;
      start_drag(g, DT_LIQUIFY_LAYER_CENTERPOINT, g->temp);
      g->just_started = FALSE;
      handled = 1;
    }
    g->status &= ~DT_LIQUIFY_STATUS_NEW;
    goto done;
  }

  if(which == 1 && is_dragging(g))
  {
    end_drag(g);
    handled = 2;
    goto done;
  }

  // right click == cancel or delete
  if(which == 3)
  {
    end_drag(g);

    // cancel line or curve creation
    if(g->temp)
    {
      node_delete(p, g->temp);
      g->temp = NULL;
      if(g->creation_continuous && !g->just_started)
        _start_new_shape(module);
      else
      {
        g->status &= ~DT_LIQUIFY_STATUS_PREVIEW;
        btn_make_radio_callback(g->btn_node_tool, NULL, module);
      }
      handled = 2;
      goto done;
    }

    // right click on background toggles node tool
    if(g->last_hit.layer == DT_LIQUIFY_LAYER_BACKGROUND)
    {
      btn_make_radio_callback(g->btn_node_tool, NULL, module);
      handled = 1;
      goto done;
    }

    // delete node
    if(g->last_hit.layer == DT_LIQUIFY_LAYER_CENTERPOINT)
    {
      node_delete(p, g->last_hit.elem);
      g->last_hit = NOWHERE;
      handled = 2;
      goto done;
    }
    // delete shape
    if(g->last_hit.layer == DT_LIQUIFY_LAYER_PATH)
    {
      path_delete(p, g->last_hit.elem);
      g->last_hit = NOWHERE;
      handled = 2;
      goto done;
    }
    goto done;
  }

  // Node tool

  if(gtk_toggle_button_get_active(g->btn_node_tool))
  {
    if(which == 1 && dt_modifier_is(g->last_mouse_mods, 0) && !dragged)
    {
      // select/unselect start/endpoint and clear previous selections
      if(g->last_hit.layer == DT_LIQUIFY_LAYER_CENTERPOINT)
      {
        const int oldsel = !!g->last_hit.elem->header.selected;
        unselect_all(p);
        g->last_hit.elem->header.selected = oldsel ? 0 : g->last_hit.layer;
        handled = 1;
        goto done;
      }
      // unselect all
      if(g->last_hit.layer == DT_LIQUIFY_LAYER_BACKGROUND)
      {
        unselect_all(p);
        handled = 1;
        goto done;
      }
    }
    if(which == 1 && dt_modifier_is(g->last_mouse_mods, GDK_SHIFT_MASK) && !dragged)
    {
      // select/unselect start/endpoint and keep previous selections
      if(g->last_hit.layer == DT_LIQUIFY_LAYER_CENTERPOINT)
      {
        const int oldsel = !!g->last_hit.elem->header.selected;
        g->last_hit.elem->header.selected = oldsel ? 0 : g->last_hit.layer;
        handled = 1;
        goto done;
      }
    }
    if(which == 1 && dt_modifier_is(g->last_mouse_mods, GDK_CONTROL_MASK) && !dragged)
    {
      // add node
      if(g->last_hit.layer == DT_LIQUIFY_LAYER_PATH)
      {
        dt_liquify_path_data_t *e = g->last_hit.elem;
        dt_liquify_path_data_t *prev = node_prev(p, e);
        if(prev && e->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
        {
          // add node to curve
          dt_liquify_path_data_t *curve1 = (dt_liquify_path_data_t *) e;

          dt_liquify_path_data_t *curve2 = (dt_liquify_path_data_t *)alloc_curve_to(module, 0);
          if(!curve2) goto done;

          curve2->node.ctrl1 = curve1->node.ctrl1;
          curve2->node.ctrl2 = curve1->node.ctrl2;

          dt_liquify_warp_t *warp1 = &prev->warp;
          dt_liquify_warp_t *warp2 = &curve2->warp;
          dt_liquify_warp_t *warp3 = &e->warp;

          const float t = find_nearest_on_curve_t(warp1->point, curve1->node.ctrl1, curve1->node.ctrl2,
                                                   warp3->point, pt, INTERPOLATION_POINTS);

          float complex midpoint = warp3->point;
          casteljau(&warp1->point, &curve1->node.ctrl1, &curve1->node.ctrl2, &midpoint, t);
          midpoint = warp1->point;
          casteljau(&warp3->point, &curve2->node.ctrl2, &curve2->node.ctrl1, &midpoint, 1.0 - t);

          mix_warps(warp2, warp1, warp3, midpoint, t);

          node_insert_before(p, e, (dt_liquify_path_data_t *)curve2);

          handled = 2;
          goto done;
        }
        if(prev && e->header.type == DT_LIQUIFY_PATH_LINE_TO_V1)
        {
          // add node to line
          dt_liquify_warp_t *warp1 = &prev->warp;
          dt_liquify_warp_t *warp3 = &e->warp;
          const float t = find_nearest_on_line_t(warp1->point, warp3->point, pt);

          dt_liquify_path_data_t *tmp = alloc_line_to(module, e->warp.point);
          if(!tmp) goto done;

          dt_liquify_warp_t *warp2 = &tmp->warp;
          const float complex midpoint = cmix(warp1->point, warp3->point, t);

          mix_warps(warp2, warp1, warp3, midpoint, t);
          node_insert_before(p, e, tmp);
        }
      }
      else if(g->last_hit.elem->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1
              && g->last_hit.layer == DT_LIQUIFY_LAYER_CENTERPOINT)
      {
        // cycle node type: smooth -> cusp etc.
        dt_liquify_path_data_t *e = g->last_hit.elem;
        e->header.node_type = (e->header.node_type + 1) % DT_LIQUIFY_NODE_TYPE_LAST;

        handled = 2;
        goto done;
      }
      else if(g->last_hit.layer == DT_LIQUIFY_LAYER_STRENGTHPOINT)
      {
        // cycle warp type: linear -> radial etc.
        dt_liquify_path_data_t *e = g->last_hit.elem;
        if(e->header.type == DT_LIQUIFY_PATH_MOVE_TO_V1)
        {
          dt_liquify_warp_t *warp = &e->warp;
          warp->type = (warp->type + 1) % DT_LIQUIFY_WARP_TYPE_LAST;

          handled = 2;
          goto done;
        }
      }
    }
    if(which == 1
       && dt_modifier_is(g->last_mouse_mods, GDK_MOD1_MASK | GDK_CONTROL_MASK)
       && !dragged)
    {
      if(g->last_hit.layer == DT_LIQUIFY_LAYER_PATH)
      {
        // change segment
        dt_liquify_path_data_t *e = g->last_hit.elem;
        dt_liquify_path_data_t *prev = node_prev(p, e);
        if(prev && e->header.type == DT_LIQUIFY_PATH_CURVE_TO_V1)
        {
          // curve -> line
          e->header.type = DT_LIQUIFY_PATH_LINE_TO_V1;
          e->header.node_type = DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH;
          e->header.selected = e->header.hovered = 0;
          handled = 2;
          goto done;
        }
        if(prev && e->header.type == DT_LIQUIFY_PATH_LINE_TO_V1)
        {
          // line -> curve
          const float complex p0 = prev->warp.point;
          const float complex p1 = e->warp.point;
          dt_liquify_path_data_t *c = (dt_liquify_path_data_t *)e;
          e->header.type = DT_LIQUIFY_PATH_CURVE_TO_V1;
          e->header.node_type = DT_LIQUIFY_NODE_TYPE_AUTOSMOOTH;
          c->node.ctrl1 = (2 * p0 +     p1) / 3.0;
          c->node.ctrl2 = (    p0 + 2 * p1) / 3.0;

          handled = 2;
          goto done;
        }
      }
    }
  }

done:
  dt_iop_gui_leave_critical_section(module);
  if(which == 1)
    g->last_button1_pressed_pos = -1;
  g->last_hit = NOWHERE;
  if(handled)
  {
    update_warp_count(module);
    sync_pipe(module, handled == 2);
  }
  return handled;
}

static void _liquify_cairo_paint_point_tool(cairo_t *cr, const gint x, const gint y, const gint w, const gint h,
                                            const gint flags, void *data);

static void _liquify_cairo_paint_line_tool(cairo_t *cr, const gint x, const gint y, const gint w, const gint h,
                                           const gint flags, void *data);

static void _liquify_cairo_paint_curve_tool(cairo_t *cr, const gint x, const gint y, const gint w, const gint h,
                                            const gint flags, void *data);

static void _liquify_cairo_paint_node_tool(cairo_t *cr, const gint x, const gint y, const gint w, const gint h,
                                           const gint flags, void *data);

// we need this only because darktable has no radiobutton support

static gboolean btn_make_radio_callback(GtkToggleButton *btn, GdkEventButton *event, dt_iop_module_t *module)
{
  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *)module->gui_data;
  dt_iop_liquify_params_t *p = (dt_iop_liquify_params_t *)module->params;

  // if currently dragging and a form (line or node) has been started, does nothing (expect resetting the toggle button status).
  if(is_dragging(g) && g->temp && node_prev(p, g->temp))
  {
    return TRUE;
  }

  g->creation_continuous = event != NULL && dt_modifier_is(event->state, GDK_CONTROL_MASK);

  dt_control_hinter_message(darktable.control, "");

  // if we are on a preview, it means that a form (point, line, curve) has been started, but no node has yet been placed.
  // in this case we abort the current preview and let the new tool activated.
  if(g->status & DT_LIQUIFY_STATUS_PREVIEW)
  {
    node_delete(p, g->temp);
    g->temp = NULL;
    g->status &= ~DT_LIQUIFY_STATUS_PREVIEW;
  }

  // now, let's enable and start a new form safely
  if(!btn || !gtk_toggle_button_get_active(btn))
  {
    gtk_toggle_button_set_active(g->btn_point_tool, btn == g->btn_point_tool);
    gtk_toggle_button_set_active(g->btn_line_tool,  btn == g->btn_line_tool);
    gtk_toggle_button_set_active(g->btn_curve_tool, btn == g->btn_curve_tool);
    gtk_toggle_button_set_active(g->btn_node_tool,  btn == g->btn_node_tool);

    gtk_toggle_button_set_active(g->btn_node_tool,  btn == g->btn_node_tool);

    dt_liquify_layers[DT_LIQUIFY_LAYER_BACKGROUND].hint
        = btn == g->btn_point_tool
        ? _("click and drag to add point\nscroll to change size - "
            "shift+scroll to change strength - ctrl+scroll to change direction")
        : btn == g->btn_line_tool
        ? _("click to add line\nscroll to change size - "
            "shift+scroll to change strength - ctrl+scroll to change direction")
        : btn == g->btn_curve_tool
        ? _("click to add curve\nscroll to change size - "
            "shift+scroll to change strength - ctrl+scroll to change direction")
        : "";

    //  start the preview mode to show the shape that will be created

    if(btn == g->btn_point_tool || btn == g->btn_line_tool || btn == g->btn_curve_tool)
    {
      _start_new_shape(module);
    }

    if(btn) dt_iop_request_focus(module);
  }
  else
  {
    gtk_toggle_button_set_active(btn, FALSE);
  }

  sync_pipe(module, FALSE);

  return TRUE;
}

void gui_update(dt_iop_module_t *module)
{
  update_warp_count(module);
}

void gui_init(dt_iop_module_t *self)
{
  dt_iop_liquify_gui_data_t *g = IOP_GUI_ALLOC(liquify);

  // A dummy surface for calculations only, no drawing.
  cairo_surface_t *cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_surface_destroy(cs);

  g->dragging = NOWHERE;
  g->temp = NULL;
  g->status = 0;
  g->last_mouse_pos =
  g->last_button1_pressed_pos = -1;
  g->last_hit = NOWHERE;
  g->node_index = 0;

  self->widget = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_tooltip_text(hbox, _("use a tool to add warps.\nright-click to remove a warp."));
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);

  GtkWidget *label = dt_ui_label_new(_("warps|nodes count:"));
  gtk_box_pack_start(GTK_BOX(hbox), label, FALSE, TRUE, 0);
  g->label = GTK_LABEL(dt_ui_label_new("-"));
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->label), FALSE, TRUE, 0);

  hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);

  g->btn_node_tool = GTK_TOGGLE_BUTTON(dt_iop_togglebutton_new(self, NULL, N_("edit, add and delete nodes"), NULL,
                                       G_CALLBACK(btn_make_radio_callback), TRUE, 0, 0,
                                       _liquify_cairo_paint_node_tool, hbox));

  g->btn_curve_tool = GTK_TOGGLE_BUTTON(dt_iop_togglebutton_new(self, N_("shapes"), N_("draw curves"), N_("draw multiple curves"),
                                        G_CALLBACK(btn_make_radio_callback), TRUE, 0, 0,
                                        _liquify_cairo_paint_curve_tool, hbox));

  g->btn_line_tool = GTK_TOGGLE_BUTTON(dt_iop_togglebutton_new(self, N_("shapes"), N_("draw lines"), N_("draw multiple lines"),
                                       G_CALLBACK(btn_make_radio_callback), TRUE, 0, 0,
                                       _liquify_cairo_paint_line_tool, hbox));

  g->btn_point_tool = GTK_TOGGLE_BUTTON(dt_iop_togglebutton_new(self, N_("shapes"), N_("draw points"), N_("draw multiple points"),
                                         G_CALLBACK(btn_make_radio_callback), TRUE, 0, 0,
                                         _liquify_cairo_paint_point_tool, hbox));

  dt_liquify_layers[DT_LIQUIFY_LAYER_BACKGROUND].hint     = "";
  dt_liquify_layers[DT_LIQUIFY_LAYER_PATH].hint           = _("ctrl+click: add node - right click: remove path\n"
                                                              "ctrl+alt+click: toggle line/curve");
  dt_liquify_layers[DT_LIQUIFY_LAYER_CENTERPOINT].hint    = _("click and drag to move - click: show/hide feathering controls\n"
                                                              "ctrl+click: autosmooth, cusp, smooth, symmetrical"
                                                              " - right click to remove");
  dt_liquify_layers[DT_LIQUIFY_LAYER_CTRLPOINT1].hint     = _("drag to change shape of path");
  dt_liquify_layers[DT_LIQUIFY_LAYER_CTRLPOINT2].hint     = _("drag to change shape of path");
  dt_liquify_layers[DT_LIQUIFY_LAYER_RADIUSPOINT].hint    = _("drag to adjust warp radius");
  dt_liquify_layers[DT_LIQUIFY_LAYER_HARDNESSPOINT1].hint = _("drag to adjust hardness (center)");
  dt_liquify_layers[DT_LIQUIFY_LAYER_HARDNESSPOINT2].hint = _("drag to adjust hardness (feather)");
  dt_liquify_layers[DT_LIQUIFY_LAYER_STRENGTHPOINT].hint  = _("drag to adjust warp strength\n"
                                                              "ctrl+click: linear, grow, and shrink");
}

void gui_reset(dt_iop_module_t *self)
{
  dt_iop_liquify_gui_data_t *g = (dt_iop_liquify_gui_data_t *)self->gui_data;
  g->dragging = NOWHERE;
  g->temp = NULL;
  g->status = 0;
  btn_make_radio_callback(NULL, NULL, self);
}

void gui_cleanup(dt_iop_module_t *self)
{
  IOP_GUI_FREE;
}

// defgroup Button paint functions

#define PREAMBLE                                       \
  cairo_save(cr);                                      \
  const gint s = MIN(w, h);                            \
  cairo_translate(cr, x + (w / 2.0) - (s / 2.0),       \
                  y + (h / 2.0) - (s / 2.0));          \
  cairo_scale(cr, s, s);                               \
  cairo_push_group(cr);                                \
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);       \
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);        \
  cairo_set_line_width(cr, 0.2);

#define POSTAMBLE                                              \
  cairo_pop_group_to_source(cr);                               \
  cairo_paint_with_alpha(cr, flags & CPF_ACTIVE ? 1.0 : 0.5);  \
  cairo_restore(cr);

static void _liquify_cairo_paint_point_tool(cairo_t *cr, const gint x, const gint y, const gint w, const gint h,
                                             const gint flags, void *data)
{
  PREAMBLE;
  cairo_new_sub_path(cr);
  cairo_arc(cr, 0.5, 0.5, 0.2, 0.0, 2 * DT_M_PI);
  cairo_fill(cr);
  POSTAMBLE;
}

static void _liquify_cairo_paint_line_tool(cairo_t *cr, const gint x, const gint y, const gint w, const gint h,
                                            const gint flags, void *data)
{
  PREAMBLE;
  cairo_move_to(cr, 0.1, 0.9);
  cairo_line_to(cr, 0.9, 0.1);
  cairo_stroke(cr);
  POSTAMBLE;
}

static void _liquify_cairo_paint_curve_tool(cairo_t *cr, const gint x, const gint y, const gint w, const gint h,
                                             const gint flags, void *data)
{
  PREAMBLE;
  cairo_move_to(cr, 0.1, 0.9);
  cairo_curve_to(cr, 0.1, 0.5, 0.5, 0.1, 0.9, 0.1);
  cairo_stroke(cr);
  POSTAMBLE;
}

static void _liquify_cairo_paint_node_tool(cairo_t *cr, const gint x, const gint y, const gint w, const gint h,
                                            const gint flags, void *data)
{
  PREAMBLE;
  const double dashed[] = {0.2, 0.2};
  cairo_set_dash(cr, dashed, 2, 0);
  cairo_set_line_width(cr, 0.1);

  cairo_arc(cr, 0.75, 0.75, 0.75, 2.8, 4.7124);
  cairo_stroke(cr);
  cairo_rectangle(cr, 0.2, 0.0, 0.4, 0.4);
  cairo_fill(cr);
  cairo_move_to(cr, 0.4,  0.2);
  cairo_line_to(cr, 0.5,  1.0);
  cairo_line_to(cr, 0.9,  0.7);
  cairo_close_path(cr);
  cairo_fill(cr);
  POSTAMBLE;
}

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
