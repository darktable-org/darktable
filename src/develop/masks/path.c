/*
    This file is part of darktable,
    Copyright (C) 2013-2021 darktable developers.

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
#include "bauhaus/bauhaus.h"
#include "common/debug.h"
#include "common/imagebuf.h"
#include "common/undo.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"
#include "develop/openmp_maths.h"
#include <assert.h>

static void _path_bounding_box_raw(const float *const points, const float *border, const int nb_corner, const int num_points, int num_borders,
                                   float *x_min, float *x_max, float *y_min, float *y_max);

/** get the point of the path at pos t [0,1]  */
static void _path_get_XY(float p0x, float p0y, float p1x, float p1y, float p2x, float p2y, float p3x,
                         float p3y, float t, float *x, float *y)
{
  const float ti = 1.0f - t;
  const float a = ti * ti * ti;
  const float b = 3.0f * t * ti * ti;
  const float c = 3.0f * t * t * ti;
  const float d = t * t * t;
  *x = p0x * a + p1x * b + p2x * c + p3x * d;
  *y = p0y * a + p1y * b + p2y * c + p3y * d;
}

/** get the point of the path at pos t [0,1]  AND the corresponding border point */
static void _path_border_get_XY(float p0x, float p0y, float p1x, float p1y, float p2x, float p2y, float p3x,
                                float p3y, float t, float rad, float *xc, float *yc, float *xb, float *yb)
{
  // we use double precision math here to avoid rounding issues in paths with sharp corners
  // we get the point
  _path_get_XY(p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y, t, xc, yc);

  // now we get derivative points
  const double ti = 1.0 - (double)t;

  const double t_t = (double)t * t;
  const double ti_ti = ti * ti;
  const double t_ti = t * ti;

  const double a = 3.0 * ti_ti;
  const double b = 3.0 * (ti_ti - 2.0 * t_ti);
  const double c = 3.0 * (2.0 * t_ti - t_t);
  const double d = 3.0 * t_t;

  const double dx = -p0x * a + p1x * b + p2x * c + p3x * d;
  const double dy = -p0y * a + p1y * b + p2y * c + p3y * d;

  // so we can have the resulting point
  if(dx == 0 && dy == 0)
  {
    *xb = NAN;
    *yb = NAN;
    return;
  }
  const double l = 1.0 / sqrt(dx * dx + dy * dy);
  *xb = (*xc) + rad * dy * l;
  *yb = (*yc) - rad * dx * l;
}

/** get feather extremity from the control point n°2 */
/** the values should be in orthonormal space */
static void _path_ctrl2_to_feather(float ptx, float pty, float ctrlx, float ctrly, float *fx, float *fy,
                                   gboolean clockwise)
{
  if(clockwise)
  {
    *fx = ptx + ctrly - pty;
    *fy = pty + ptx - ctrlx;
  }
  else
  {
    *fx = ptx - ctrly + pty;
    *fy = pty - ptx + ctrlx;
  }
}

/** get bezier control points from feather extremity */
/** the values should be in orthonormal space */
static void _path_feather_to_ctrl(float ptx, float pty, float fx, float fy,
                                  float *ctrl1x, float *ctrl1y, float *ctrl2x, float *ctrl2y,
                                  gboolean clockwise)
{
  if(clockwise)
  {
    *ctrl2x = ptx + pty - fy;
    *ctrl2y = pty + fx - ptx;
    *ctrl1x = ptx - pty + fy;
    *ctrl1y = pty - fx + ptx;
  }
  else
  {
    *ctrl1x = ptx + pty - fy;
    *ctrl1y = pty + fx - ptx;
    *ctrl2x = ptx - pty + fy;
    *ctrl2y = pty - fx + ptx;
  }
}

/** Get the control points of a segment to match exactly a catmull-rom spline */
static void _path_catmull_to_bezier(float x1, float y1, float x2, float y2, float x3, float y3, float x4,
                                    float y4, float *bx1, float *by1, float *bx2, float *by2)
{
  *bx1 = (-x1 + 6 * x2 + x3) / 6;
  *by1 = (-y1 + 6 * y2 + y3) / 6;
  *bx2 = (x2 + 6 * x3 - x4) / 6;
  *by2 = (y2 + 6 * y3 - y4) / 6;
}

/** initialise all control points to eventually match a catmull-rom like spline */
static void _path_init_ctrl_points(dt_masks_form_t *form)
{
  // if we have less that 3 points, what to do ??
  const guint nb = g_list_length(form->points);
  if(nb < 2) return;

  const GList *form_points = form->points;
  for(int k = 0; k < nb; k++)
  {
    dt_masks_point_path_t *point3 = (dt_masks_point_path_t *)form_points->data;
    // if the point has not been set manually, we redefine it
    if(point3->state & DT_MASKS_POINT_STATE_NORMAL)
    {
      // we want to get point-2 (into pt1), point-1 (into pt2), point+1 (into pt4), point+2 (into pt5), wrapping
      // around to the other end of the list
      const GList *pt2 = g_list_prev_wraparound(form_points); // prev, wrapping around if already on first element
      const GList *pt1 = g_list_prev_wraparound(pt2);
      const GList *pt4 = g_list_next_wraparound(form_points, form->points); // next, wrapping around if on last element
      const GList *pt5 = g_list_next_wraparound(pt4, form->points);
      dt_masks_point_path_t *point1 = (dt_masks_point_path_t *)pt1->data;
      dt_masks_point_path_t *point2 = (dt_masks_point_path_t *)pt2->data;
      dt_masks_point_path_t *point4 = (dt_masks_point_path_t *)pt4->data;
      dt_masks_point_path_t *point5 = (dt_masks_point_path_t *)pt5->data;

      float bx1 = 0.0f, by1 = 0.0f, bx2 = 0.0f, by2 = 0.0f;
      _path_catmull_to_bezier(point1->corner[0], point1->corner[1], point2->corner[0], point2->corner[1],
                              point3->corner[0], point3->corner[1], point4->corner[0], point4->corner[1],
                              &bx1, &by1, &bx2, &by2);
      if(point2->ctrl2[0] == -1.0) point2->ctrl2[0] = bx1;
      if(point2->ctrl2[1] == -1.0) point2->ctrl2[1] = by1;
      point3->ctrl1[0] = bx2;
      point3->ctrl1[1] = by2;
      _path_catmull_to_bezier(point2->corner[0], point2->corner[1], point3->corner[0], point3->corner[1],
                              point4->corner[0], point4->corner[1], point5->corner[0], point5->corner[1],
                              &bx1, &by1, &bx2, &by2);
      if(point4->ctrl1[0] == -1.0) point4->ctrl1[0] = bx2;
      if(point4->ctrl1[1] == -1.0) point4->ctrl1[1] = by2;
      point3->ctrl2[0] = bx1;
      point3->ctrl2[1] = by1;
    }
    // keep form_points tracking the kth element of form->points
    form_points = g_list_next(form_points);
  }
}

static gboolean _path_is_clockwise(dt_masks_form_t *form)
{
  if(!g_list_shorter_than(form->points,3)) // if we have at least three points...
  {
    float sum = 0.0f;
    for(const GList *form_points = form->points; form_points; form_points = g_list_next(form_points))
    {
      const GList *next = g_list_next_wraparound(form_points, form->points); // next, wrapping around if on last elt
      dt_masks_point_path_t *point1 = (dt_masks_point_path_t *)form_points->data; // kth element of form->points
      dt_masks_point_path_t *point2 = (dt_masks_point_path_t *)next->data;
      sum += (point2->corner[0] - point1->corner[0]) * (point2->corner[1] + point1->corner[1]);
      ;
    }
    return (sum < 0);
  }
  // return dummy answer
  return TRUE;
}

/** fill eventual gaps between 2 points with a line */
static int _path_fill_gaps(int lastx, int lasty, int x, int y, dt_masks_dynbuf_t *points)
{
  dt_masks_dynbuf_reset(points);
  dt_masks_dynbuf_add_2(points, x, y);

  // now we want to be sure everything is continuous
  if(x - lastx > 1)
  {
    for(int j = x - 1; j > lastx; j--)
    {
      const int yyy = (j - lastx) * (y - lasty) / (float)(x - lastx) + lasty;
      const int lasty2 = dt_masks_dynbuf_get(points, -1);
      if(lasty2 - yyy > 1)
      {
        for(int jj = lasty2 + 1; jj < yyy; jj++)
        {
          dt_masks_dynbuf_add_2(points, j, jj);
        }
      }
      else if(lasty2 - yyy < -1)
      {
        for(int jj = lasty2 - 1; jj > yyy; jj--)
        {
          dt_masks_dynbuf_add_2(points, j, jj);
        }
      }
      dt_masks_dynbuf_add_2(points, j, yyy);
    }
  }
  else if(x - lastx < -1)
  {
    for(int j = x + 1; j < lastx; j++)
    {
      int yyy = (j - lastx) * (y - lasty) / (float)(x - lastx) + lasty;
      int lasty2 = dt_masks_dynbuf_get(points, -1);
      if(lasty2 - yyy > 1)
      {
        for(int jj = lasty2 + 1; jj < yyy; jj++)
        {
          dt_masks_dynbuf_add_2(points, j, jj);
        }
      }
      else if(lasty2 - yyy < -1)
      {
        for(int jj = lasty2 - 1; jj > yyy; jj--)
        {
          dt_masks_dynbuf_add_2(points, j, jj);
        }
      }
      dt_masks_dynbuf_add_2(points, j, yyy);
    }
  }
  return 1;
}

/** fill the gap between 2 points with an arc of circle */
/** this function is here because we can have gap in border, esp. if the corner is very sharp */
static void _path_points_recurs_border_gaps(float *cmax, float *bmin, float *bmin2, float *bmax, dt_masks_dynbuf_t *dpoints,
                                            dt_masks_dynbuf_t *dborder, gboolean clockwise)
{
  // we want to find the start and end angles
  double a1 = atan2f(bmin[1] - cmax[1], bmin[0] - cmax[0]);
  double a2 = atan2f(bmax[1] - cmax[1], bmax[0] - cmax[0]);
  if(a1 == a2) return;

  // we have to be sure that we turn in the correct direction
  if(a2 < a1 && clockwise)
  {
    a2 += 2 * M_PI;
  }
  if(a2 > a1 && !clockwise)
  {
    a1 += 2 * M_PI;
  }

  // we determine start and end radius too
  const float r1 = sqrtf((bmin[1] - cmax[1]) * (bmin[1] - cmax[1]) + (bmin[0] - cmax[0]) * (bmin[0] - cmax[0]));
  const float r2 = sqrtf((bmax[1] - cmax[1]) * (bmax[1] - cmax[1]) + (bmax[0] - cmax[0]) * (bmax[0] - cmax[0]));

  // and the max length of the circle arc
  int l = 0;
  if(a2 > a1)
    l = (a2 - a1) * fmaxf(r1, r2);
  else
    l = (a1 - a2) * fmaxf(r1, r2);
  if(l < 2) return;

  // and now we add the points
  const float incra = (a2 - a1) / l;
  const float incrr = (r2 - r1) / l;
  float rr = r1 + incrr;
  float aa = a1 + incra;
  // allocate entries in the dynbufs
  float *dpoints_ptr = dt_masks_dynbuf_reserve_n(dpoints, 2*(l-1));
  float *dborder_ptr = dborder ? dt_masks_dynbuf_reserve_n(dborder, 2*(l-1)) : NULL;
  // and fill them in: the same center pos for each point in dpoints, and the corresponding border point at
  //  successive angular positions for dborder
  if(dpoints_ptr)
  {
    for(int i = 1; i < l; i++)
    {
      *dpoints_ptr++ = cmax[0];
      *dpoints_ptr++ = cmax[1];
      if(dborder_ptr)
      {
        *dborder_ptr++ = cmax[0] + rr * cosf(aa);
        *dborder_ptr++ = cmax[1] + rr * sinf(aa);
      }
      rr += incrr;
      aa += incra;
    }
  }
}

/** recursive function to get all points of the path AND all point of the border */
/** the function take care to avoid big gaps between points */
static void _path_points_recurs(float *p1, float *p2, double tmin, double tmax, float *path_min,
                                float *path_max, float *border_min, float *border_max, float *rpath,
                                float *rborder, dt_masks_dynbuf_t *dpoints, dt_masks_dynbuf_t *dborder,
                                int withborder)
{
  // we calculate points if needed
  if(isnan(path_min[0]))
  {
    _path_border_get_XY(p1[0], p1[1], p1[2], p1[3], p2[2], p2[3], p2[0], p2[1], tmin,
                        p1[4] + (p2[4] - p1[4]) * tmin * tmin * (3.0 - 2.0 * tmin), path_min, path_min + 1,
                        border_min, border_min + 1);
  }
  if(isnan(path_max[0]))
  {
    _path_border_get_XY(p1[0], p1[1], p1[2], p1[3], p2[2], p2[3], p2[0], p2[1], tmax,
                        p1[4] + (p2[4] - p1[4]) * tmax * tmax * (3.0 - 2.0 * tmax), path_max, path_max + 1,
                        border_max, border_max + 1);
  }
  // are the points near ?
  if((tmax - tmin < 0.0001)
     || ((int)path_min[0] - (int)path_max[0] < 1 && (int)path_min[0] - (int)path_max[0] > -1
         && (int)path_min[1] - (int)path_max[1] < 1 && (int)path_min[1] - (int)path_max[1] > -1
         && (!withborder
             || ((int)border_min[0] - (int)border_max[0] < 1 && (int)border_min[0] - (int)border_max[0] > -1
                 && (int)border_min[1] - (int)border_max[1] < 1
                 && (int)border_min[1] - (int)border_max[1] > -1))))
  {
    dt_masks_dynbuf_add_2(dpoints, path_max[0], path_max[1]);
    rpath[0] = path_max[0];
    rpath[1] = path_max[1];

    if(withborder)
    {
      dt_masks_dynbuf_add_2(dborder, border_max[0], border_max[1]);
      rborder[0] = border_max[0];
      rborder[1] = border_max[1];
    }
    return;
  }

  // we split in two part
  double tx = (tmin + tmax) / 2.0;
  float c[2] = { NAN, NAN }, b[2] = { NAN, NAN };
  float rc[2] = { 0 }, rb[2] = { 0 };
  _path_points_recurs(p1, p2, tmin, tx, path_min, c, border_min, b, rc, rb, dpoints, dborder, withborder);
  _path_points_recurs(p1, p2, tx, tmax, rc, path_max, rb, border_max, rpath, rborder, dpoints, dborder, withborder);
}

/** find all self intersections in a path */
static int _path_find_self_intersection(dt_masks_dynbuf_t *inter, int nb_corners, float *border, int border_count)
{
  if(nb_corners == 0 || border_count == 0) return 0;

  int inter_count = 0;

  // we search extreme points in x and y
  int xmin = INT_MAX, xmax = INT_MIN, ymin = INT_MAX, ymax = INT_MIN;
  int posextr[4] = { -1 }; // xmin,xmax,ymin,ymax

  for(int i = nb_corners * 3; i < border_count; i++)
  {
    if(isnan(border[i * 2]) || isnan(border[i * 2 + 1]))
    {
      border[i * 2] = border[i * 2 - 2];
      border[i * 2 + 1] = border[i * 2 - 1];
    }
    if(xmin > border[i * 2])
    {
      xmin = border[i * 2];
      posextr[0] = i;
    }
    if(xmax < border[i * 2])
    {
      xmax = border[i * 2];
      posextr[1] = i;
    }
    if(ymin > border[i * 2 + 1])
    {
      ymin = border[i * 2 + 1];
      posextr[2] = i;
    }
    if(ymax < border[i * 2 + 1])
    {
      ymax = border[i * 2 + 1];
      posextr[3] = i;
    }
  }
  xmin -= 1, ymin -= 1;
  xmax += 1, ymax += 1;
  const int hb = ymax - ymin;
  const int wb = xmax - xmin;

  // we allocate the buffer
  const size_t ss = (size_t)hb * wb;
  if(ss < 10 || hb < 0 || wb < 0) return 0;

  int *binter = dt_alloc_align(64, sizeof(int) * ss);
  if(binter == NULL) return 0;
  memset(binter, 0, sizeof(int) * ss);

  dt_masks_dynbuf_t *extra = dt_masks_dynbuf_init(100000, "path extra");
  if(extra == NULL)
  {
    dt_free_align(binter);
    return 0;
  }

  // we'll iterate through all border points, but we can't start at point[0]
  // because it may be in a self-intersected section
  // so we choose a point where we are sure there's no intersection:
  // one from border shape extrema (here x_max)
  int lastx = border[(posextr[1] - 1) * 2];
  int lasty = border[(posextr[1] - 1) * 2 + 1];

  for(int ii = nb_corners * 3; ii < border_count; ii++)
  {
    // we want to loop from one border extremity
    int i = ii - nb_corners * 3 + posextr[1];
    if(i >= border_count) i = i - border_count + nb_corners * 3;

    if(inter_count >= nb_corners * 4) break;

    // we want to be sure everything is continuous
    _path_fill_gaps(lastx, lasty, border[i * 2], border[i * 2 + 1], extra);

    // extra represent all the points between the last one and the current one
    // for all the points in extra, we'll check for self-intersection
    // and "register" them in binter
    for(int j = dt_masks_dynbuf_position(extra) / 2 - 1; j >= 0; j--)
    {
      const int xx = (dt_masks_dynbuf_buffer(extra))[j * 2];
      const int yy = (dt_masks_dynbuf_buffer(extra))[j * 2 + 1];

      // we check also 2 points around to be sure catching intersection
      int v[3] = { 0 };
      const int idx = (yy - ymin) * wb + (xx - xmin);
      if(idx < 0 || idx > ss)
      {
        dt_free_align(binter);
        return 0;
      }
      v[0] = binter[idx];
      if(xx > xmin) v[1] = binter[idx - 1];
      if(yy > ymin) v[2] = binter[idx - wb];

      for(int k = 0; k < 3; k++)
      {
        if(v[k] > 0)
        {
          // there's already a border point "registered" at this coordinate.
          // so we've potentially found a self-intersection portion between v[k] and i
          if((xx == lastx && yy == lasty) || v[k] == i - 1)
          {
            // we haven't move from last point.
            // this is not a real self-interesection, so we just update binter
            binter[idx] = i;
          }
          else if((i > v[k]
                   && ((posextr[0] < v[k] || posextr[0] > i) && (posextr[1] < v[k] || posextr[1] > i)
                       && (posextr[2] < v[k] || posextr[2] > i) && (posextr[3] < v[k] || posextr[3] > i)))
                  || (i < v[k] && posextr[0] < v[k] && posextr[0] > i && posextr[1] < v[k] && posextr[1] > i
                      && posextr[2] < v[k] && posextr[2] > i && posextr[3] < v[k] && posextr[3] > i))
          {
            // we have found a self-intersection portion, between v[k] and i
            // and we are sure that this portion doesn't include one of the shape extrema
            if(inter_count > 0)
            {
              if((v[k] - i) * ((int)dt_masks_dynbuf_get(inter, -2) - (int)dt_masks_dynbuf_get(inter, -1)) > 0
                 && (int)dt_masks_dynbuf_get(inter, -2) >= v[k] && (int)dt_masks_dynbuf_get(inter, -1) <= i)
              {
                // we find an self-intersection portion which include the last one
                // we just update it
                dt_masks_dynbuf_set(inter, -2, v[k]);
                dt_masks_dynbuf_set(inter, -1, i);
              }
              else
              {
                // we find a new self-intersection portion
                dt_masks_dynbuf_add_2(inter, v[k], i);
                inter_count++;
              }
            }
            else
            {
              // we find a new self-intersection portion
              dt_masks_dynbuf_add_2(inter, v[k], i);
              inter_count++;
            }
          }
        }
        else
        {
          // there wasn't anything "registered" at this place in binter
          // we do it now
          binter[idx] = i;
        }
      }
      lastx = xx;
      lasty = yy;
    }
  }

  dt_masks_dynbuf_free(extra);
  dt_free_align(binter);

  // and we return the number of self-intersection found
  return inter_count;
}

/** get all points of the path and the border */
/** this take care of gaps and self-intersection and iop distortions */
static int _path_get_pts_border(dt_develop_t *dev, dt_masks_form_t *form, const double iop_order, const int transf_direction,
                                   dt_dev_pixelpipe_t *pipe, float **points, int *points_count,
                                   float **border, int *border_count, gboolean source)
{
  double start2 = 0.0;

  if(darktable.unmuted & DT_DEBUG_PERF) start2 = dt_get_wtime();

  const float wd = pipe->iwidth, ht = pipe->iheight;
  const guint nb = g_list_length(form->points);

  dt_masks_dynbuf_t *dpoints = NULL, *dborder = NULL, *intersections = NULL;

  *points = NULL;
  *points_count = 0;
  if(border) *border = NULL;
  if(border) *border_count = 0;

  dpoints = dt_masks_dynbuf_init(1000000, "path dpoints");
  if(dpoints == NULL) return 0;

  if(border)
  {
    dborder = dt_masks_dynbuf_init(1000000, "path dborder");
    if(dborder == NULL)
    {
      dt_masks_dynbuf_free(dpoints);
      return 0;
    }
  }

  intersections = dt_masks_dynbuf_init(10 * MAX(nb, 1), "path intersections");
  if(intersections == NULL)
  {
    dt_masks_dynbuf_free(dpoints);
    dt_masks_dynbuf_free(dborder);
    return 0;
  }

  // we store all points
  float dx = 0.0f, dy = 0.0f;

  if(source && nb > 0 && transf_direction != DT_DEV_TRANSFORM_DIR_ALL)
  {
    dt_masks_point_path_t *pt = (dt_masks_point_path_t *)form->points->data;
    dx = (pt->corner[0] - form->source[0]) * wd;
    dy = (pt->corner[1] - form->source[1]) * ht;
  }
  for(const GList *l = form->points; l; l = g_list_next(l))
  {
    const dt_masks_point_path_t *const pt = (dt_masks_point_path_t *)l->data;
    float *const buf = dt_masks_dynbuf_reserve_n(dpoints, 6);
    if(buf)
    {
      buf[0] = pt->ctrl1[0] * wd - dx;
      buf[1] = pt->ctrl1[1] * ht - dy;
      buf[2] = pt->corner[0] * wd - dx;
      buf[3] = pt->corner[1] * ht - dy;
      buf[4] = pt->ctrl2[0] * wd - dx;
      buf[5] = pt->ctrl2[1] * ht - dy;
    }
  }
  // for the border, we store value too
  if(dborder)
  {
    dt_masks_dynbuf_add_zeros(dborder, 6 * nb);  // need six zeros for each border point
  }

  float *border_init = dt_alloc_align_float((size_t)6 * nb);
  int cw = _path_is_clockwise(form);
  if(cw == 0) cw = -1;

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] path_points init took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we render all segments
  const GList *form_points = form->points;
  for(int k = 0; k < nb; k++)
  {
    const int pb = dborder ? dt_masks_dynbuf_position(dborder) : 0;
    border_init[k * 6 + 2] = -pb;
    const GList *pt2 = g_list_next_wraparound(form_points, form->points); // next, wrapping around if on last element
    const GList *pt3 = g_list_next_wraparound(pt2, form->points);
    dt_masks_point_path_t *point1 = (dt_masks_point_path_t *)form_points->data; // kth element of form->points
    dt_masks_point_path_t *point2 = (dt_masks_point_path_t *)pt2->data;
    dt_masks_point_path_t *point3 = (dt_masks_point_path_t *)pt3->data;
    float p1[5] = { point1->corner[0] * wd - dx, point1->corner[1] * ht - dy, point1->ctrl2[0] * wd - dx,
                    point1->ctrl2[1] * ht - dy, cw * point1->border[1] * MIN(wd, ht) };
    float p2[5] = { point2->corner[0] * wd - dx, point2->corner[1] * ht - dy, point2->ctrl1[0] * wd - dx,
                    point2->ctrl1[1] * ht - dy, cw * point2->border[0] * MIN(wd, ht) };
    float p3[5] = { point2->corner[0] * wd - dx, point2->corner[1] * ht - dy, point2->ctrl2[0] * wd - dx,
                    point2->ctrl2[1] * ht - dy, cw * point2->border[1] * MIN(wd, ht) };
    float p4[5] = { point3->corner[0] * wd - dx, point3->corner[1] * ht - dy, point3->ctrl1[0] * wd - dx,
                    point3->ctrl1[1] * ht - dy, cw * point3->border[0] * MIN(wd, ht) };

    // advance form_points for next iteration so that it tracks the kth element of form->points
    form_points = g_list_next(form_points);

    // and we determine all points by recursion (to be sure the distance between 2 points is <=1)
    float rc[2] = { 0 }, rb[2] = { 0 };
    float bmin[2] = { NAN, NAN };
    float bmax[2] = { NAN, NAN };
    float cmin[2] = { NAN, NAN };
    float cmax[2] = { NAN, NAN };

    _path_points_recurs(p1, p2, 0.0, 1.0, cmin, cmax, bmin, bmax, rc, rb, dpoints, dborder, border && (nb >= 3));

    // we check gaps in the border (sharp edges)
    if(dborder && (fabs(dt_masks_dynbuf_get(dborder, -2) - rb[0]) > 1.0f
                   || fabs(dt_masks_dynbuf_get(dborder, -1) - rb[1]) > 1.0f))
    {
      bmin[0] = dt_masks_dynbuf_get(dborder, -2);
      bmin[1] = dt_masks_dynbuf_get(dborder, -1);
    }

    dt_masks_dynbuf_add_2(dpoints, rc[0], rc[1]);

    border_init[k * 6 + 4] = dborder ? -dt_masks_dynbuf_position(dborder) : 0;

    if(dborder)
    {
      if(isnan(rb[0]))
      {
        if(isnan(dt_masks_dynbuf_get(dborder, - 2)))
        {
          dt_masks_dynbuf_set(dborder, -2, dt_masks_dynbuf_get(dborder, -4));
          dt_masks_dynbuf_set(dborder, -1, dt_masks_dynbuf_get(dborder, -3));
        }
        rb[0] = dt_masks_dynbuf_get(dborder, -2);
        rb[1] = dt_masks_dynbuf_get(dborder, -1);
      }
      dt_masks_dynbuf_add_2(dborder, rb[0], rb[1]);

      (dt_masks_dynbuf_buffer(dborder))[k * 6] = border_init[k * 6] = (dt_masks_dynbuf_buffer(dborder))[pb];
      (dt_masks_dynbuf_buffer(dborder))[k * 6 + 1] = border_init[k * 6 + 1] = (dt_masks_dynbuf_buffer(dborder))[pb + 1];
    }

    // we first want to be sure that there are no gaps in border
    if(dborder && nb >= 3)
    {
      // we get the next point (start of the next segment)
      // t=0.00001f to workaround rounding effects with full optimization that result in bmax[0] NOT being set to
      // NAN when t=0 and the two points in p3 are identical (as is the case on a control node set to sharp corner)
      _path_border_get_XY(p3[0], p3[1], p3[2], p3[3], p4[2], p4[3], p4[0], p4[1], 0.00001f, p3[4], cmin, cmin + 1,
                          bmax, bmax + 1);
      if(isnan(bmax[0]))
      {
        _path_border_get_XY(p3[0], p3[1], p3[2], p3[3], p4[2], p4[3], p4[0], p4[1], 0.00001f, p3[4], cmin,
                            cmin + 1, bmax, bmax + 1);
      }
      if(bmax[0] - rb[0] > 1 || bmax[0] - rb[0] < -1 || bmax[1] - rb[1] > 1 || bmax[1] - rb[1] < -1)
      {
        float bmin2[2] = { dt_masks_dynbuf_get(dborder, -22), dt_masks_dynbuf_get(dborder, -21) };
        _path_points_recurs_border_gaps(rc, rb, bmin2, bmax, dpoints, dborder, _path_is_clockwise(form));
      }
    }
  }

  *points_count = dt_masks_dynbuf_position(dpoints) / 2;
  *points = dt_masks_dynbuf_harvest(dpoints);
  dt_masks_dynbuf_free(dpoints);

  if(dborder)
  {
    *border_count = dt_masks_dynbuf_position(dborder) / 2;
    *border = dt_masks_dynbuf_harvest(dborder);
    dt_masks_dynbuf_free(dborder);
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] path_points point recurs %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we don't want the border to self-intersect
  int inter_count = 0;
  if(border)
  {
    inter_count = _path_find_self_intersection(intersections, nb, *border, *border_count);

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_print(DT_DEBUG_MASKS, "[masks %s] path_points self-intersect took %0.04f sec\n", form->name,
               dt_get_wtime() - start2);
      start2 = dt_get_wtime();
    }
  }

  // and we transform them with all distorted modules
  if(source && transf_direction == DT_DEV_TRANSFORM_DIR_ALL)
  {
    // we transform with all distortion that happen *before* the module
    // so we have now the TARGET points in module input reference
    if(dt_dev_distort_transform_plus(dev, pipe, iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL, *points, *points_count))
    {
      // now we move all the points by the shift
      // so we have now the SOURCE points in module input reference
      float pts[2] = { form->source[0] * wd, form->source[1] * ht };
      if(!dt_dev_distort_transform_plus(dev, pipe, iop_order, DT_DEV_TRANSFORM_DIR_BACK_EXCL, pts, 1)) goto fail;

      dx = pts[0] - (*points)[2];
      dy = pts[1] - (*points)[3];

#ifdef _OPENMP
#pragma omp parallel for simd default(none) \
    dt_omp_firstprivate(points_count, points, dx, dy) \
    schedule(static) if(*points_count > 100) aligned(points:64)
#endif
      for(int i = 0; i < *points_count; i++)
      {
        (*points)[i * 2]     += dx;
        (*points)[i * 2 + 1] += dy;
      }

      // we apply the rest of the distortions (those after the module)
      // so we have now the SOURCE points in final image reference
      if(!dt_dev_distort_transform_plus(dev, pipe, iop_order, DT_DEV_TRANSFORM_DIR_FORW_INCL, *points,
                                        *points_count))
        goto fail;
    }

    if(darktable.unmuted & DT_DEBUG_PERF)
      dt_print(DT_DEBUG_MASKS, "[masks %s] path_points end took %0.04f sec\n", form->name, dt_get_wtime() - start2);

    dt_masks_dynbuf_free(intersections);
    dt_free_align(border_init);
    return 1;
  }
  else if(dt_dev_distort_transform_plus(dev, pipe, iop_order, transf_direction, *points, *points_count))
  {
    if(!border || dt_dev_distort_transform_plus(dev, pipe, iop_order, transf_direction, *border, *border_count))
    {
      if(darktable.unmuted & DT_DEBUG_PERF)
      {
        dt_print(DT_DEBUG_MASKS, "[masks %s] path_points transform took %0.04f sec\n", form->name,
                 dt_get_wtime() - start2);
        start2 = dt_get_wtime();
      }

      if(border)
      {
        // we don't want to copy the falloff points
        for(int k = 0; k < nb; k++)
          for(int i = 2; i < 6; i++) (*border)[k * 6 + i] = border_init[k * 6 + i];

        // now we want to write the skipping zones
        for(int i = 0; i < inter_count; i++)
        {
          const int v = (dt_masks_dynbuf_buffer(intersections))[i * 2];
          const int w = (dt_masks_dynbuf_buffer(intersections))[ i * 2 + 1];
          if(v <= w)
          {
            (*border)[v * 2] = NAN;
            (*border)[v * 2 + 1] = w;
          }
          else
          {
            if(w > nb * 3)
            {
              if(isnan((*border)[nb * 6]) && isnan((*border)[nb * 6 + 1]))
                (*border)[nb * 6 + 1] = w;
              else if(isnan((*border)[nb * 6]))
                (*border)[nb * 6 + 1] = MAX((*border)[nb * 6 + 1], w);
              else
                (*border)[nb * 6 + 1] = w;
              (*border)[nb * 6] = NAN;
            }
            (*border)[v * 2] = NAN;
            (*border)[v * 2 + 1] = NAN;
          }
        }
      }

      if(darktable.unmuted & DT_DEBUG_PERF)
        dt_print(DT_DEBUG_MASKS, "[masks %s] path_points end took %0.04f sec\n", form->name,
                 dt_get_wtime() - start2);

      dt_masks_dynbuf_free(intersections);
      dt_free_align(border_init);
      return 1;
    }
  }

  // if we failed, then free all and return
fail:
  dt_masks_dynbuf_free(intersections);
  dt_free_align(border_init);
  dt_free_align(*points);
  *points = NULL;
  *points_count = 0;
  if(border) dt_free_align(*border);
  if(border) *border = NULL;
  if(border) *border_count = 0;
  return 0;
}

/** get the distance between point (x,y) and the path */
static void _path_get_distance(float x, float y, float as, dt_masks_form_gui_t *gui, int index,
                               int corner_count, int *inside, int *inside_border, int *near, int *inside_source, float *dist)
{
  // initialise returned values
  *inside_source = 0;
  *inside = 0;
  *inside_border = 0;
  *near = -1;
  *dist = FLT_MAX;

  if(!gui) return;

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;

  // we first check if we are inside the source form
  if(dt_masks_point_in_form_exact(x, y, gpt->source, corner_count * 6, gpt->source_count))
  {
    *inside_source = 1;
    *inside = 1;

    float x_min = FLT_MAX, y_min = FLT_MAX;
    float x_max = FLT_MIN, y_max = FLT_MIN;

    for(int i = corner_count * 3; i < gpt->source_count; i++)
    {
      const float xx = gpt->source[i * 2];
      const float yy = gpt->source[i * 2 + 1];

      x_min = fminf(x_min, xx);
      x_max = fmaxf(x_max, xx);
      y_min = fminf(y_min, yy);
      y_max = fmaxf(y_max, yy);

      const float dd = sqf(xx - x) + sqf(yy - y);
      *dist = fminf(*dist, dd);
    }

    const float cx = x - (x_min + (x_max - x_min) / 2.0f);
    const float cy = y - (y_min + (y_max - y_min) / 2.0f);
    const float dd = sqf(cx) + sqf(cy);
    *dist = fminf(*dist, dd);

    return;
  }

  // we check if it's inside borders
  if(!dt_masks_point_in_form_exact(x, y, gpt->border, corner_count * 3, gpt->border_count))
    return;

  *inside = 1;

  // and we check if it's inside form
  if(gpt->points_count > 2 + corner_count * 3)
  {
    const float as2 = as * as;
    //float as2 = 1600.0 * as1;
    float last = gpt->points[gpt->points_count * 2 - 1];
    int nb = 0;
    int near_form = 0;
    int current_seg = 1;

    float x_min = FLT_MAX, y_min = FLT_MAX;
    float x_max = FLT_MIN, y_max = FLT_MIN;

    for(int i = corner_count * 3; i < gpt->points_count; i++)
    {
      //if we need to jump to skip points (in case of deleted point, because of self-intersection)
      if(isnan(gpt->points[i * 2]))
      {
        if(isnan(gpt->points[i * 2 + 1])) break;
        i = (int)gpt->points[i * 2 + 1] - 1;
        continue;
      }
      // do we change of path segment ?
      if(gpt->points[i * 2 + 1] == gpt->points[current_seg * 6 + 3]
         && gpt->points[i * 2] == gpt->points[current_seg * 6 + 2])
      {
        current_seg = (current_seg + 1) % corner_count;
      }
      //distance from tested point to current form point
      const float xx = gpt->points[i * 2];
      const float yy = gpt->points[i * 2 + 1];

      x_min = fminf(x_min, xx);
      x_max = fmaxf(x_max, xx);
      y_min = fminf(y_min, yy);
      y_max = fmaxf(y_max, yy);

      const float dd = sqf(xx - x) + sqf(yy - y);
      *dist = fminf(*dist, dd);

      if(dd < as2)
      {
        near_form = 1;
        if(current_seg == 0)
          *near = corner_count - 1;
        else
          *near = current_seg - 1;
      }

      if(((y<=yy && y>last) || (y>=yy && y<last)) && (gpt->points[i * 2] > x)) nb++;

      last = yy;
    }
    *inside_border = !((nb & 1) || (near_form));

    const float cx = x - (x_min + (x_max - x_min) / 2.0f);
    const float cy = y - (y_min + (y_max - y_min) / 2.0f);
    const float dd = sqf(cx) + sqf(cy);
    *dist = fminf(*dist, dd);
  }
  else *inside_border = 1;
}

static int _path_get_points_border(dt_develop_t *dev, dt_masks_form_t *form, float **points, int *points_count,
                                   float **border, int *border_count, int source, const dt_iop_module_t *module)
{
  if(source && !module) return 0;
  const double ioporder = (module) ? module->iop_order : 0.0f;
  return _path_get_pts_border(dev, form, ioporder, DT_DEV_TRANSFORM_DIR_ALL, dev->preview_pipe, points,
                              points_count, border, border_count, source);
}

static int _path_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up,
                                       uint32_t state, dt_masks_form_t *form, int parentid,
                                       dt_masks_form_gui_t *gui, int index)
{
  // resize a shape even if on a node or segment
  if(gui->form_selected || gui->point_selected >= 0 || gui->feather_selected >= 0 || gui->seg_selected >= 0
     || gui->point_border_selected >= 0)
  {
    // we register the current position
    if(gui->scrollx == 0.0f && gui->scrolly == 0.0f)
    {
      gui->scrollx = pzx;
      gui->scrolly = pzy;
    }
    if(dt_modifier_is(state, GDK_CONTROL_MASK))
    {
      // we try to change the opacity
      dt_masks_form_change_opacity(form, parentid, up ? 0.05f : -0.05f);
    }
    else
    {
      const float amount = up ? 1.03f : 1.0f / 1.03f;
      // resize don't care where the mouse is inside a shape
      if(dt_modifier_is(state, GDK_SHIFT_MASK))
      {
        float feather_size = 0.0f;

        // do not exceed upper limit of 1.0
        for(const GList *l = form->points; l; l = g_list_next(l))
        {
          const dt_masks_point_path_t *point = (dt_masks_point_path_t *)l->data;
          if(amount > 1.0f && (point->border[0] > 1.0f || point->border[1] > 1.0f)) return 1;
        }
        for(const GList *l = form->points; l; l = g_list_next(l))
        {
          dt_masks_point_path_t *point = (dt_masks_point_path_t *)l->data;
          point->border[0] *= amount;
          point->border[1] *= amount;
          feather_size += point->border[0] + point->border[1];
        }
        float masks_border = dt_conf_get_float(DT_MASKS_CONF(form->type, path, border));
        masks_border = MAX(0.0005f, MIN(masks_border * amount, 0.5f));
        dt_conf_set_float(DT_MASKS_CONF(form->type, path, border), masks_border);
        dt_toast_log(_("feather size: %3.2f%%"), feather_size * 50.0f / g_list_length(form->points));
      }
      else if(gui->edit_mode == DT_MASKS_EDIT_FULL)
      {
        // get the center of gravity of the form (like if it was a simple polygon)
        float bx = 0.0f;
        float by = 0.0f;
        float surf = 0.0f;

        for(const GList *form_points = form->points; form_points; form_points = g_list_next(form_points))
        {
          const GList *next = g_list_next_wraparound(form_points, form->points); // next w/ wrap
          dt_masks_point_path_t *point1 = (dt_masks_point_path_t *)form_points->data; // kth element of form->points
          dt_masks_point_path_t *point2 = (dt_masks_point_path_t *)next->data;
          surf += point1->corner[0] * point2->corner[1] - point2->corner[0] * point1->corner[1];

          bx += (point1->corner[0] + point2->corner[0])
                * (point1->corner[0] * point2->corner[1] - point2->corner[0] * point1->corner[1]);
          by += (point1->corner[1] + point2->corner[1])
                * (point1->corner[0] * point2->corner[1] - point2->corner[0] * point1->corner[1]);
        }
        bx /= 3.0f * surf;
        by /= 3.0f * surf;

        surf = sqrtf(fabsf(surf));
        if(amount < 1.0f && surf < 0.001f) return 1;
        if(amount > 1.0f && surf > 2.0f) return 1;

        // now we move each point
        for(GList *l = form->points; l; l = g_list_next(l))
        {
          dt_masks_point_path_t *point = (dt_masks_point_path_t *)l->data;
          const float x = (point->corner[0] - bx) * amount;
          const float y = (point->corner[1] - by) * amount;

          // we stretch ctrl points
          const float ct1x = (point->ctrl1[0] - point->corner[0]) * amount;
          const float ct1y = (point->ctrl1[1] - point->corner[1]) * amount;
          const float ct2x = (point->ctrl2[0] - point->corner[0]) * amount;
          const float ct2y = (point->ctrl2[1] - point->corner[1]) * amount;

          // and we set the new points
          point->corner[0] = bx + x;
          point->corner[1] = by + y;
          point->ctrl1[0] = point->corner[0] + ct1x;
          point->ctrl1[1] = point->corner[1] + ct1y;
          point->ctrl2[0] = point->corner[0] + ct2x;
          point->ctrl2[1] = point->corner[1] + ct2y;
        }

        // now the redraw/save stuff
        _path_init_ctrl_points(form);

        dt_toast_log(_("size: %3.2f%%"), surf * amount * 50.0f);
      }
      else
      {
        return 0;
      }

      dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

      // we recreate the form points
      dt_masks_gui_form_create(form, gui, index, module);

      // we save the move
      dt_masks_update_image(darktable.develop);
    }
    return 1;
  }
  return 0;
}

static int _path_events_button_pressed(struct dt_iop_module_t *module, float pzx, float pzy,
                                       double pressure, int which, int type, uint32_t state,
                                       dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, int index)
{
  if(type == GDK_2BUTTON_PRESS || type == GDK_3BUTTON_PRESS) return 1;
  if(!gui) return 0;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return 0;

  float masks_border = MIN(dt_conf_get_float(DT_MASKS_CONF(form->type, path, border)), 0.5f);

  if(gui->creation && which == 1 && form->points == NULL
     && (dt_modifier_is(state, GDK_CONTROL_MASK | GDK_SHIFT_MASK)
         || dt_modifier_is(state, GDK_SHIFT_MASK)))
  {
    // set some absolute or relative position for the source of the clone mask
    if(form->type & DT_MASKS_CLONE) dt_masks_set_source_pos_initial_state(gui, state, pzx, pzy);

    return 1;
  }
  else if(gui->creation && (which == 3 || gui->creation_closing_form))
  {
    // we don't want a form with less than 3 points
    if(g_list_shorter_than(form->points, 4))
    {
      // we don't really have a way to know if the user wants to cancel the continuous add here
      // or just cancelling this mask, let's assume that this is not a mistake and cancel
      // the continuous add
      gui->creation_continuous = FALSE;
      gui->creation_continuous_module = NULL;
      dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
      dt_masks_iop_update(module);
      dt_control_queue_redraw_center();
      return 1;
    }
    else
    {
      dt_iop_module_t *crea_module = gui->creation_module;
      // we delete last point (the one we are currently dragging)
      dt_masks_point_path_t *point = (dt_masks_point_path_t *)g_list_last(form->points)->data;
      form->points = g_list_remove(form->points, point);
      free(point);
      point = NULL;

      gui->point_dragging = -1;
      _path_init_ctrl_points(form);

      // we save the form and quit creation mode
      dt_masks_gui_form_save_creation(darktable.develop, crea_module, form, gui);
      if(crea_module)
      {
        dt_dev_add_history_item(darktable.develop, crea_module, TRUE, TRUE);
        // and we switch in edit mode to show all the forms
        // spots and retouch have their own handling of creation_continuous
        if(gui->creation_continuous
           && (strcmp(crea_module->so->op, "spots") == 0
               || strcmp(crea_module->so->op, "retouch") == 0))
          dt_masks_set_edit_mode_single_form(crea_module, form->formid, DT_MASKS_EDIT_FULL);
        else if(!gui->creation_continuous)
          dt_masks_set_edit_mode(crea_module, DT_MASKS_EDIT_FULL);
        dt_masks_iop_update(crea_module);
        dt_dev_masks_selection_change(darktable.develop, crea_module, form->formid);
        gui->creation_module = NULL;
      }
      else
      {
        dt_dev_masks_selection_change(darktable.develop, NULL, form->formid);
      }

      if(gui->creation_continuous)
      {
        //spot and retouch manage creation_continuous in their own way
        if(strcmp(crea_module->so->op, "spots") != 0 && strcmp(crea_module->so->op, "retouch") != 0)
        {
          dt_iop_gui_blend_data_t *bd = (dt_iop_gui_blend_data_t *)crea_module->blend_data;
          for(int n = 0; n < DEVELOP_MASKS_NB_SHAPES; n++)
            if(bd->masks_type[n] == form->type)
              gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_shapes[n]), TRUE);

          gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bd->masks_edit), FALSE);
          dt_masks_form_t *newform = dt_masks_create(form->type);
          dt_masks_change_form_gui(newform);
          darktable.develop->form_gui->creation = TRUE;
          darktable.develop->form_gui->creation_module = crea_module;
          darktable.develop->form_gui->creation_continuous = TRUE;
          darktable.develop->form_gui->creation_continuous_module = crea_module;
        }
        else
        {
          dt_masks_form_t *form_new = dt_masks_create(form->type);
          dt_masks_change_form_gui(form_new);
          darktable.develop->form_gui->creation_module = gui->creation_continuous_module;
        }
      }
      else if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
      {
        dt_masks_form_t *grp = darktable.develop->form_visible;
        if(!grp || !(grp->type & DT_MASKS_GROUP)) return 1;
        int pos3 = 0, pos2 = -1;
        for(GList *fs = grp->points; fs; fs = g_list_next(fs))
        {
          dt_masks_point_group_t *pt = (dt_masks_point_group_t *)fs->data;
          if(pt->formid == form->formid)
          {
            pos2 = pos3;
            break;
          }
          pos3++;
        }
        if(pos2 < 0) return 1;
        dt_masks_form_gui_t *gui2 = darktable.develop->form_gui;
        if(!gui2) return 1;
        gui2->group_selected = pos2;

        dt_masks_select_form(crea_module, dt_masks_get_from_id(darktable.develop, form->formid));
      }

      dt_control_queue_redraw_center();
    }
  }
  else if(which == 1)
  {
    if(gui->creation)
    {
      dt_masks_point_path_t *bzpt = (dt_masks_point_path_t *)(malloc(sizeof(dt_masks_point_path_t)));
      int nb = g_list_length(form->points);
      // change the values
      const float wd = darktable.develop->preview_pipe->backbuf_width;
      const float ht = darktable.develop->preview_pipe->backbuf_height;
      float pts[2] = { pzx * wd, pzy * ht };
      dt_dev_distort_backtransform(darktable.develop, pts, 1);

      bzpt->corner[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
      bzpt->corner[1] = pts[1] / darktable.develop->preview_pipe->iheight;
      bzpt->ctrl1[0] = bzpt->ctrl1[1] = bzpt->ctrl2[0] = bzpt->ctrl2[1] = -1.0;
      bzpt->state = DT_MASKS_POINT_STATE_NORMAL;

      bzpt->border[0] = bzpt->border[1] = MAX(0.0005f, masks_border);

      // if that's the first point we should had another one as base point
      if(nb == 0)
      {
        dt_masks_point_path_t *bzpt2 = (dt_masks_point_path_t *)(malloc(sizeof(dt_masks_point_path_t)));
        bzpt2->corner[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
        bzpt2->corner[1] = pts[1] / darktable.develop->preview_pipe->iheight;
        bzpt2->ctrl1[0] = bzpt2->ctrl1[1] = bzpt2->ctrl2[0] = bzpt2->ctrl2[1] = -1.0;
        bzpt2->border[0] = bzpt2->border[1] = MAX(0.0005f, masks_border);
        bzpt2->state = DT_MASKS_POINT_STATE_NORMAL;
        form->points = g_list_append(form->points, bzpt2);

        if(form->type & DT_MASKS_CLONE)
        {
          dt_masks_set_source_pos_initial_value(gui, DT_MASKS_PATH, form, pzx, pzy);
        }
        else
        {
          // not used by regular masks
          form->source[0] = form->source[1] = 0.0f;
        }
        nb++;
      }
      form->points = g_list_append(form->points, bzpt);

      // if this is a ctrl click, the last created point is a sharp one
      if(dt_modifier_is(state, GDK_CONTROL_MASK))
      {
        dt_masks_point_path_t *bzpt3 = g_list_nth_data(form->points, nb - 1);
        bzpt3->ctrl1[0] = bzpt3->ctrl2[0] = bzpt3->corner[0];
        bzpt3->ctrl1[1] = bzpt3->ctrl2[1] = bzpt3->corner[1];
        bzpt3->state = DT_MASKS_POINT_STATE_USER;
      }

      gui->point_dragging = nb;

      _path_init_ctrl_points(form);

      // we recreate the form points
      dt_masks_gui_form_create(form, gui, index, module);

      dt_control_queue_redraw_center();
      return 1;
    }
    else if(gui->source_selected && gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      if(!gpt) return 0;
      // we start the form dragging
      gui->source_dragging = TRUE;
      gui->point_edited = -1;
      gui->dx = gpt->source[2] - gui->posx;
      gui->dy = gpt->source[3] - gui->posy;
      return 1;
    }
    else if(gui->form_selected && gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      gui->form_dragging = TRUE;
      gui->point_edited = -1;
      gui->dx = gpt->points[2] - gui->posx;
      gui->dy = gpt->points[3] - gui->posy;
      return 1;
    }
    else if(gui->point_selected >= 0)
    {
      // if ctrl is pressed, we change the type of point
      if(gui->point_edited == gui->point_selected && dt_modifier_is(state, GDK_CONTROL_MASK))
      {
        dt_masks_point_path_t *point
            = (dt_masks_point_path_t *)g_list_nth_data(form->points, gui->point_edited);
        if(point == NULL)
        {
          gui->point_selected = -1;
          return 1;
        }
        if(point->state != DT_MASKS_POINT_STATE_NORMAL)
        {
          point->state = DT_MASKS_POINT_STATE_NORMAL;
          _path_init_ctrl_points(form);
        }
        else
        {
          point->ctrl1[0] = point->ctrl2[0] = point->corner[0];
          point->ctrl1[1] = point->ctrl2[1] = point->corner[1];
          point->state = DT_MASKS_POINT_STATE_USER;
        }
        dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

        // we recreate the form points
        dt_masks_gui_form_create(form, gui, index, module);
        gpt->clockwise = _path_is_clockwise(form);
        // we save the move
        dt_masks_update_image(darktable.develop);
        return 1;
      }
      // we register the current position to avoid accidental move
      if(gui->point_edited < 0 && gui->scrollx == 0.0f && gui->scrolly == 0.0f)
      {
        gui->scrollx = pzx;
        gui->scrolly = pzy;
      }
      gui->point_edited = gui->point_dragging = gui->point_selected;
      gpt->clockwise = _path_is_clockwise(form);
      dt_control_queue_redraw_center();
      return 1;
    }
    else if(gui->feather_selected >= 0)
    {
      gui->feather_dragging = gui->feather_selected;
      dt_control_queue_redraw_center();
      return 1;
    }
    else if(gui->point_border_selected >= 0)
    {
      gui->point_edited = -1;
      gui->point_border_dragging = gui->point_border_selected;
      dt_control_queue_redraw_center();
      return 1;
    }
    else if(gui->seg_selected >= 0)
    {
      gui->point_edited = -1;
      if(dt_modifier_is(state, GDK_CONTROL_MASK))
      {
        // we add a new point to the path
        dt_masks_point_path_t *bzpt = (dt_masks_point_path_t *)(malloc(sizeof(dt_masks_point_path_t)));
        // change the values
        const float wd = darktable.develop->preview_pipe->backbuf_width;
        const float ht = darktable.develop->preview_pipe->backbuf_height;
        float pts[2] = { pzx * wd, pzy * ht };
        dt_dev_distort_backtransform(darktable.develop, pts, 1);

        bzpt->corner[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
        bzpt->corner[1] = pts[1] / darktable.develop->preview_pipe->iheight;
        bzpt->ctrl1[0] = bzpt->ctrl1[1] = bzpt->ctrl2[0] = bzpt->ctrl2[1] = -1.0;
        bzpt->state = DT_MASKS_POINT_STATE_NORMAL;

        // interpolate the border width of the two neighbour points'
        const GList* first = g_list_nth(form->points, gui->seg_selected);
        const GList* second = g_list_next_wraparound(first, form->points); // next, wrapping around if on last element
        dt_masks_point_path_t *left = (dt_masks_point_path_t *)first->data;
        dt_masks_point_path_t *right = (dt_masks_point_path_t *)second->data;
        bzpt->border[0] = MAX(0.0005f, (left->border[0] + right->border[0]) * 0.5);
        bzpt->border[1] = MAX(0.0005f, (left->border[1] + right->border[1]) * 0.5);

        form->points = g_list_insert(form->points, bzpt, gui->seg_selected + 1);
        _path_init_ctrl_points(form);
        dt_masks_gui_form_create(form, gui, index, module);
        gui->point_edited = gui->point_dragging = gui->point_selected = gui->seg_selected + 1;
        gui->seg_selected = -1;
        dt_control_queue_redraw_center();
      }
      else
      {
        // we move the entire segment
        gui->seg_dragging = gui->seg_selected;
        gui->dx = gpt->points[gui->seg_selected * 6 + 2] - gui->posx;
        gui->dy = gpt->points[gui->seg_selected * 6 + 3] - gui->posy;
      }
      return 1;
    }
    gui->point_edited = -1;
  }
  else if(which == 3 && gui->point_selected >= 0)
  {
    // we remove the point (and the entire form if there is too few points)
    if(g_list_shorter_than(form->points, 4))
    {
      // if the form doesn't belong to a group, we don't delete it
      if(parentid <= 0) return 1;

      // we hide the form
      if(!(darktable.develop->form_visible->type & DT_MASKS_GROUP))
        dt_masks_change_form_gui(NULL);
      else if(g_list_shorter_than(darktable.develop->form_visible->points, 2))
        dt_masks_change_form_gui(NULL);
      else
      {
        const int emode = gui->edit_mode;
        dt_masks_clear_form_gui(darktable.develop);
        for(GList *forms = darktable.develop->form_visible->points; forms; forms = g_list_next(forms))
        {
          dt_masks_point_group_t *guipt = (dt_masks_point_group_t *)forms->data;
          if(guipt->formid == form->formid)
          {
            darktable.develop->form_visible->points
                = g_list_remove(darktable.develop->form_visible->points, guipt);
            free(guipt);
            break;
          }
        }
        gui->edit_mode = emode;
      }

      // we delete or remove the shape
      dt_masks_form_remove(module, NULL, form);
      dt_control_queue_redraw_center();
      return 1;
    }
    dt_masks_point_path_t *point
        = (dt_masks_point_path_t *)g_list_nth_data(form->points, gui->point_selected);
    if(point == NULL)
    {
      gui->point_selected = -1;
      return 1;
    }
    form->points = g_list_remove(form->points, point);
    free(point);
    // form->points = g_list_delete_link(form->points, g_list_nth(form->points, gui->point_selected));
    gui->point_selected = -1;
    _path_init_ctrl_points(form);

    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);
    gpt->clockwise = _path_is_clockwise(form);
    // we save the move
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(which == 3 && gui->feather_selected >= 0)
  {
    dt_masks_point_path_t *point
        = (dt_masks_point_path_t *)g_list_nth_data(form->points, gui->feather_selected);
    if(point != NULL && point->state != DT_MASKS_POINT_STATE_NORMAL)
    {
      point->state = DT_MASKS_POINT_STATE_NORMAL;
      _path_init_ctrl_points(form);

      dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

      // we recreate the form points
      dt_masks_gui_form_create(form, gui, index, module);
      gpt->clockwise = _path_is_clockwise(form);
      // we save the move
      dt_masks_update_image(darktable.develop);
    }
    return 1;
  }
  else if(which == 3 && parentid > 0 && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    // we hide the form
    if(!(darktable.develop->form_visible->type & DT_MASKS_GROUP))
      dt_masks_change_form_gui(NULL);
    else if(g_list_shorter_than(darktable.develop->form_visible->points, 2))
      dt_masks_change_form_gui(NULL);
    else
    {
      dt_masks_clear_form_gui(darktable.develop);
      for(GList *forms = darktable.develop->form_visible->points; forms; forms = g_list_next(forms))
      {
        dt_masks_point_group_t *guipt = (dt_masks_point_group_t *)forms->data;
        if(guipt->formid == form->formid)
        {
          darktable.develop->form_visible->points
              = g_list_remove(darktable.develop->form_visible->points, guipt);
          free(guipt);
          break;
        }
      }
      gui->edit_mode = DT_MASKS_EDIT_FULL;
    }

    // we remove the shape
    dt_dev_masks_list_remove(darktable.develop, form->formid, parentid);
    dt_masks_form_remove(module, dt_masks_get_from_id(darktable.develop, parentid), form);
    return 1;
  }

  return 0;
}

static int _path_events_button_released(struct dt_iop_module_t *module, float pzx, float pzy, int which,
                                        uint32_t state, dt_masks_form_t *form, int parentid,
                                        dt_masks_form_gui_t *gui, int index)
{
  if(!gui) return 0;
  if(gui->creation) return 1;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return 0;
  if(gui->form_dragging)
  {
    // we end the form dragging
    gui->form_dragging = FALSE;

    // we get point0 new values
    dt_masks_point_path_t *point = (dt_masks_point_path_t *)(form->points)->data;
    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    const float dx = pts[0] / darktable.develop->preview_pipe->iwidth - point->corner[0];
    const float dy = pts[1] / darktable.develop->preview_pipe->iheight - point->corner[1];

    // we move all points
    for(GList *points = form->points; points; points = g_list_next(points))
    {
      point = (dt_masks_point_path_t *)points->data;
      point->corner[0] += dx;
      point->corner[1] += dy;
      point->ctrl1[0] += dx;
      point->ctrl1[1] += dy;
      point->ctrl2[0] += dx;
      point->ctrl2[1] += dy;
    }

    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);

    // we save the move
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->source_dragging)
  {
    // we end the form dragging
    gui->source_dragging = FALSE;

    // we change the source value
    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    form->source[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
    form->source[1] = pts[1] / darktable.develop->preview_pipe->iheight;
    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);

    // we save the move
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->seg_dragging >= 0)
  {
    gui->seg_dragging = -1;
    gpt->clockwise = _path_is_clockwise(form);
    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
    dt_masks_update_image(darktable.develop);
    return 1;
  }
  else if(gui->point_dragging >= 0)
  {
    dt_masks_point_path_t *point
        = (dt_masks_point_path_t *)g_list_nth_data(form->points, gui->point_dragging);
    gui->point_dragging = -1;
    if(gui->scrollx != 0.0f || gui->scrolly != 0.0f)
    {
      gui->scrollx = gui->scrolly = 0;
      return 1;
    }
    gui->scrollx = gui->scrolly = 0;
    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd, pzy * ht };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    const float dx = pts[0] / darktable.develop->preview_pipe->iwidth - point->corner[0];
    const float dy = pts[1] / darktable.develop->preview_pipe->iheight - point->corner[1];

    point->corner[0] += dx;
    point->corner[1] += dy;
    point->ctrl1[0] += dx;
    point->ctrl1[1] += dy;
    point->ctrl2[0] += dx;
    point->ctrl2[1] += dy;

    _path_init_ctrl_points(form);

    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);
    gpt->clockwise = _path_is_clockwise(form);
    // we save the move
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->feather_dragging >= 0)
  {
    dt_masks_point_path_t *point
        = (dt_masks_point_path_t *)g_list_nth_data(form->points, gui->feather_dragging);
    gui->feather_dragging = -1;
    const float wd = darktable.develop->preview_pipe->backbuf_width;
    const float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd, pzy * ht };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);

    float p1x, p1y, p2x, p2y;
    _path_feather_to_ctrl(point->corner[0] * darktable.develop->preview_pipe->iwidth,
                          point->corner[1] * darktable.develop->preview_pipe->iheight,
                          pts[0], pts[1],
                          &p1x, &p1y, &p2x, &p2y, gpt->clockwise);
    point->ctrl1[0] = p1x / darktable.develop->preview_pipe->iwidth;
    point->ctrl1[1] = p1y / darktable.develop->preview_pipe->iheight;
    point->ctrl2[0] = p2x / darktable.develop->preview_pipe->iwidth;
    point->ctrl2[1] = p2y / darktable.develop->preview_pipe->iheight;

    point->state = DT_MASKS_POINT_STATE_USER;

    _path_init_ctrl_points(form);

    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);
    gpt->clockwise = _path_is_clockwise(form);
    // we save the move
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->point_border_dragging >= 0)
  {
    gui->point_border_dragging = -1;

    // we save the move
    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);
    dt_masks_update_image(darktable.develop);
    dt_control_queue_redraw_center();
    return 1;
  }

  return 0;
}

static int _path_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy, double pressure,
                                    int which, dt_masks_form_t *form, int parentid,
                                    dt_masks_form_gui_t *gui, int index)
{
  const dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  const int closeup = dt_control_get_dev_closeup();
  const float zoom_scale = dt_dev_get_zoom_scale(darktable.develop, zoom, 1<<closeup, 1);
  // centre view will have zoom_scale * backbuf_width pixels, we want the handle offset to scale with DPI:
  const float as = DT_PIXEL_APPLY_DPI(5) / zoom_scale;  // transformed to backbuf dimensions
  if(!gui) return 0;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return 0;

  const float wd = darktable.develop->preview_pipe->backbuf_width;
  const float ht = darktable.develop->preview_pipe->backbuf_height;

  if(gui->point_dragging >= 0)
  {
    float pts[2] = { pzx * wd, pzy * ht };
    if(gui->creation && !g_list_shorter_than(form->points, 4))
    {
      // if we are near the first point, we have to say that the form should be closed
      if(pts[0] - gpt->points[2] < as && pts[0] - gpt->points[2] > -as
         && pts[1] - gpt->points[3] < as && pts[1] - gpt->points[3] > -as)
      {
        gui->creation_closing_form = TRUE;
      }
      else
      {
        gui->creation_closing_form = FALSE;
      }
    }

    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    dt_masks_point_path_t *bzpt = (dt_masks_point_path_t *)g_list_nth_data(form->points, gui->point_dragging);
    pzx = pts[0] / darktable.develop->preview_pipe->iwidth;
    pzy = pts[1] / darktable.develop->preview_pipe->iheight;

    // if first point, adjust the source accordingly
    if((form->type & DT_MASKS_CLONE)
       && gui->point_dragging == 0)
    {
      form->source[0] += (pzx - bzpt->corner[0]);
      form->source[1] += (pzy - bzpt->corner[1]);
    }

    bzpt->ctrl1[0] += pzx - bzpt->corner[0];
    bzpt->ctrl2[0] += pzx - bzpt->corner[0];
    bzpt->ctrl1[1] += pzy - bzpt->corner[1];
    bzpt->ctrl2[1] += pzy - bzpt->corner[1];
    bzpt->corner[0] = pzx;
    bzpt->corner[1] = pzy;

    _path_init_ctrl_points(form);

    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->seg_dragging >= 0)
  {
    // we get point0 new values
    const GList *const pt = g_list_nth(form->points, gui->seg_dragging);
    const GList *const pt2 = g_list_next_wraparound(pt, form->points);
    dt_masks_point_path_t *point = (dt_masks_point_path_t *)pt->data;
    dt_masks_point_path_t *point2 = (dt_masks_point_path_t *)pt2->data;
    float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    const float dx = pts[0] / darktable.develop->preview_pipe->iwidth - point->corner[0];
    const float dy = pts[1] / darktable.develop->preview_pipe->iheight - point->corner[1];

    // if first or last segment, adjust the source accordingly as the source point
    // is at the end of the first segment and at the start of the last one.
    if((form->type & DT_MASKS_CLONE)
       && (gui->seg_dragging == 0
           || gui->seg_dragging == (g_list_length(form->points) - 1)))
    {
      form->source[0] += dx;
      form->source[1] += dy;
    }

    // we move all points
    point->corner[0] += dx;
    point->corner[1] += dy;
    point->ctrl1[0]  += dx;
    point->ctrl1[1]  += dy;
    point->ctrl2[0]  += dx;
    point->ctrl2[1]  += dy;

    point2->corner[0] += dx;
    point2->corner[1] += dy;
    point2->ctrl1[0]  += dx;
    point2->ctrl1[1]  += dy;
    point2->ctrl2[0]  += dx;
    point2->ctrl2[1]  += dy;

    _path_init_ctrl_points(form);

    dt_dev_add_masks_history_item(darktable.develop, module, TRUE);

    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);

    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->feather_dragging >= 0)
  {
    float pts[2] = { pzx * wd, pzy * ht };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    dt_masks_point_path_t *point
        = (dt_masks_point_path_t *)g_list_nth_data(form->points, gui->feather_dragging);

    float p1x, p1y, p2x, p2y;
    _path_feather_to_ctrl(point->corner[0] * darktable.develop->preview_pipe->iwidth,
                          point->corner[1] * darktable.develop->preview_pipe->iheight,
                          pts[0], pts[1],
                          &p1x, &p1y, &p2x, &p2y, gpt->clockwise);
    point->ctrl1[0] = p1x / darktable.develop->preview_pipe->iwidth;
    point->ctrl1[1] = p1y / darktable.develop->preview_pipe->iheight;
    point->ctrl2[0] = p2x / darktable.develop->preview_pipe->iwidth;
    point->ctrl2[1] = p2y / darktable.develop->preview_pipe->iheight;
    point->state = DT_MASKS_POINT_STATE_USER;

    _path_init_ctrl_points(form);
    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->point_border_dragging >= 0)
  {
    const int k = gui->point_border_dragging;

    // now we want to know the position reflected on actual corner/border segment
    const float a = (gpt->border[k * 6 + 1] - gpt->points[k * 6 + 3])
                    / (float)(gpt->border[k * 6] - gpt->points[k * 6 + 2]);
    const float b = gpt->points[k * 6 + 3] - a * gpt->points[k * 6 + 2];

    float pts[2] = { (a * pzy * ht + pzx * wd - b * a) / (a * a + 1.0), a * pts[0] + b };

    dt_dev_distort_backtransform(darktable.develop, pts, 1);

    dt_masks_point_path_t *point = (dt_masks_point_path_t *)g_list_nth_data(form->points, k);
    const float nx = point->corner[0] * darktable.develop->preview_pipe->iwidth;
    const float ny = point->corner[1] * darktable.develop->preview_pipe->iheight;
    const float nr = sqrtf((pts[0] - nx) * (pts[0] - nx) + (pts[1] - ny) * (pts[1] - ny));
    const float bdr = nr / fminf(darktable.develop->preview_pipe->iwidth, darktable.develop->preview_pipe->iheight);

    point->border[0] = point->border[1] = bdr;

    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->form_dragging || gui->source_dragging)
  {
    float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);

    // we move all points
    if(gui->form_dragging)
    {
      dt_masks_point_path_t *point = (dt_masks_point_path_t *)(form->points)->data;
      const float dx = pts[0] / darktable.develop->preview_pipe->iwidth - point->corner[0];
      const float dy = pts[1] / darktable.develop->preview_pipe->iheight - point->corner[1];
      for(GList *points = form->points; points; points = g_list_next(points))
      {
        point = (dt_masks_point_path_t *)points->data;
        point->corner[0] += dx;
        point->corner[1] += dy;
        point->ctrl1[0] += dx;
        point->ctrl1[1] += dy;
        point->ctrl2[0] += dx;
        point->ctrl2[1] += dy;
      }
    }
    else
    {
      form->source[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
      form->source[1] = pts[1] / darktable.develop->preview_pipe->iheight;
    }

    // we recreate the form points
    dt_masks_gui_form_create(form, gui, index, module);
    dt_control_queue_redraw_center();
    return 1;
  }

  gui->form_selected = FALSE;
  gui->border_selected = FALSE;
  gui->source_selected = FALSE;
  gui->feather_selected = -1;
  gui->point_selected = -1;
  gui->seg_selected = -1;
  gui->point_border_selected = -1;
  // are we near a point or feather ?
  const guint nb = g_list_length(form->points);

  pzx *= darktable.develop->preview_pipe->backbuf_width;
  pzy *= darktable.develop->preview_pipe->backbuf_height;

  if((gui->group_selected == index) && gui->point_edited >= 0)
  {
    const int k = gui->point_edited;
    // we only select feather if the point is not "sharp"
    if(gpt->points[k * 6 + 2] != gpt->points[k * 6 + 4]
       && gpt->points[k * 6 + 3] != gpt->points[k * 6 + 5])
    {
      float ffx, ffy;
      _path_ctrl2_to_feather(gpt->points[k * 6 + 2], gpt->points[k * 6 + 3], gpt->points[k * 6 + 4],
                             gpt->points[k * 6 + 5], &ffx, &ffy, gpt->clockwise);
      if(pzx - ffx > -as && pzx - ffx < as && pzy - ffy > -as && pzy - ffy < as)
      {
        gui->feather_selected = k;
        dt_control_queue_redraw_center();
        return 1;
      }
    }
    // corner ??
    if(pzx - gpt->points[k * 6 + 2] > -as && pzx - gpt->points[k * 6 + 2] < as
       && pzy - gpt->points[k * 6 + 3] > -as && pzy - gpt->points[k * 6 + 3] < as)
    {
      gui->point_selected = k;
      dt_control_queue_redraw_center();
      return 1;
    }
  }

  for(int k = 0; k < nb; k++)
  {
    // corner ??
    if(pzx - gpt->points[k * 6 + 2] > -as && pzx - gpt->points[k * 6 + 2] < as
       && pzy - gpt->points[k * 6 + 3] > -as && pzy - gpt->points[k * 6 + 3] < as)
    {
      gui->point_selected = k;
      dt_control_queue_redraw_center();
      return 1;
    }

    // border corner ??
    if(pzx - gpt->border[k * 6] > -as && pzx - gpt->border[k * 6] < as
       && pzy - gpt->border[k * 6 + 1] > -as && pzy - gpt->border[k * 6 + 1] < as)
    {
      gui->point_border_selected = k;
      dt_control_queue_redraw_center();
      return 1;
    }
  }

  // are we inside the form or the borders or near a segment ???
  int in = 0, inb = 0, near = 0, ins = 0;
  float dist = 0;
  _path_get_distance(pzx, (int)pzy, as, gui, index, nb, &in, &inb, &near, &ins, &dist);
  gui->seg_selected = near;
  if(near < 0)
  {
    if(ins)
    {
      gui->form_selected = TRUE;
      gui->source_selected = TRUE;
    }
    else if(inb)
    {
      gui->form_selected = TRUE;
      gui->border_selected = TRUE;
    }
    else if(in)
    {
      gui->form_selected = TRUE;
    }
  }
  dt_control_queue_redraw_center();
  if(!gui->form_selected && !gui->border_selected && gui->seg_selected < 0) return 0;
  if(gui->edit_mode != DT_MASKS_EDIT_FULL) return 0;
  return 1;
}

static void _path_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *gui, int index, int nb)
{
  double dashed[] = { 4.0, 4.0 };
  dashed[0] /= zoom_scale;
  dashed[1] /= zoom_scale;
  const int len = sizeof(dashed) / sizeof(dashed[0]);
  if(!gui) return;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;

  // draw path
  if(gpt->points_count > nb * 3 + 6)
  {
    cairo_set_dash(cr, dashed, 0, 0);

    cairo_move_to(cr, gpt->points[nb * 6], gpt->points[nb * 6 + 1]);
    int seg = 1, seg2 = 0;
    for(int i = nb * 3; i < gpt->points_count; i++)
    {
      cairo_line_to(cr, gpt->points[i * 2], gpt->points[i * 2 + 1]);
      // we decide to highlight the form segment by segment
      if(gpt->points[i * 2 + 1] == gpt->points[seg * 6 + 3]
         && gpt->points[i * 2] == gpt->points[seg * 6 + 2])
      {
        // this is the end of the last segment, so we have to draw it
        if((gui->group_selected == index)
           && (gui->form_selected || gui->form_dragging || gui->seg_selected == seg2))
          cairo_set_line_width(cr, 5.0 / zoom_scale);
        else
          cairo_set_line_width(cr, 3.0 / zoom_scale);
        dt_draw_set_color_overlay(cr, FALSE, 0.8);
        cairo_stroke_preserve(cr);
        if((gui->group_selected == index)
           && (gui->form_selected || gui->form_dragging || gui->seg_selected == seg2))
          cairo_set_line_width(cr, 2.0 / zoom_scale);
        else
          cairo_set_line_width(cr, 1.0 / zoom_scale);
        dt_draw_set_color_overlay(cr, TRUE, 0.8);
        cairo_stroke(cr);
        // and we update the segment number
        seg = (seg + 1) % nb;
        seg2++;
        cairo_move_to(cr, gpt->points[i * 2], gpt->points[i * 2 + 1]);
      }
    }
  }

  // draw corners
  if(gui->group_selected == index && gpt->points_count > nb * 3 + 6)
  {
    for(int k = 0; k < nb; k++)
      dt_masks_draw_anchor(cr, k == gui->point_dragging || k == gui->point_selected, zoom_scale,
                           gpt->points[k * 6 + 2], gpt->points[k * 6 + 3]);
  }

  // draw feathers
  if((gui->group_selected == index) && gui->point_edited >= 0)
  {
    const int k = gui->point_edited;
    // uncomment this part if you want to see "real" control points
    /*cairo_move_to(cr, gui->points[k*6+2],gui->points[k*6+3]);
    cairo_line_to(cr, gui->points[k*6],gui->points[k*6+1]);
    cairo_stroke(cr);
    cairo_move_to(cr, gui->points[k*6+2],gui->points[k*6+3]);
    cairo_line_to(cr, gui->points[k*6+4],gui->points[k*6+5]);
    cairo_stroke(cr);*/
    float ffx = 0.0f, ffy = 0.0f;
    _path_ctrl2_to_feather(gpt->points[k * 6 + 2], gpt->points[k * 6 + 3], gpt->points[k * 6 + 4],
                           gpt->points[k * 6 + 5], &ffx, &ffy, gpt->clockwise);
    cairo_move_to(cr, gpt->points[k * 6 + 2], gpt->points[k * 6 + 3]);
    cairo_line_to(cr, ffx, ffy);
    cairo_set_line_width(cr, 1.5 / zoom_scale);
    dt_draw_set_color_overlay(cr, FALSE, 0.8);
    cairo_stroke_preserve(cr);
    cairo_set_line_width(cr, 0.75 / zoom_scale);
    dt_draw_set_color_overlay(cr, TRUE, 0.8);
    cairo_stroke(cr);

    if(k == gui->feather_dragging || k == gui->feather_selected)
      cairo_arc(cr, ffx, ffy, 3.0f / zoom_scale, 0, 2.0 * M_PI);
    else
      cairo_arc(cr, ffx, ffy, 1.5f / zoom_scale, 0, 2.0 * M_PI);
    dt_draw_set_color_overlay(cr, TRUE, 0.8);
    cairo_fill_preserve(cr);

    cairo_set_line_width(cr, 1.0 / zoom_scale);
    dt_draw_set_color_overlay(cr, FALSE, 0.8);
    cairo_stroke(cr);
  }

  // draw border and corners
  if((gui->show_all_feathers || gui->group_selected == index) && gpt->border_count > nb * 3 + 6)
  {
    int dep = 1;
    for(int i = nb * 3; i < gpt->border_count; i++)
    {
      if(isnan(gpt->border[i * 2]))
      {
        if(isnan(gpt->border[i * 2 + 1])) break;
        i = gpt->border[i * 2 + 1] - 1;
        continue;
      }
      if(dep)
      {
        cairo_move_to(cr, gpt->border[i * 2], gpt->border[i * 2 + 1]);
        dep = 0;
      }
      else
        cairo_line_to(cr, gpt->border[i * 2], gpt->border[i * 2 + 1]);
    }
    // we execute the drawing
    if(gui->border_selected)
      cairo_set_line_width(cr, 2.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    dt_draw_set_color_overlay(cr, FALSE, 0.8);
    cairo_set_dash(cr, dashed, len, 0);
    cairo_stroke_preserve(cr);
    if(gui->border_selected)
      cairo_set_line_width(cr, 2.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    dt_draw_set_color_overlay(cr, TRUE, 0.8);
    cairo_set_dash(cr, dashed, len, 4);
    cairo_stroke(cr);

    // we draw the path segment by segment
    for(int k = 0; k < nb; k++)
      dt_masks_draw_anchor(cr, gui->point_border_selected == k, zoom_scale, gpt->border[k * 6], gpt->border[k * 6 + 1]);
  }

  // draw a cross where the source will be created
  if(gui->creation
     && darktable.develop->form_visible
     && (darktable.develop->form_visible->type & DT_MASKS_CLONE))
  {
    const int k = nb - 1;
    if((k * 6 + 2) >= 0)
    {
      float x = 0.f, y = 0.f;
      dt_masks_calculate_source_pos_value(gui, DT_MASKS_PATH, gpt->points[2], gpt->points[3],
                                          gpt->points[k * 6 + 2], gpt->points[k * 6 + 3], &x, &y, TRUE);
      dt_masks_draw_clone_source_pos(cr, zoom_scale, x, y);
    }
    else
    {
      float xpos, ypos;
      if((gui->posx == -1.f && gui->posy == -1.f) || gui->mouse_leaved_center)
      {
        xpos = (.5f + dt_control_get_dev_zoom_x()) * darktable.develop->preview_pipe->backbuf_width;
        ypos = (.5f + dt_control_get_dev_zoom_y()) * darktable.develop->preview_pipe->backbuf_height;
      }
      else
      {
        xpos = gui->posx;
        ypos = gui->posy;
      }

      float x = 0.0f, y = 0.0f;
      dt_masks_calculate_source_pos_value(gui, DT_MASKS_PATH, xpos, ypos, xpos, ypos, &x, &y, FALSE);
      dt_masks_draw_clone_source_pos(cr, zoom_scale, x, y);
    }
  }

  // draw the source if needed
  if(!gui->creation && gpt->source_count > nb * 3 + 6)
  {
    // we draw the line between source and dest
    cairo_move_to(cr, gpt->source[2], gpt->source[3]);
    cairo_line_to(cr, gpt->points[2], gpt->points[3]);
    cairo_set_dash(cr, dashed, 0, 0);
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, 2.5 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.5 / zoom_scale);
    dt_draw_set_color_overlay(cr, FALSE, 0.8);
    cairo_stroke_preserve(cr);
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 0.5 / zoom_scale);
    dt_draw_set_color_overlay(cr, TRUE, 0.8);
    cairo_stroke(cr);

    // we draw the source
    cairo_set_dash(cr, dashed, 0, 0);
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, 2.5 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.5 / zoom_scale);
    dt_draw_set_color_overlay(cr, FALSE, 0.8);
    cairo_move_to(cr, gpt->source[nb * 6], gpt->source[nb * 6 + 1]);
    for(int i = nb * 3; i < gpt->source_count; i++)
      cairo_line_to(cr, gpt->source[i * 2], gpt->source[i * 2 + 1]);
    cairo_line_to(cr, gpt->source[nb * 6], gpt->source[nb * 6 + 1]);
    cairo_stroke_preserve(cr);
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 0.5 / zoom_scale);
    dt_draw_set_color_overlay(cr, TRUE, 0.8);
    cairo_stroke(cr);
  }
}

static void _path_bounding_box_raw(const float *const points, const float *border, const int nb_corner, const int num_points, int num_borders,
                                   float *x_min, float *x_max, float *y_min, float *y_max)
{
  float xmin, xmax, ymin, ymax;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  for(int i = nb_corner * 3; i < num_borders; i++)
  {
    // we look at the borders
    const float xx = border[i * 2];
    const float yy = border[i * 2 + 1];
    if(isnan(xx))
    {
     if(isnan(yy)) break; // that means we have to skip the end of the border path
      i = yy - 1;
      continue;
    }
    xmin = MIN(xx, xmin);
    xmax = MAX(xx, xmax);
    ymin = MIN(yy, ymin);
    ymax = MAX(yy, ymax);
  }
  for(int i = nb_corner * 3; i < num_points; i++)
  {
    // we look at the path too
    const float xx = points[i * 2];
    const float yy = points[i * 2 + 1];
    xmin = MIN(xx, xmin);
    xmax = MAX(xx, xmax);
    ymin = MIN(yy, ymin);
    ymax = MAX(yy, ymax);
  }

  *x_min = xmin;
  *x_max = xmax;
  *y_min = ymin;
  *y_max = ymax;
}

static void _path_bounding_box(const float *const points, const float *border, const int nb_corner, const int num_points, int num_borders,
                               int *width, int *height, int *posx, int *posy)
{
  // now we want to find the area, so we search min/max points
  float xmin, xmax, ymin, ymax;
  _path_bounding_box_raw(points, border, nb_corner, num_points, num_borders, &xmin, &xmax, &ymin, &ymax);
  *height = ymax - ymin + 4;
  *width = xmax - xmin + 4;
  *posx = xmin - 2;
  *posy = ymin - 2;
}

static int _get_area(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                     dt_masks_form_t *const form, int *width, int *height, int *posx, int *posy, gboolean get_source)
{
  if(!module) return 0;

  // we get buffers for all points
  float *points = NULL, *border = NULL;
  int points_count = 0, border_count = 0;

  if(!_path_get_pts_border(module->dev, form, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, piece->pipe, &points, &points_count,
                           &border, &border_count, get_source))
  {
    dt_free_align(points);
    dt_free_align(border);
    return 0;
  }

  const guint nb_corner = g_list_length(form->points);
  _path_bounding_box(points, border, nb_corner, points_count, border_count, width, height, posx, posy);

  dt_free_align(points);
  dt_free_align(border);
  return 1;
}

static int _path_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                 dt_masks_form_t *form, int *width, int *height, int *posx, int *posy)
{
  return _get_area(module, piece, form, width, height, posx, posy, TRUE);
}

static int _path_get_area(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                          dt_masks_form_t *const form,
                          int *width, int *height, int *posx, int *posy)
{
  return _get_area(module, piece, form, width, height, posx, posy, FALSE);
}

/** we write a falloff segment */
/*static*/ void _path_falloff(float *const restrict buffer, int *p0, int *p1, int posx, int posy, int bw)
{
  // segment length
  int l = sqrtf(sqf(p1[0] - p0[0]) + sqf(p1[1] - p0[1])) + 1;

  const float lx = p1[0] - p0[0];
  const float ly = p1[1] - p0[1];

  for(int i = 0; i < l; i++)
  {
    // position
    const int x = (int)((float)i * lx / (float)l) + p0[0] - posx;
    const int y = (int)((float)i * ly / (float)l) + p0[1] - posy;
    const float op = 1.0 - (float)i / (float)l;
    size_t idx = y * bw + x;
    buffer[idx] = fmaxf(buffer[idx], op);
    if(x > 0)
      buffer[idx - 1] = fmaxf(buffer[idx - 1], op); // this one is to avoid gap due to int rounding
    if(y > 0)
      buffer[idx - bw] = fmaxf(buffer[idx - bw], op); // this one is to avoid gap due to int rounding
  }
}

static int _path_get_mask(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                          dt_masks_form_t *const form,
                          float **buffer, int *width, int *height, int *posx, int *posy)
{
  if(!module) return 0;
  double start = 0.0;
  double start2 = 0.0;

  if(darktable.unmuted & DT_DEBUG_PERF) start = dt_get_wtime();

  // we get buffers for all points
  float *points = NULL, *border = NULL;
  int points_count, border_count;
  if(!_path_get_pts_border(module->dev, form, module->iop_order,
                           DT_DEV_TRANSFORM_DIR_BACK_INCL, piece->pipe, &points, &points_count,
                           &border, &border_count, FALSE))
  {
    dt_free_align(points);
    dt_free_align(border);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] path points took %0.04f sec\n", form->name, dt_get_wtime() - start);
    start = start2 = dt_get_wtime();
  }

  // now we want to find the area, so we search min/max points
  const guint nb_corner = g_list_length(form->points);
  _path_bounding_box(points, border, nb_corner, points_count, border_count, width, height, posx, posy);

  const int hb = *height;
  const int wb = *width;

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill min max took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // we allocate the buffer
  const size_t bufsize = (size_t)(*width) * (*height);
  // ensure that the buffer is zeroed, as the following code only actually sets the path+falloff pixels
  float *const restrict bufptr = *buffer = dt_calloc_align_float(bufsize);
  if(*buffer == NULL)
  {
    dt_free_align(points);
    dt_free_align(border);
    return 0;
  }

  // we write all the point around the path into the buffer
  const int nbp = border_count;
  if(nbp > 2)
  {
    int lastx = (int)points[(nbp - 1) * 2];
    int lasty = (int)points[(nbp - 1) * 2 + 1];
    int lasty2 = (int)points[(nbp - 2) * 2 + 1];

    int just_change_dir = 0;
    for(int ii = nb_corner * 3; ii < 2 * nbp - nb_corner * 3; ii++)
    {
      // we are writing more than 1 loop in the case the dir in y change
      // exactly at start/end point
      int i = ii;
      if(ii >= nbp) i = (ii - nb_corner * 3) % (nbp - nb_corner * 3) + nb_corner * 3;
      const int xx = (int)points[i * 2];
      const int yy = (int)points[i * 2 + 1];

      // we don't store the point if it has the same y value as the last one
      if(yy == lasty) continue;

      // we want to be sure that there is no y jump
      if(yy - lasty > 1 || yy - lasty < -1)
      {
        if(yy < lasty)
        {
          for(int j = yy + 1; j < lasty; j++)
          {
            const int nx = (j - yy) * (lastx - xx) / (float)(lasty - yy) + xx;
            const size_t idx = (size_t)(j - (*posy)) * (*width) + nx - (*posx);
            assert(idx < bufsize);
            bufptr[idx] = 1.0f;
          }
          lasty2 = yy + 2;
          lasty = yy + 1;
        }
        else
        {
          for(int j = lasty + 1; j < yy; j++)
          {
            const int nx = (j - lasty) * (xx - lastx) / (float)(yy - lasty) + lastx;
            const size_t idx = (size_t)(j - (*posy)) * (*width) + nx - (*posx);
            assert(idx < bufsize);
            bufptr[idx] = 1.0f;
          }
          lasty2 = yy - 2;
          lasty = yy - 1;
        }
      }
      // if we change the direction of the path (in y), then we add a extra point
      if((lasty - lasty2) * (lasty - yy) > 0)
      {
        const size_t idx = (size_t)(lasty - (*posy)) * (*width) + lastx + 1 - (*posx);
        assert(idx < bufsize);
        bufptr[idx] = 1.0f;
        just_change_dir = 1;
      }
      // we add the point
      if(just_change_dir && ii == i)
      {
        // if we have changed the direction, we have to be careful that point can be at the same place
        // as the previous one, especially on sharp edges
        const size_t idx = (size_t)(yy - (*posy)) * (*width) + xx - (*posx);
        assert(idx < bufsize);
        float v = bufptr[idx];
        if(v > 0.0)
        {
          if(xx - (*posx) > 0)
          {
            const size_t idx_ = (size_t)(yy - (*posy)) * (*width) + xx - 1 - (*posx);
            assert(idx_ < bufsize);
            bufptr[idx_] = 1.0f;
          }
          else if(xx - (*posx) < (*width) - 1)
          {
            const size_t idx_ = (size_t)(yy - (*posy)) * (*width) + xx + 1 - (*posx);
            assert(idx_ < bufsize);
            bufptr[idx_] = 1.0f;
          }
        }
        else
        {
          const size_t idx_ = (size_t)(yy - (*posy)) * (*width) + xx - (*posx);
          assert(idx_ < bufsize);
          bufptr[idx_] = 1.0f;
          just_change_dir = 0;
        }
      }
      else
      {
        const size_t idx_ = (size_t)(yy - (*posy)) * (*width) + xx - (*posx);
        assert(idx_ < bufsize);
        bufptr[idx_] = 1.0f;
      }
      // we change last values
      lasty2 = lasty;
      lasty = yy;
      lastx = xx;
      if(ii != i) break;
    }
  }
  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill draw path took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

#ifdef _OPENMP
#pragma omp parallel for \
  dt_omp_firstprivate(hb, wb, bufptr) \
  schedule(static)
#endif
  for(int yy = 0; yy < hb; yy++)
  {
    int state = 0;
    for(int xx = 0; xx < wb; xx++)
    {
      const float v = bufptr[yy * wb + xx];
      if(v == 1.0f) state = !state;
      if(state) bufptr[yy * wb + xx] = 1.0f;
    }
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill fill plain took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // now we fill the falloff
  int p0[2] = { 0 }, p1[2] = { 0 };
  float pf1[2] = { 0.0f };
  int last0[2] = { -100, -100 }, last1[2] = { -100, -100 };
  int next = 0;
  for(int i = nb_corner * 3; i < border_count; i++)
  {
    p0[0] = points[i * 2];
    p0[1] = points[i * 2 + 1];
    if(next > 0)
      p1[0] = pf1[0] = border[next * 2], p1[1] = pf1[1] = border[next * 2 + 1];
    else
      p1[0] = pf1[0] = border[i * 2], p1[1] = pf1[1] = border[i * 2 + 1];

    // now we check p1 value to know if we have to skip a part
    if(next == i) next = 0;
    while(isnan(pf1[0]))
    {
      if(isnan(pf1[1]))
        next = i - 1;
      else
        next = p1[1];
      p1[0] = pf1[0] = border[next * 2];
      p1[1] = pf1[1] = border[next * 2 + 1];
    }

    // and we draw the falloff
    if(last0[0] != p0[0] || last0[1] != p0[1] || last1[0] != p1[0] || last1[1] != p1[1])
    {
      _path_falloff(bufptr, p0, p1, *posx, *posy, *width);
      last0[0] = p0[0];
      last0[1] = p0[1];
      last1[0] = p1[0];
      last1[1] = p1[1];
    }
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill fill falloff took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);

  dt_free_align(points);
  dt_free_align(border);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] path fill buffer took %0.04f sec\n", form->name,
             dt_get_wtime() - start);

  return 1;
}


/** crop path to roi given by xmin, xmax, ymin, ymax. path segments outside of roi are replaced by
    nodes lying on roi borders. */
static int _path_crop_to_roi(float *path, const int point_count, float xmin, float xmax, float ymin,
                             float ymax)
{
  int point_start = -1;
  int l = -1, r = -1;


  // first try to find a node clearly inside roi
  for(int k = 0; k < point_count; k++)
  {
    float x = path[2 * k];
    float y = path[2 * k + 1];

    if(x >= xmin + 1 && y >= ymin + 1
       && x <= xmax - 1 && y <= ymax - 1)
    {
      point_start = k;
      break;
    }
  }

  // printf("crop to xmin %f, xmax %f, ymin %f, ymax %f - start %d (%f, %f)\n", xmin, xmax, ymin, ymax,
  // point_start, path[2*point_start], path[2*point_start+1]);

  if(point_start < 0) return 0; // no point means roi lies completely within path

  // find the crossing points with xmin and replace segment by nodes on border
  for(int k = 0; k < point_count; k++)
  {
    const int kk = (k + point_start) % point_count;

    if(l < 0 && path[2 * kk] < xmin) l = k;       // where we leave roi
    if(l >= 0 && path[2 * kk] >= xmin) r = k - 1; // where we re-enter roi

    // replace that segment
    if(l >= 0 && r >= 0)
    {
      const int count = r - l + 1;
      const int ll = (l - 1 + point_start) % point_count;
      const int rr = (r + 1 + point_start) % point_count;
      const float delta_y = (count == 1) ? 0 : (path[2 * rr + 1] - path[2 * ll + 1]) / (count - 1);
      const float start_y = path[2 * ll + 1];

      for(int n = 0; n < count; n++)
      {
        const int nn = (n + l + point_start) % point_count;
        path[2 * nn] = xmin;
        path[2 * nn + 1] = start_y + n * delta_y;
      }

      l = r = -1;
    }
  }

  // find the crossing points with xmax and replace segment by nodes on border
  for(int k = 0; k < point_count; k++)
  {
    const int kk = (k + point_start) % point_count;

    if(l < 0 && path[2 * kk] > xmax) l = k;       // where we leave roi
    if(l >= 0 && path[2 * kk] <= xmax) r = k - 1; // where we re-enter roi

    // replace that segment
    if(l >= 0 && r >= 0)
    {
      const int count = r - l + 1;
      const int ll = (l - 1 + point_start) % point_count;
      const int rr = (r + 1 + point_start) % point_count;
      const float delta_y = (count == 1) ? 0 : (path[2 * rr + 1] - path[2 * ll + 1]) / (count - 1);
      const float start_y = path[2 * ll + 1];

      for(int n = 0; n < count; n++)
      {
        const int nn = (n + l + point_start) % point_count;
        path[2 * nn] = xmax;
        path[2 * nn + 1] = start_y + n * delta_y;
      }

      l = r = -1;
    }
  }

  // find the crossing points with ymin and replace segment by nodes on border
  for(int k = 0; k < point_count; k++)
  {
    const int kk = (k + point_start) % point_count;

    if(l < 0 && path[2 * kk + 1] < ymin) l = k;       // where we leave roi
    if(l >= 0 && path[2 * kk + 1] >= ymin) r = k - 1; // where we re-enter roi

    // replace that segment
    if(l >= 0 && r >= 0)
    {
      const int count = r - l + 1;
      const int ll = (l - 1 + point_start) % point_count;
      const int rr = (r + 1 + point_start) % point_count;
      const float delta_x = (count == 1) ? 0 : (path[2 * rr] - path[2 * ll]) / (count - 1);
      const float start_x = path[2 * ll];

      for(int n = 0; n < count; n++)
      {
        const int nn = (n + l + point_start) % point_count;
        path[2 * nn] = start_x + n * delta_x;
        path[2 * nn + 1] = ymin;
      }

      l = r = -1;
    }
  }

  // find the crossing points with ymax and replace segment by nodes on border
  for(int k = 0; k < point_count; k++)
  {
    const int kk = (k + point_start) % point_count;

    if(l < 0 && path[2 * kk + 1] > ymax) l = k;       // where we leave roi
    if(l >= 0 && path[2 * kk + 1] <= ymax) r = k - 1; // where we re-enter roi

    // replace that segment
    if(l >= 0 && r >= 0)
    {
      const int count = r - l + 1;
      const int ll = (l - 1 + point_start) % point_count;
      const int rr = (r + 1 + point_start) % point_count;
      const float delta_x = (count == 1) ? 0 : (path[2 * rr] - path[2 * ll]) / (count - 1);
      const float start_x = path[2 * ll];

      for(int n = 0; n < count; n++)
      {
        const int nn = (n + l + point_start) % point_count;
        path[2 * nn] = start_x + n * delta_x;
        path[2 * nn + 1] = ymax;
      }

      l = r = -1;
    }
  }
  return 1;
}

/** we write a falloff segment respecting limits of buffer */
static void _path_falloff_roi(float *buffer, int *p0, int *p1, int bw, int bh)
{
  // segment length
  const int l = sqrt((p1[0] - p0[0]) * (p1[0] - p0[0]) + (p1[1] - p0[1]) * (p1[1] - p0[1])) + 1;

  const float lx = p1[0] - p0[0];
  const float ly = p1[1] - p0[1];

  const int dx = lx < 0 ? -1 : 1;
  const int dy = ly < 0 ? -1 : 1;
  const int dpy = dy * bw;

  for(int i = 0; i < l; i++)
  {
    // position
    const int x = (int)((float)i * lx / (float)l) + p0[0];
    const int y = (int)((float)i * ly / (float)l) + p0[1];
    const float op = 1.0f - (float)i / (float)l;
    float *buf = buffer + (size_t)y * bw + x;
    if(x >= 0 && x < bw && y >= 0 && y < bh)
      buf[0] = MAX(buf[0], op);
    if(x + dx >= 0 && x + dx < bw && y >= 0 && y < bh)
      buf[dx] = MAX(buf[dx], op); // this one is to avoid gap due to int rounding
    if(x >= 0 && x < bw && y + dy >= 0 && y + dy < bh)
      buf[dpy] = MAX(buf[dpy], op); // this one is to avoid gap due to int rounding
  }
}

// build a stamp which can be combined with other shapes in the same group
// prerequisite: 'buffer' is all zeros
static int _path_get_mask_roi(const dt_iop_module_t *const module, const dt_dev_pixelpipe_iop_t *const piece,
                              dt_masks_form_t *const form,
                              const dt_iop_roi_t *roi, float *buffer)
{
  if(!module) return 0;
  double start = 0.0;
  double start2 = 0.0;
  if(darktable.unmuted & DT_DEBUG_PERF) start = dt_get_wtime();

  const int px = roi->x;
  const int py = roi->y;
  const int width = roi->width;
  const int height = roi->height;
  const float scale = roi->scale;

  // we need to take care of four different cases:
  // 1) path and feather are outside of roi
  // 2) path is outside of roi, feather reaches into roi
  // 3) roi lies completely within path
  // 4) all other situations :)
  int path_in_roi = 0;
  int feather_in_roi = 0;
  int path_encircles_roi = 0;

  // we get buffers for all points
  float *points = NULL, *border = NULL;
  int points_count = 0, border_count = 0;
  if(!_path_get_pts_border(module->dev, form, module->iop_order, DT_DEV_TRANSFORM_DIR_BACK_INCL, piece->pipe, &points, &points_count,
                           &border, &border_count, FALSE) || (points_count <= 2))
  {
    dt_free_align(points);
    dt_free_align(border);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] path points took %0.04f sec\n", form->name, dt_get_wtime() - start);
    start = start2 = dt_get_wtime();
  }

  const guint nb_corner = g_list_length(form->points);

  // we shift and scale down path and border
  for(int i = nb_corner * 3; i < border_count; i++)
  {
    const float xx = border[2 * i];
    const float yy = border[2 * i + 1];
    if(isnan(xx))
    {
      if(isnan(yy)) break; // that means we have to skip the end of the border path
      i = yy - 1;
      continue;
    }
    border[2 * i] = xx * scale - px;
    border[2 * i + 1] = yy * scale - py;
  }
  for(int i = nb_corner * 3; i < points_count; i++)
  {
    const float xx = points[2 * i];
    const float yy = points[2 * i + 1];
    points[2 * i] = xx * scale - px;
    points[2 * i + 1] = yy * scale - py;
  }

  // now check if path is at least partially within roi
  for(int i = nb_corner * 3; i < points_count; i++)
  {
    const int xx = points[i * 2];
    const int yy = points[i * 2 + 1];

    if(xx > 1 && yy > 1 && xx < width - 2 && yy < height - 2)
    {
      path_in_roi = 1;
      break;
    }
  }

  // if not this still might mean that path fully encircles roi -> we need to check that
  if(!path_in_roi)
  {
    int nb = 0;
    int last = -9999;
    const int x = width / 2;
    const int y = height / 2;

    for(int i = nb_corner * 3; i < points_count; i++)
    {
      const int yy = (int)points[2 * i + 1];
      if(yy != last && yy == y)
      {
        if(points[2 * i] > x) nb++;
      }
      last = yy;
    }
    // if there is an uneven number of intersection points roi lies within path
    if(nb & 1)
    {
      path_in_roi = 1;
      path_encircles_roi = 1;
    }
  }

  // now check if feather is at least partially within roi
  for(int i = nb_corner * 3; i < border_count; i++)
  {
    const float xx = border[i * 2];
    const float yy = border[i * 2 + 1];
    if(isnan(xx))
    {
      if(isnan(yy)) break; // that means we have to skip the end of the border path
      i = yy - 1;
      continue;
    }
    if(xx > 1 && yy > 1 && xx < width - 2 && yy < height - 2)
    {
      feather_in_roi = 1;
      break;
    }
  }

  // if path and feather completely lie outside of roi -> we're done/mask remains empty
  if(!path_in_roi && !feather_in_roi)
  {
    dt_free_align(points);
    dt_free_align(border);
    return 1;
  }

  // now get min/max values
  float xmin, xmax, ymin, ymax;
  _path_bounding_box_raw(points, border, nb_corner, points_count, border_count, &xmin, &xmax, &ymin, &ymax);

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill min max took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
  {
    dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill clear mask took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // deal with path if it does not lie outside of roi
  if(path_in_roi)
  {
    // second copy of path which we can modify when cropping to roi
    float *cpoints = dt_alloc_align_float((size_t)2 * points_count);
    if(cpoints == NULL)
    {
      dt_free_align(points);
      dt_free_align(border);
      return 0;
    }
    memcpy(cpoints, points, sizeof(float) * 2 * points_count);

    // now we clip cpoints to roi -> catch special case when roi lies completely within path.
    // dirty trick: we allow path to extend one pixel beyond height-1. this avoids need of special handling
    // of the last roi line in the following edge-flag polygon fill algorithm.
    const int crop_success = _path_crop_to_roi(cpoints + 2 * (nb_corner * 3),
                                               points_count - nb_corner * 3, 0,
                                               width - 1, 0, height);
    path_encircles_roi = path_encircles_roi || !crop_success;

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill crop to roi took %0.04f sec\n", form->name,
               dt_get_wtime() - start2);
      start2 = dt_get_wtime();
    }

    if(path_encircles_roi)
    {
      // roi lies completely within path
      for(size_t k = 0; k < (size_t)width * height; k++) buffer[k] = 1.0f;
    }
    else
    {
      // all other cases

      // edge-flag polygon fill: we write all the point around the path into the buffer
      float xlast = cpoints[(points_count - 1) * 2];
      float ylast = cpoints[(points_count - 1) * 2 + 1];

      for(int i = nb_corner * 3; i < points_count; i++)
      {
        float xstart = xlast;
        float ystart = ylast;

        float xend = xlast = cpoints[i * 2];
        float yend = ylast = cpoints[i * 2 + 1];

        if(ystart > yend)
        {
          float tmp;
          tmp = ystart, ystart = yend, yend = tmp;
          tmp = xstart, xstart = xend, xend = tmp;
        }

        const float m = (xstart - xend) / (ystart - yend); // we don't need special handling of ystart==yend
                                                           // as following loop will take care

        for(int yy = (int)ceilf(ystart); (float)yy < yend;
            yy++) // this would normally never touch the last roi line => see comment further above
        {
          const float xcross = xstart + m * (yy - ystart);

          int xx = floorf(xcross);
          if((float)xx + 0.5f <= xcross) xx++;

          if(xx < 0 || xx >= width || yy < 0 || yy >= height)
            continue; // sanity check just to be on the safe side

          const size_t index = (size_t)yy * width + xx;

          buffer[index] = 1.0f - buffer[index];
        }
      }

      if(darktable.unmuted & DT_DEBUG_PERF)
      {
        dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill draw path took %0.04f sec\n", form->name,
                 dt_get_wtime() - start2);
        start2 = dt_get_wtime();
      }

      // we fill the inside plain
      // we don't need to deal with parts of shape outside of roi
      const int xxmin = MAX(xmin, 0);
      const int xxmax = MIN(xmax, width - 1);
      const int yymin = MAX(ymin, 0);
      const int yymax = MIN(ymax, height - 1);

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(xxmin, xxmax, yymin, yymax, width) \
  shared(buffer) schedule(static) num_threads(MIN(8,darktable.num_openmp_threads))
#else
#pragma omp parallel for shared(buffer)
#endif
#endif
      for(int yy = yymin; yy <= yymax; yy++)
      {
        int state = 0;
        for(int xx = xxmin; xx <= xxmax; xx++)
        {
          const size_t index = (size_t)yy * width + xx;
          const float v = buffer[index];
          if(v > 0.5f) state = !state;
          if(state) buffer[index] = 1.0f;
        }
      }

      if(darktable.unmuted & DT_DEBUG_PERF)
      {
        dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill fill plain took %0.04f sec\n", form->name,
                 dt_get_wtime() - start2);
        start2 = dt_get_wtime();
      }
    }
    dt_free_align(cpoints);
  }

  // deal with feather if it does not lie outside of roi
  if(!path_encircles_roi)
  {
    int *dpoints = dt_alloc_align(64, sizeof(int) * 4 * border_count);
    if(dpoints == NULL)
    {
      dt_free_align(points);
      dt_free_align(border);
      return 0;
    }

    int dindex = 0;
    int p0[2], p1[2];
    float pf1[2];
    int last0[2] = { -100, -100 };
    int last1[2] = { -100, -100 };
    int next = 0;
    for(int i = nb_corner * 3; i < border_count; i++)
    {
      p0[0] = floorf(points[i * 2] + 0.5f);
      p0[1] = ceilf(points[i * 2 + 1]);
      if(next > 0)
      {
        p1[0] = pf1[0] = border[next * 2];
        p1[1] = pf1[1] = border[next * 2 + 1];
      }
      else
      {
        p1[0] = pf1[0] = border[i * 2];
        p1[1] = pf1[1] = border[i * 2 + 1];
      }

      // now we check p1 value to know if we have to skip a part
      if(next == i) next = 0;
      while(isnan(pf1[0]))
      {
        if(isnan(pf1[1]))
          next = i - 1;
        else
          next = p1[1];
        p1[0] = pf1[0] = border[next * 2];
        p1[1] = pf1[1] = border[next * 2 + 1];
      }

      // and we draw the falloff
      if(last0[0] != p0[0] || last0[1] != p0[1] || last1[0] != p1[0] || last1[1] != p1[1])
      {
        dpoints[dindex] = p0[0];
        dpoints[dindex + 1] = p0[1];
        dpoints[dindex + 2] = p1[0];
        dpoints[dindex + 3] = p1[1];
        dindex += 4;

        last0[0] = p0[0];
        last0[1] = p0[1];
        last1[0] = p1[0];
        last1[1] = p1[1];
      }
    }

#ifdef _OPENMP
#if !defined(__SUNOS__) && !defined(__NetBSD__)
#pragma omp parallel for default(none) \
  dt_omp_firstprivate(width, height, dindex) \
  shared(buffer, dpoints)
#else
#pragma omp parallel for shared(buffer)
#endif
#endif
    for(int n = 0; n < dindex; n += 4)
      _path_falloff_roi(buffer, dpoints + n, dpoints + n + 2, width, height);

    dt_free_align(dpoints);

    if(darktable.unmuted & DT_DEBUG_PERF)
    {
      dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill fill falloff took %0.04f sec\n", form->name,
               dt_get_wtime() - start2);
    }
  }

  dt_free_align(points);
  dt_free_align(border);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] path fill buffer took %0.04f sec\n", form->name,
             dt_get_wtime() - start);

  return 1;
}

static GSList *_path_setup_mouse_actions(const struct dt_masks_form_t *const form)
{
  GSList *lm = NULL;
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT, 0, _("[PATH creation] add a smooth node"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT, GDK_CONTROL_MASK,
                                     _("[PATH creation] add a sharp node"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_RIGHT, 0, _("[PATH creation] terminate path creation"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT, GDK_CONTROL_MASK,
                                     _("[PATH on node] switch between smooth/sharp node"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_RIGHT, 0, _("[PATH on node] remove the node"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_RIGHT, 0, _("[PATH on feather] reset curvature"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_LEFT, GDK_CONTROL_MASK,
                                     _("[PATH on segment] add node"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, 0, _("[PATH] change size"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, GDK_SHIFT_MASK, _("[PATH] change feather size"));
  lm = dt_mouse_action_create_simple(lm, DT_MOUSE_ACTION_SCROLL, GDK_CONTROL_MASK, _("[PATH] change opacity"));
  return lm;
}

static void _path_sanitize_config(dt_masks_type_t type)
{
  // nothing to do (yet?)
}

static void _path_set_form_name(struct dt_masks_form_t *const form, const size_t nb)
{
  snprintf(form->name, sizeof(form->name), _("path #%d"), (int)nb);
}

static void _path_set_hint_message(const dt_masks_form_gui_t *const gui, const dt_masks_form_t *const form,
                                     const int opacity, char *const restrict msgbuf, const size_t msgbuf_len)
{
  if(gui->creation && g_list_length(form->points) < 4)
    g_strlcat(msgbuf, _("<b>add node</b>: click, <b>add sharp node</b>:ctrl+click\n"
                        "<b>cancel</b>: right-click"), msgbuf_len);
  else if(gui->creation)
    g_strlcat(msgbuf, _("<b>add node</b>: click, <b>add sharp node</b>:ctrl+click\n"
                        "<b>finish path</b>: right-click"), msgbuf_len);
  else if(gui->point_selected >= 0)
    g_strlcat(msgbuf, _("<b>move node</b>: drag, <b>remove node</b>: right-click\n"
                        "<b>switch smooth/sharp mode</b>: ctrl+click"), msgbuf_len);
  else if(gui->feather_selected >= 0)
    g_strlcat(msgbuf, _("<b>node curvature</b>: drag\n<b>reset curvature</b>: right-click"), msgbuf_len);
  else if(gui->seg_selected >= 0)
    g_strlcat(msgbuf, _("<b>move segment</b>: drag\n<b>add node</b>: ctrl+click"), msgbuf_len);
  else if(gui->form_selected)
    g_snprintf(msgbuf, msgbuf_len, _("<b>size</b>: scroll, <b>feather size</b>: shift+scroll\n"
                                     "<b>opacity</b>: ctrl+scroll (%d%%)"), opacity);
}

static void _path_duplicate_points(dt_develop_t *const dev, dt_masks_form_t *const base, dt_masks_form_t *const dest)
{
  (void)dev; // unused arg, keep compiler from complaining
  for(const GList *pts = base->points; pts; pts = g_list_next(pts))
  {
    dt_masks_point_path_t *pt = (dt_masks_point_path_t *)pts->data;
    dt_masks_point_path_t *npt = (dt_masks_point_path_t *)malloc(sizeof(dt_masks_point_path_t));
    memcpy(npt, pt, sizeof(dt_masks_point_path_t));
    dest->points = g_list_append(dest->points, npt);
  }
}

static void _path_initial_source_pos(const float iwd, const float iht, float *x, float *y)
{
  *x = (0.02f * iwd);
  *y = (0.02f * iht);
}

static void _path_modify_property(dt_masks_form_t *const form, dt_masks_property_t prop, float old_val, float new_val, float *sum, int *count, float *min, float *max)
{
  float ratio = (!old_val || !new_val) ? 1.0f : new_val / old_val;

  switch(prop)
  {
    case DT_MASKS_PROPERTY_SIZE:;
      // get the center of gravity of the form (like if it was a simple polygon)
      float bx = 0.0f;
      float by = 0.0f;
      float surf = 0.0f;

      for(const GList *form_points = form->points; form_points; form_points = g_list_next(form_points))
      {
        const GList *next = g_list_next_wraparound(form_points, form->points);
        float *point1 = ((dt_masks_point_path_t *)form_points->data)->corner;
        float *point2 = ((dt_masks_point_path_t *)next->data)->corner;
        surf += point1[0] * point2[1] - point2[0] * point1[1];

        bx += (point1[0] + point2[0]) * (point1[0] * point2[1] - point2[0] * point1[1]);
        by += (point1[1] + point2[1]) * (point1[0] * point2[1] - point2[0] * point1[1]);
      }
      bx /= 3.0f * surf;
      by /= 3.0f * surf;

      if(surf)
      {
        surf = sqrtf(fabsf(surf));
        ratio = fminf(fmaxf(ratio, 0.001f / surf), 2.0f / surf);
      }

      // now we move each point
      for(GList *l = form->points; l; l = g_list_next(l))
      {
        dt_masks_point_path_t *point = (dt_masks_point_path_t *)l->data;
        const float x = (point->corner[0] - bx) * ratio;
        const float y = (point->corner[1] - by) * ratio;

        // we stretch ctrl points
        const float ct1x = (point->ctrl1[0] - point->corner[0]) * ratio;
        const float ct1y = (point->ctrl1[1] - point->corner[1]) * ratio;
        const float ct2x = (point->ctrl2[0] - point->corner[0]) * ratio;
        const float ct2y = (point->ctrl2[1] - point->corner[1]) * ratio;

        // and we set the new points
        point->corner[0] = bx + x;
        point->corner[1] = by + y;
        point->ctrl1[0] = point->corner[0] + ct1x;
        point->ctrl1[1] = point->corner[1] + ct1y;
        point->ctrl2[0] = point->corner[0] + ct2x;
        point->ctrl2[1] = point->corner[1] + ct2y;
      }

      // now the redraw/save stuff
      _path_init_ctrl_points(form);

      surf *= ratio;
      *max = fminf(*max, 2.0f / surf);
      *min = fmaxf(*min, 0.001f / surf);
      *sum += surf / 2.0f;
      ++*count;
      break;
    case DT_MASKS_PROPERTY_FEATHER:;
      for(const GList *l = form->points; l; l = g_list_next(l))
      {
        dt_masks_point_path_t *point = (dt_masks_point_path_t *)l->data;
        point->border[0] = CLAMP(point->border[0] * ratio, 0.0005f, 1.0f);
        point->border[1] = CLAMP(point->border[1] * ratio, 0.0005f, 1.0f);
        *sum += point->border[0] + point->border[1];
        *max = fminf(*max, fminf(1.0f / point->border[0], 1.0f / point->border[1]));
        *min = fmaxf(*min, fmaxf(0.0005f / point->border[0], 0.0005f / point->border[1]));
        *count += 2;
      }
      break;
    default:;
  }
}

// The function table for paths.  This must be public, i.e. no "static" keyword.
const dt_masks_functions_t dt_masks_functions_path = {
  .point_struct_size = sizeof(struct dt_masks_point_path_t),
  .sanitize_config = _path_sanitize_config,
  .setup_mouse_actions = _path_setup_mouse_actions,
  .set_form_name = _path_set_form_name,
  .set_hint_message = _path_set_hint_message,
  .modify_property = _path_modify_property,
  .duplicate_points = _path_duplicate_points,
  .initial_source_pos = _path_initial_source_pos,
  .get_distance = _path_get_distance,
  .get_points_border = _path_get_points_border,
  .get_mask = _path_get_mask,
  .get_mask_roi = _path_get_mask_roi,
  .get_area = _path_get_area,
  .get_source_area = _path_get_source_area,
  .mouse_moved = _path_events_mouse_moved,
  .mouse_scrolled = _path_events_mouse_scrolled,
  .button_pressed = _path_events_button_pressed,
  .button_released = _path_events_button_released,
  .post_expose = _path_events_post_expose
};


// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on

