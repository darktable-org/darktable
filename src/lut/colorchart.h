#pragma once

#include "common/colorspaces.h"

#include <glib.h>

typedef struct point_t
{
  float x, y;
} point_t;

typedef struct f_line_t
{
  point_t p[4];
} f_line_t;

typedef struct box_t
{
  // position
  point_t p;
  float w, h;
  // color
  dt_colorspaces_color_profile_type_t color_space;
  float color[3]; // either XYZ or Lab, depending on color_space
  float rgb[3];   // color converted to sRGB for rough displaying of patches
} box_t;

typedef struct chart_t
{
  // the F marks
  GList *f_list;
  // the two kinds of boxen
  GHashTable *d_table, *box_table;
  // the box sets. the data is as follows:
  // a hash table with a human readable name as key. the values are GList* with the names of patches.
  // use those to lookup box_table
  GHashTable *patch_sets;
  // the bounding box
  float bb_w, bb_h;
  // other data from the CHT file
  float box_shrink, ref_rotation;
} chart_t;


void free_chart(chart_t *chart);
chart_t* parse_cht(const char* filename);
int parse_it8(const char *filename, chart_t *chart);
void set_color(box_t *box, dt_colorspaces_color_profile_type_t color_space, float c0, float c1, float c2);


// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
