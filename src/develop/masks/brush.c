/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson.
    copyright (c) 2012 aldric renaudin.
    copyright (c) 2013 Ulrich Pegelow.

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
#include "common/debug.h"
#include "control/conf.h"
#include "control/control.h"
#include "develop/blend.h"
#include "develop/imageop.h"
#include "develop/masks.h"

/** get squared distance of indexed point to line segment, taking weighted payload data into account */
static float _brush_point_line_distance2(int index, int pointscount, const float *points, const float *payload)
{
  const float x = points[2 * index];
  const float y = points[2 * index + 1];
  const float b = payload[4 * index];
  const float h = payload[4 * index + 1];
  const float d = payload[4 * index + 2];
  const float xstart = points[0];
  const float ystart = points[1];
  const float bstart = payload[0];
  const float hstart = payload[1];
  const float dstart = payload[2];
  const float xend = points[2 * (pointscount - 1)];
  const float yend = points[2 * (pointscount - 1) + 1];
  const float bend = payload[4 * (pointscount - 1)];
  const float hend = payload[4 * (pointscount - 1) + 1];
  const float dend = payload[4 * (pointscount - 1) + 2];
  const float bweight = 1.0f;
  const float hweight = 0.01f;
  const float dweight = 0.01f;

  const float r1 = x - xstart;
  const float r2 = y - ystart;
  const float r3 = xend - xstart;
  const float r4 = yend - ystart;
  const float r5 = bend - bstart;
  const float r6 = hend - hstart;
  const float r7 = dend - dstart;

  const float r = r1 * r3 + r2 * r4;
  const float l = r3 * r3 + r4 * r4;
  const float p = r / l;

  float dx, dy, db, dh, dd;

  if(l == 0.0f)
  {
    dx = x - xstart;
    dy = y - ystart;
    db = b - bstart;
    dh = h - hstart;
    dd = d - dstart;
  }
  else if(p < 0.0f)
  {
    dx = x - xstart;
    dy = y - ystart;
    db = b - bstart;
    dh = h - hstart;
    dd = d - dstart;
  }
  else if(p > 1.0f)
  {
    dx = x - xend;
    dy = y - yend;
    db = b - bend;
    dh = h - hend;
    dd = d - dend;
  }
  else
  {
    dx = x - (xstart + p * r3);
    dy = y - (ystart + p * r4);
    db = b - (bstart + p * r5);
    dh = h - (hstart + p * r6);
    dd = d - (dstart + p * r7);
  }

  return dx * dx + dy * dy + bweight * db * db + hweight * dh * dh + dweight * dd * dd;
}

/** remove unneeded points (Ramer-Douglas-Peucker algorithm) and return resulting path as linked list */
static GList *_brush_ramer_douglas_peucker(const float *points, int points_count, const float *payload,
                                           float epsilon2)
{
  GList *ResultList = NULL;

  float dmax2 = 0.0f;
  int index = 0;

  for(int i = 1; i < points_count - 1; i++)
  {
    float d2 = _brush_point_line_distance2(i, points_count, points, payload);
    if(d2 > dmax2)
    {
      index = i;
      dmax2 = d2;
    }
  }

  if(dmax2 >= epsilon2)
  {
    GList *ResultList1 = _brush_ramer_douglas_peucker(points, index + 1, payload, epsilon2);
    GList *ResultList2 = _brush_ramer_douglas_peucker(points + index * 2, points_count - index,
                                                      payload + index * 4, epsilon2);

    // remove last element from ResultList1
    GList *end1 = g_list_last(ResultList1);
    free(end1->data);
    ResultList1 = g_list_delete_link(ResultList1, end1);

    ResultList = g_list_concat(ResultList1, ResultList2);
  }
  else
  {
    dt_masks_point_brush_t *point1 = malloc(sizeof(dt_masks_point_brush_t));
    point1->corner[0] = points[0];
    point1->corner[1] = points[1];
    point1->ctrl1[0] = point1->ctrl1[1] = point1->ctrl2[0] = point1->ctrl2[1] = -1.0f;
    point1->border[0] = point1->border[1] = payload[0];
    point1->hardness = payload[1];
    point1->density = payload[2];
    point1->state = DT_MASKS_POINT_STATE_NORMAL;
    ResultList = g_list_append(ResultList, (gpointer)point1);

    dt_masks_point_brush_t *pointn = malloc(sizeof(dt_masks_point_brush_t));
    pointn->corner[0] = points[(points_count - 1) * 2];
    pointn->corner[1] = points[(points_count - 1) * 2 + 1];
    pointn->ctrl1[0] = pointn->ctrl1[1] = pointn->ctrl2[0] = pointn->ctrl2[1] = -1.0f;
    pointn->border[0] = pointn->border[1] = payload[(points_count - 1) * 4];
    pointn->hardness = payload[(points_count - 1) * 4 + 1];
    pointn->density = payload[(points_count - 1) * 4 + 2];
    pointn->state = DT_MASKS_POINT_STATE_NORMAL;
    ResultList = g_list_append(ResultList, (gpointer)pointn);
  }

  return ResultList;
}

/** get the point of the brush at pos t [0,1]  */
static void _brush_get_XY(float p0x, float p0y, float p1x, float p1y, float p2x, float p2y, float p3x,
                          float p3y, float t, float *x, float *y)
{
  float a = (1 - t) * (1 - t) * (1 - t);
  float b = 3 * t * (1 - t) * (1 - t);
  float c = 3 * t * t * (1 - t);
  float d = t * t * t;
  *x = p0x * a + p1x * b + p2x * c + p3x * d;
  *y = p0y * a + p1y * b + p2y * c + p3y * d;
}

/** get the point of the brush at pos t [0,1]  AND the corresponding border point */
static void _brush_border_get_XY(float p0x, float p0y, float p1x, float p1y, float p2x, float p2y, float p3x,
                                 float p3y, float t, float rad, float *xc, float *yc, float *xb, float *yb)
{
  // we get the point
  _brush_get_XY(p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y, t, xc, yc);

  // now we get derivative points
  float a = 3 * (1 - t) * (1 - t);
  float b = 3 * ((1 - t) * (1 - t) - 2 * t * (1 - t));
  float c = 3 * (2 * t * (1 - t) - t * t);
  float d = 3 * t * t;

  float dx = -p0x * a + p1x * b + p2x * c + p3x * d;
  float dy = -p0y * a + p1y * b + p2y * c + p3y * d;

  // so we can have the resulting point
  if(dx == 0 && dy == 0)
  {
    *xb = NAN;
    *yb = NAN;
    return;
  }
  float l = 1.0 / sqrtf(dx * dx + dy * dy);
  *xb = (*xc) + rad * dy * l;
  *yb = (*yc) - rad * dx * l;
}

/** get feather extremity from the control point n°2 */
/** the values should be in orthonormal space */
static void _brush_ctrl2_to_feather(int ptx, int pty, int ctrlx, int ctrly, int *fx, int *fy,
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
static void _brush_feather_to_ctrl(int ptx, int pty, int fx, int fy, int *ctrl1x, int *ctrl1y, int *ctrl2x,
                                   int *ctrl2y, gboolean clockwise)
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
static void _brush_catmull_to_bezier(float x1, float y1, float x2, float y2, float x3, float y3, float x4,
                                     float y4, float *bx1, float *by1, float *bx2, float *by2)
{
  *bx1 = (-x1 + 6 * x2 + x3) / 6;
  *by1 = (-y1 + 6 * y2 + y3) / 6;
  *bx2 = (x2 + 6 * x3 - x4) / 6;
  *by2 = (y2 + 6 * y3 - y4) / 6;
}

/** initialise all control points to eventually match a catmull-rom like spline */
static void _brush_init_ctrl_points(dt_masks_form_t *form)
{
  // if we have less that 2 points, what to do ??
  if(g_list_length(form->points) < 2) return;

  // we need extra points to deal with curve ends
  dt_masks_point_brush_t start_point[2], end_point[2];

  guint nb = g_list_length(form->points);
  for(int k = 0; k < nb; k++)
  {
    dt_masks_point_brush_t *point3 = (dt_masks_point_brush_t *)g_list_nth_data(form->points, k);
    // if the point as not be set manually, we redfine it
    if(point3->state & DT_MASKS_POINT_STATE_NORMAL)
    {
      // we want to get point-2, point-1, point+1, point+2
      dt_masks_point_brush_t *point1
          = k - 2 >= 0 ? (dt_masks_point_brush_t *)g_list_nth_data(form->points, k - 2) : NULL;
      dt_masks_point_brush_t *point2
          = k - 1 >= 0 ? (dt_masks_point_brush_t *)g_list_nth_data(form->points, k - 1) : NULL;
      dt_masks_point_brush_t *point4
          = k + 1 < nb ? (dt_masks_point_brush_t *)g_list_nth_data(form->points, k + 1) : NULL;
      dt_masks_point_brush_t *point5
          = k + 2 < nb ? (dt_masks_point_brush_t *)g_list_nth_data(form->points, k + 2) : NULL;

      // deal with end points: make both extending points mirror their neighborhood
      if(point1 == NULL && point2 == NULL)
      {
        start_point[0].corner[0] = start_point[1].corner[0] = 2 * point3->corner[0] - point4->corner[0];
        start_point[0].corner[1] = start_point[1].corner[1] = 2 * point3->corner[1] - point4->corner[1];
        point1 = &(start_point[0]);
        point2 = &(start_point[1]);
      }
      else if(point1 == NULL)
      {
        start_point[0].corner[0] = 2 * point2->corner[0] - point3->corner[0];
        start_point[0].corner[1] = 2 * point2->corner[1] - point3->corner[1];
        point1 = &(start_point[0]);
      }

      if(point4 == NULL && point5 == NULL)
      {
        end_point[0].corner[0] = end_point[1].corner[0] = 2 * point3->corner[0] - point2->corner[0];
        end_point[0].corner[1] = end_point[1].corner[1] = 2 * point3->corner[1] - point2->corner[1];
        point4 = &(end_point[0]);
        point5 = &(end_point[1]);
      }
      else if(point5 == NULL)
      {
        end_point[0].corner[0] = 2 * point4->corner[0] - point3->corner[0];
        end_point[0].corner[1] = 2 * point4->corner[1] - point3->corner[1];
        point5 = &(end_point[0]);
      }


      float bx1, by1, bx2, by2;
      _brush_catmull_to_bezier(point1->corner[0], point1->corner[1], point2->corner[0], point2->corner[1],
                               point3->corner[0], point3->corner[1], point4->corner[0], point4->corner[1],
                               &bx1, &by1, &bx2, &by2);
      if(point2->ctrl2[0] == -1.0) point2->ctrl2[0] = bx1;
      if(point2->ctrl2[1] == -1.0) point2->ctrl2[1] = by1;
      point3->ctrl1[0] = bx2;
      point3->ctrl1[1] = by2;
      _brush_catmull_to_bezier(point2->corner[0], point2->corner[1], point3->corner[0], point3->corner[1],
                               point4->corner[0], point4->corner[1], point5->corner[0], point5->corner[1],
                               &bx1, &by1, &bx2, &by2);
      if(point4->ctrl1[0] == -1.0) point4->ctrl1[0] = bx2;
      if(point4->ctrl1[1] == -1.0) point4->ctrl1[1] = by2;
      point3->ctrl2[0] = bx1;
      point3->ctrl2[1] = by1;
    }
  }
}


/** fill the gap between 2 points with an arc of circle */
/** this function is here because we can have gap in border, esp. if the corner is very sharp */
static void _brush_points_recurs_border_gaps(float *cmax, float *bmin, float *bmin2, float *bmax,
                                             dt_masks_dynbuf_t *dpoints, dt_masks_dynbuf_t *dborder,
                                             gboolean clockwise)
{
  // we want to find the start and end angles
  float a1 = atan2(bmin[1] - cmax[1], bmin[0] - cmax[0]);
  float a2 = atan2(bmax[1] - cmax[1], bmax[0] - cmax[0]);

  if(a1 == a2) return;

  // we have to be sure that we turn in the correct direction
  if(a2 < a1 && clockwise)
  {
    a2 += 2.0f * M_PI;
  }
  if(a2 > a1 && !clockwise)
  {
    a1 += 2.0f * M_PI;
  }

  // we determine start and end radius too
  float r1 = sqrtf((bmin[1] - cmax[1]) * (bmin[1] - cmax[1]) + (bmin[0] - cmax[0]) * (bmin[0] - cmax[0]));
  float r2 = sqrtf((bmax[1] - cmax[1]) * (bmax[1] - cmax[1]) + (bmax[0] - cmax[0]) * (bmax[0] - cmax[0]));

  // and the max length of the circle arc
  int l;
  if(a2 > a1)
    l = (a2 - a1) * fmaxf(r1, r2);
  else
    l = (a1 - a2) * fmaxf(r1, r2);
  if(l < 2) return;

  // and now we add the points
  float incra = (a2 - a1) / l;
  float incrr = (r2 - r1) / l;
  float rr = r1 + incrr;
  float aa = a1 + incra;
  for(int i = 1; i < l; i++)
  {
    dt_masks_dynbuf_add(dpoints, cmax[0]);
    dt_masks_dynbuf_add(dpoints, cmax[1]);
    dt_masks_dynbuf_add(dborder, cmax[0] + rr * cosf(aa));
    dt_masks_dynbuf_add(dborder, cmax[1] + rr * sinf(aa));
    rr += incrr;
    aa += incra;
  }
}

/** fill small gap between 2 points with an arc of circle */
/** in contrast to the previous function it will always run the shortest path (max. PI) and does not consider
 * clock or anti-clockwise action */
static void _brush_points_recurs_border_small_gaps(float *cmax, float *bmin, float *bmin2, float *bmax,
                                                   dt_masks_dynbuf_t *dpoints, dt_masks_dynbuf_t *dborder)
{
  // we want to find the start and end angles
  float a1 = fmodf(atan2(bmin[1] - cmax[1], bmin[0] - cmax[0]) + 2.0f * M_PI, 2.0f * M_PI);
  float a2 = fmodf(atan2(bmax[1] - cmax[1], bmax[0] - cmax[0]) + 2.0f * M_PI, 2.0f * M_PI);

  if(a1 == a2) return;

  // we determine start and end radius too
  float r1 = sqrtf((bmin[1] - cmax[1]) * (bmin[1] - cmax[1]) + (bmin[0] - cmax[0]) * (bmin[0] - cmax[0]));
  float r2 = sqrtf((bmax[1] - cmax[1]) * (bmax[1] - cmax[1]) + (bmax[0] - cmax[0]) * (bmax[0] - cmax[0]));

  // we close the gap in the shortest direction
  float delta = a2 - a1;
  if(fabsf(delta) > M_PI) delta = delta - copysignf(2.0f * M_PI, delta);

  // get the max length of the circle arc
  int l = fabsf(delta) * fmaxf(r1, r2);
  if(l < 2) return;

  // and now we add the points
  float incra = delta / l;
  float incrr = (r2 - r1) / l;
  float rr = r1 + incrr;
  float aa = a1 + incra;
  for(int i = 1; i < l; i++)
  {
    dt_masks_dynbuf_add(dpoints, cmax[0]);
    dt_masks_dynbuf_add(dpoints, cmax[1]);
    dt_masks_dynbuf_add(dborder, cmax[0] + rr * cosf(aa));
    dt_masks_dynbuf_add(dborder, cmax[1] + rr * sinf(aa));
    rr += incrr;
    aa += incra;
  }
}


/** draw a circle with given radius. can be used to terminate a stroke and to draw junctions where attributes
 * (opacity) change */
static void _brush_points_stamp(float *cmax, float *bmin, dt_masks_dynbuf_t *dpoints,  dt_masks_dynbuf_t *dborder,
                                gboolean clockwise)
{
  // we want to find the start angle
  float a1 = atan2(bmin[1] - cmax[1], bmin[0] - cmax[0]);

  // we determine the radius too
  float rad = sqrtf((bmin[1] - cmax[1]) * (bmin[1] - cmax[1]) + (bmin[0] - cmax[0]) * (bmin[0] - cmax[0]));

  // determine the max length of the circle arc
  int l = 2.0f * M_PI * rad;
  if(l < 2) return;

  // and now we add the points
  float incra = 2.0f * M_PI / l;
  float aa = a1 + incra;
  for(int i = 0; i < l; i++)
  {
    dt_masks_dynbuf_add(dpoints, cmax[0]);
    dt_masks_dynbuf_add(dpoints, cmax[1]);
    dt_masks_dynbuf_add(dborder, cmax[0] + rad * cosf(aa));
    dt_masks_dynbuf_add(dborder, cmax[1] + rad * sinf(aa));
    aa += incra;
  }
}

/** recursive function to get all points of the brush AND all point of the border */
/** the function takes care to avoid big gaps between points */
static void _brush_points_recurs(float *p1, float *p2, double tmin, double tmax, float *points_min,
                                 float *points_max, float *border_min, float *border_max, float *rpoints,
                                 float *rborder, float *rpayload, dt_masks_dynbuf_t *dpoints, dt_masks_dynbuf_t *dborder,
                                 dt_masks_dynbuf_t *dpayload)
{
  const gboolean withborder = (dborder != NULL);
  const gboolean withpayload = (dpayload != NULL);

  // we calculate points if needed
  if(isnan(points_min[0]))
  {
    _brush_border_get_XY(p1[0], p1[1], p1[2], p1[3], p2[2], p2[3], p2[0], p2[1], tmin,
                         p1[4] + (p2[4] - p1[4]) * tmin * tmin * (3.0 - 2.0 * tmin), points_min,
                         points_min + 1, border_min, border_min + 1);
  }
  if(isnan(points_max[0]))
  {
    _brush_border_get_XY(p1[0], p1[1], p1[2], p1[3], p2[2], p2[3], p2[0], p2[1], tmax,
                         p1[4] + (p2[4] - p1[4]) * tmax * tmax * (3.0 - 2.0 * tmax), points_max,
                         points_max + 1, border_max, border_max + 1);
  }
  // are the points near ?
  if((tmax - tmin < 0.0001f)
     || ((int)points_min[0] - (int)points_max[0] < 1 && (int)points_min[0] - (int)points_max[0] > -1
         && (int)points_min[1] - (int)points_max[1] < 1 && (int)points_min[1] - (int)points_max[1] > -1
         && (!withborder
             || ((int)border_min[0] - (int)border_max[0] < 1 && (int)border_min[0] - (int)border_max[0] > -1
                 && (int)border_min[1] - (int)border_max[1] < 1
                 && (int)border_min[1] - (int)border_max[1] > -1))))
  {
    rpoints[0] = points_max[0];
    rpoints[1] = points_max[1];
    dt_masks_dynbuf_add(dpoints, rpoints[0]);
    dt_masks_dynbuf_add(dpoints, rpoints[1]);

    if(withborder)
    {
      if(isnan(border_max[0]))
      {
        border_max[0] = border_min[0];
        border_max[1] = border_min[1];
      }
      else if(isnan(border_min[0]))
      {
        border_min[0] = border_max[0];
        border_min[1] = border_max[1];
      }

      // we check gaps in the border (sharp edges)
      if(abs((int)border_max[0] - (int)border_min[0]) > 2 || abs((int)border_max[1] - (int)border_min[1]) > 2)
      {
        _brush_points_recurs_border_small_gaps(points_max, border_min, NULL, border_max, dpoints, dborder);
      }

      rborder[0] = border_max[0];
      rborder[1] = border_max[1];
      dt_masks_dynbuf_add(dborder, rborder[0]);
      dt_masks_dynbuf_add(dborder, rborder[1]);
    }

    if(withpayload)
    {
      while(dt_masks_dynbuf_position(dpayload) < dt_masks_dynbuf_position(dpoints))
      {
        rpayload[0] = p1[5] + tmax * (p2[5] - p1[5]);
        rpayload[1] = p1[6] + tmax * (p2[6] - p1[6]);
        dt_masks_dynbuf_add(dpayload, rpayload[0]);
        dt_masks_dynbuf_add(dpayload, rpayload[1]);
      }
    }

    return;
  }

  // we split in two part
  double tx = (tmin + tmax) / 2.0;
  float c[2] = { NAN, NAN }, b[2] = { NAN, NAN };
  float rc[2], rb[2], rp[2];
  _brush_points_recurs(p1, p2, tmin, tx, points_min, c, border_min, b, rc, rb, rp, dpoints, dborder, dpayload);
  _brush_points_recurs(p1, p2, tx, tmax, rc, points_max, rb, border_max, rpoints, rborder, rpayload, dpoints,
                       dborder, dpayload);
}


/** converts n into a cyclical sequence counting upwards from 0 to nb-1 and back down again, counting
 * endpoints twice */
static inline int _brush_cyclic_cursor(int n, int nb)
{
  const int o = n % (2 * nb);
  const int p = o % nb;

  return (o <= p) ? o : o - 2 * p - 1;
}


/** get all points of the brush and the border */
/** this takes care of gaps and iop distortions */
static int _brush_get_points_border(dt_develop_t *dev, dt_masks_form_t *form, int prio_max,
                                    dt_dev_pixelpipe_t *pipe, float **points, int *points_count,
                                    float **border, int *border_count, float **payload, int *payload_count,
                                    int source)
{
  double start2 = dt_get_wtime();

  float wd = pipe->iwidth, ht = pipe->iheight;

  *points = NULL;
  *points_count = 0;
  if(border) *border = NULL;
  if(border) *border_count = 0;
  if(payload) *payload = NULL;
  if(payload) *payload_count = 0;

  dt_masks_dynbuf_t *dpoints = NULL, *dborder = NULL, *dpayload = NULL;

  dpoints = dt_masks_dynbuf_init(1000000, "brush dpoints");
  if(dpoints == NULL) return 0;

  if(border)
  {
    dborder = dt_masks_dynbuf_init(1000000, "brush dborder");
    if(dborder == NULL)
    {
      dt_masks_dynbuf_free(dpoints);
      return 0;
    }
  }

  if(payload)
  {
    dpayload = dt_masks_dynbuf_init(1000000, "brush dpayload");
    if(dpayload == NULL)
    {
      dt_masks_dynbuf_free(dpoints);
      dt_masks_dynbuf_free(dborder);
      return 0;
    }
  }

  // we store all points
  float dx = 0.0f, dy = 0.0f;

  guint nb = g_list_length(form->points);

  if(source && nb > 0)
  {
    dt_masks_point_brush_t *pt = (dt_masks_point_brush_t *)g_list_nth_data(form->points, 0);
    dx = (pt->corner[0] - form->source[0]) * wd;
    dy = (pt->corner[1] - form->source[1]) * ht;
  }

  for(int k = 0; k < nb; k++)
  {
    dt_masks_point_brush_t *pt = (dt_masks_point_brush_t *)g_list_nth_data(form->points, k);
    dt_masks_dynbuf_add(dpoints, pt->ctrl1[0] * wd - dx);
    dt_masks_dynbuf_add(dpoints, pt->ctrl1[1] * ht - dy);
    dt_masks_dynbuf_add(dpoints, pt->corner[0] * wd - dx);
    dt_masks_dynbuf_add(dpoints, pt->corner[1] * ht - dy);
    dt_masks_dynbuf_add(dpoints, pt->ctrl2[0] * wd - dx);
    dt_masks_dynbuf_add(dpoints, pt->ctrl2[1] * ht - dy);
  }

  // for the border, we store value too
  if(dborder)
  {
    for(int k = 0; k < nb; k++)
    {
      dt_masks_dynbuf_add(dborder, 0.0f);
      dt_masks_dynbuf_add(dborder, 0.0f);
      dt_masks_dynbuf_add(dborder, 0.0f);
      dt_masks_dynbuf_add(dborder, 0.0f);
      dt_masks_dynbuf_add(dborder, 0.0f);
      dt_masks_dynbuf_add(dborder, 0.0f);
    }
  }

  // for the payload, we reserve an equivalent number of cells to keep it in sync
  if(dpayload)
  {
    for(int k = 0; k < nb; k++)
    {
      dt_masks_dynbuf_add(dpayload, 0.0f);
      dt_masks_dynbuf_add(dpayload, 0.0f);
      dt_masks_dynbuf_add(dpayload, 0.0f);
      dt_masks_dynbuf_add(dpayload, 0.0f);
      dt_masks_dynbuf_add(dpayload, 0.0f);
      dt_masks_dynbuf_add(dpayload, 0.0f);
    }
  }

  int cw = 1;
  int start_stamp = 0;

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush_points init took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we render all segments first upwards, then downwards
  for(int n = 0; n < 2 * nb; n++)
  {
    float p1[7], p2[7], p3[7], p4[7];
    int k = _brush_cyclic_cursor(n, nb);
    int k1 = _brush_cyclic_cursor(n + 1, nb);
    int k2 = _brush_cyclic_cursor(n + 2, nb);

    dt_masks_point_brush_t *point1 = (dt_masks_point_brush_t *)g_list_nth_data(form->points, k);
    dt_masks_point_brush_t *point2 = (dt_masks_point_brush_t *)g_list_nth_data(form->points, k1);
    dt_masks_point_brush_t *point3 = (dt_masks_point_brush_t *)g_list_nth_data(form->points, k2);
    if(cw > 0)
    {
      float pa[7] = { point1->corner[0] * wd - dx, point1->corner[1] * ht - dy, point1->ctrl2[0] * wd - dx,
                      point1->ctrl2[1] * ht - dy, point1->border[1] * MIN(wd, ht), point1->hardness,
                      point1->density };
      float pb[7] = { point2->corner[0] * wd - dx, point2->corner[1] * ht - dy, point2->ctrl1[0] * wd - dx,
                      point2->ctrl1[1] * ht - dy, point2->border[0] * MIN(wd, ht), point2->hardness,
                      point2->density };
      float pc[7] = { point2->corner[0] * wd - dx, point2->corner[1] * ht - dy, point2->ctrl2[0] * wd - dx,
                      point2->ctrl2[1] * ht - dy, point2->border[1] * MIN(wd, ht), point2->hardness,
                      point2->density };
      float pd[7] = { point3->corner[0] * wd - dx, point3->corner[1] * ht - dy, point3->ctrl1[0] * wd - dx,
                      point3->ctrl1[1] * ht - dy, point3->border[0] * MIN(wd, ht), point3->hardness,
                      point3->density };
      memcpy(p1, pa, 7 * sizeof(float));
      memcpy(p2, pb, 7 * sizeof(float));
      memcpy(p3, pc, 7 * sizeof(float));
      memcpy(p4, pd, 7 * sizeof(float));
    }
    else
    {
      float pa[7] = { point1->corner[0] * wd - dx, point1->corner[1] * ht - dy, point1->ctrl1[0] * wd - dx,
                      point1->ctrl1[1] * ht - dy, point1->border[1] * MIN(wd, ht), point1->hardness,
                      point1->density };
      float pb[7] = { point2->corner[0] * wd - dx, point2->corner[1] * ht - dy, point2->ctrl2[0] * wd - dx,
                      point2->ctrl2[1] * ht - dy, point2->border[0] * MIN(wd, ht), point2->hardness,
                      point2->density };
      float pc[7] = { point2->corner[0] * wd - dx, point2->corner[1] * ht - dy, point2->ctrl1[0] * wd - dx,
                      point2->ctrl1[1] * ht - dy, point2->border[1] * MIN(wd, ht), point2->hardness,
                      point2->density };
      float pd[7] = { point3->corner[0] * wd - dx, point3->corner[1] * ht - dy, point3->ctrl2[0] * wd - dx,
                      point3->ctrl2[1] * ht - dy, point3->border[0] * MIN(wd, ht), point3->hardness,
                      point3->density };
      memcpy(p1, pa, 7 * sizeof(float));
      memcpy(p2, pb, 7 * sizeof(float));
      memcpy(p3, pc, 7 * sizeof(float));
      memcpy(p4, pd, 7 * sizeof(float));
    }

    // 1st. special case: render abrupt transitions between different opacity and/or hardness values
    if((fabs(p1[5] - p2[5]) > 0.05f || fabs(p1[6] - p2[6]) > 0.05f) || (start_stamp && n == 2 * nb - 1))
    {
      if(n == 0)
      {
        start_stamp = 1; // remember to deal with the first node as a final step
      }
      else
      {
        if(dborder)
        {
          float bmin[2] = { dt_masks_dynbuf_get(dborder, -2), dt_masks_dynbuf_get(dborder, -1) };
          float cmax[2] = { dt_masks_dynbuf_get(dpoints, -2), dt_masks_dynbuf_get(dpoints, -1) };
          _brush_points_stamp(cmax, bmin, dpoints, dborder, TRUE);
        }

        if(dpayload)
        {
          while(dt_masks_dynbuf_position(dpayload) < dt_masks_dynbuf_position(dpoints))
          {
            dt_masks_dynbuf_add(dpayload, p1[5]);
            dt_masks_dynbuf_add(dpayload, p1[6]);
          }
        }
      }
    }

    // 2nd. special case: render transition point between different brush sizes
    if(fabs(p1[4] - p2[4]) > 0.0001f && n > 0)
    {
      if(dborder)
      {
        float bmin[2] = { dt_masks_dynbuf_get(dborder, -2), dt_masks_dynbuf_get(dborder, -1) };
        float cmax[2] = { dt_masks_dynbuf_get(dpoints, -2), dt_masks_dynbuf_get(dpoints, -1) };
        float bmax[2] = { 2 * cmax[0] - bmin[0], 2 * cmax[1] - bmin[1] };
        _brush_points_recurs_border_gaps(cmax, bmin, NULL, bmax, dpoints, dborder, TRUE);
      }

      if(dpayload)
      {
        while(dt_masks_dynbuf_position(dpayload) < dt_masks_dynbuf_position(dpoints))
        {
          dt_masks_dynbuf_add(dpayload, p1[5]);
          dt_masks_dynbuf_add(dpayload, p1[6]);
        }
      }
    }

    // 3rd. special case: render endpoints
    if(k == k1)
    {
      if(dborder)
      {
        float bmin[2] = { dt_masks_dynbuf_get(dborder, -2), dt_masks_dynbuf_get(dborder, -1) };
        float cmax[2] = { dt_masks_dynbuf_get(dpoints, -2), dt_masks_dynbuf_get(dpoints, -1) };
        float bmax[2] = { 2 * cmax[0] - bmin[0], 2 * cmax[1] - bmin[1] };
        _brush_points_recurs_border_gaps(cmax, bmin, NULL, bmax, dpoints, dborder, TRUE);
      }

      if(dpayload)
      {
        while(dt_masks_dynbuf_position(dpayload) < dt_masks_dynbuf_position(dpoints))
        {
          dt_masks_dynbuf_add(dpayload, p1[5]);
          dt_masks_dynbuf_add(dpayload, p1[6]);
        }
      }

      cw *= -1;
      continue;
    }

    // and we determine all points by recursion (to be sure the distance between 2 points is <=1)
    float rc[2], rb[2], rp[2];
    float bmin[2] = { NAN, NAN };
    float bmax[2] = { NAN, NAN };
    float cmin[2] = { NAN, NAN };
    float cmax[2] = { NAN, NAN };

    _brush_points_recurs(p1, p2, 0.0, 1.0, cmin, cmax, bmin, bmax, rc, rb, rp, dpoints, dborder, dpayload);

    dt_masks_dynbuf_add(dpoints, rc[0]);
    dt_masks_dynbuf_add(dpoints, rc[1]);

    if(dpayload)
    {
      dt_masks_dynbuf_add(dpayload, rp[0]);
      dt_masks_dynbuf_add(dpayload, rp[1]);
    }

    if(dborder)
    {
      if(isnan(rb[0]))
      {
        if(isnan(dt_masks_dynbuf_get(dborder, -2)))
        {
          dt_masks_dynbuf_set(dborder, -2, dt_masks_dynbuf_get(dborder, -4));
          dt_masks_dynbuf_set(dborder, -1, dt_masks_dynbuf_get(dborder, -3));
        }
        rb[0] = dt_masks_dynbuf_get(dborder, -2);
        rb[1] = dt_masks_dynbuf_get(dborder, -1);
      }
      dt_masks_dynbuf_add(dborder, rb[0]);
      dt_masks_dynbuf_add(dborder, rb[1]);
    }

    // we first want to be sure that there are no gaps in border
    if(dborder && nb >= 3)
    {
      // we get the next point (start of the next segment)
      _brush_border_get_XY(p3[0], p3[1], p3[2], p3[3], p4[2], p4[3], p4[0], p4[1], 0, p3[4], cmin, cmin + 1,
                           bmax, bmax + 1);
      if(isnan(bmax[0]))
      {
        _brush_border_get_XY(p3[0], p3[1], p3[2], p3[3], p4[2], p4[3], p4[0], p4[1], 0.0001, p3[4], cmin,
                             cmin + 1, bmax, bmax + 1);
      }
      if(bmax[0] - rb[0] > 1 || bmax[0] - rb[0] < -1 || bmax[1] - rb[1] > 1 || bmax[1] - rb[1] < -1)
      {
        // float bmin2[2] = {(*border)[posb-22],(*border)[posb-21]};
        _brush_points_recurs_border_gaps(rc, rb, NULL, bmax, dpoints, dborder, cw);
      }
    }

    if(dpayload)
    {
      while(dt_masks_dynbuf_position(dpayload) < dt_masks_dynbuf_position(dpoints))
      {
        dt_masks_dynbuf_add(dpayload, rp[0]);
        dt_masks_dynbuf_add(dpayload, rp[1]);
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

  if(dpayload)
  {
    *payload_count = dt_masks_dynbuf_position(dpayload) / 2;
    *payload = dt_masks_dynbuf_harvest(dpayload);
    dt_masks_dynbuf_free(dpayload);
  }
  // printf("points %d, border %d, playload %d\n", *points_count, border ? *border_count : -1, payload ?
  // *payload_count : -1);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush_points point recurs %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // and we transform them with all distorted modules
  if(dt_dev_distort_transform_plus(dev, pipe, 0, prio_max, *points, *points_count))
  {
    if(!border || dt_dev_distort_transform_plus(dev, pipe, 0, prio_max, *border, *border_count))
    {
      if(darktable.unmuted & DT_DEBUG_PERF)
        dt_print(DT_DEBUG_MASKS, "[masks %s] brush_points transform took %0.04f sec\n", form->name,
                 dt_get_wtime() - start2);
//       start2 = dt_get_wtime();
      return 1;
    }
  }

  // if we failed, then free all and return
  free(*points);
  *points = NULL;
  *points_count = 0;
  if(border) free(*border);
  if(border) *border = NULL;
  if(border) *border_count = 0;
  if(payload) free(*payload);
  if(payload) *payload = NULL;
  if(payload) *payload_count = 0;
  return 0;
}

/** get the distance between point (x,y) and the brush */
static void dt_brush_get_distance(float x, int y, float as, dt_masks_form_gui_t *gui, int index,
                                  int corner_count, int *inside, int *inside_border, int *near,
                                  int *inside_source)
{
  // initialise returned values
  *inside_source = 0;
  *inside = 0;
  *inside_border = 0;
  *near = -1;

  if(!gui) return;

  float yf = (float)y;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;

  // we first check if we are inside the source form

  // add support for clone masks
  if(gpt->points_count > 2 + corner_count * 3 && gpt->source_count > 2 + corner_count * 3)
  {
    float dx = -gpt->points[2] + gpt->source[2];
    float dy = -gpt->points[3] + gpt->source[3];

    int current_seg = 1;
    for(int i = corner_count * 3; i < gpt->points_count; i++)
    {
      // do we change of path segment ?
      if(gpt->points[i * 2 + 1] == gpt->points[current_seg * 6 + 3]
         && gpt->points[i * 2] == gpt->points[current_seg * 6 + 2])
      {
        current_seg = (current_seg + 1) % corner_count;
      }
      // distance from tested point to current form point
      const float yy = gpt->points[i * 2 + 1] + dy;
      const float xx = gpt->points[i * 2] + dx;
      if((yy - yf) < as && (yy - yf) > -as && (xx - x) < as && (xx - x) > -as)
      {
        if(current_seg == 0)
          *inside_source = corner_count - 1;
        else
          *inside_source = current_seg - 1;

        if(*inside_source)
        {
          *inside = 1;
          return;
        }
      }
    }
  }

  // we check if it's inside borders
  if(gpt->border_count > 2 + corner_count * 3)
  {
    float last = gpt->border[gpt->border_count * 2 - 1];
    int nb = 0;
    for(int i = corner_count * 3; i < gpt->border_count; i++)
    {
      float yy = gpt->border[i * 2 + 1];
      if (((yf<=yy && yf>last) || (yf>=yy && yf<last)) && (gpt->border[i * 2] > x)) nb++;
      last = yy;
    }
    *inside = *inside_border = (nb & 1);
  }

  // and we check if we are near a segment
  if(gpt->points_count > 2 + corner_count * 3)
  {
    int current_seg = 1;
    for(int i = corner_count * 3; i < gpt->points_count; i++)
    {
      // do we change of path segment ?
      if(gpt->points[i * 2 + 1] == gpt->points[current_seg * 6 + 3] && gpt->points[i * 2] == gpt->points[current_seg * 6 + 2])
      {
        current_seg = (current_seg + 1) % corner_count;
      }
      //distance from tested point to current form point
      float yy = gpt->points[i * 2 + 1];
      float xx = gpt->points[i * 2];
      if ((yy-yf)<as && (yy-yf)>-as && (xx-x)<as && (xx-x)>-as)
      {
        if(current_seg == 0)
          *near = corner_count - 1;
        else
          *near = current_seg - 1;

        return;
      }
    }
  }
}

static int dt_brush_get_points_border(dt_develop_t *dev, dt_masks_form_t *form, float **points,
                                      int *points_count, float **border, int *border_count, int source)
{
  return _brush_get_points_border(dev, form, 999999, dev->preview_pipe, points, points_count, border,
                                  border_count, NULL, NULL, source);
}

/** find relative position within a brush segment that is closest to the point given by coordinates x and y;
    we only need to find the minimum with a resolution of 1%, so we just do an exhaustive search without any
   frills */
static float _brush_get_position_in_segment(float x, float y, dt_masks_form_t *form, int segment)
{
  guint nb = g_list_length(form->points);
  int pos0 = segment;
  int pos1 = segment + 1;
  int pos2 = segment + 2;
  int pos3 = segment + 3;

  dt_masks_point_brush_t *point0 = (dt_masks_point_brush_t *)g_list_nth_data(form->points, pos0);
  dt_masks_point_brush_t *point1 = pos1 < nb ? (dt_masks_point_brush_t *)g_list_nth_data(form->points, pos1)
                                             : point0;
  dt_masks_point_brush_t *point2 = pos2 < nb ? (dt_masks_point_brush_t *)g_list_nth_data(form->points, pos2)
                                             : point1;
  dt_masks_point_brush_t *point3 = pos3 < nb ? (dt_masks_point_brush_t *)g_list_nth_data(form->points, pos3)
                                             : point2;

  float tmin = 0;
  float dmin = FLT_MAX;

  for(int i = 0; i <= 100; i++)
  {
    float t = i / 100.0f;
    float sx, sy;
    _brush_get_XY(point0->corner[0], point0->corner[1], point1->corner[0], point1->corner[1],
                  point2->corner[0], point2->corner[1], point3->corner[0], point3->corner[1], t, &sx, &sy);

    float d = (x - sx) * (x - sx) + (y - sy) * (y - sy);
    if(d < dmin)
    {
      dmin = d;
      tmin = t;
    }
  }

  return tmin;
}

static int dt_brush_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up,
                                          uint32_t state, dt_masks_form_t *form, int parentid,
                                          dt_masks_form_gui_t *gui, int index)
{
  if(gui->creation)
  {
    if((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
    {
      float masks_hardness;
      float amount = 1.03f;
      if(up) amount = 0.97f;

      if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
      {
        masks_hardness = dt_conf_get_float("plugins/darkroom/spots/brush_hardness");
        masks_hardness = MAX(0.05f, MIN(masks_hardness * amount, 1.0f));
        dt_conf_set_float("plugins/darkroom/spots/brush_hardness", masks_hardness);
      }
      else
      {
        masks_hardness = dt_conf_get_float("plugins/darkroom/masks/brush/hardness");
        masks_hardness = MAX(0.05f, MIN(masks_hardness * amount, 1.0f));
        dt_conf_set_float("plugins/darkroom/masks/brush/hardness", masks_hardness);
      }

      if(gui->guipoints_count > 0)
      {
        dt_masks_dynbuf_set(gui->guipoints_payload, -3, masks_hardness);
      }
    }
    else if((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
    {
      float masks_density;
      float amount = 1.03f;
      if(up) amount = 0.97f;

      if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
      {
        masks_density = dt_conf_get_float("plugins/darkroom/spots/brush_density");
        masks_density = MAX(0.05f, MIN(masks_density * amount, 1.0f));
        dt_conf_set_float("plugins/darkroom/spots/brush_density", masks_density);
      }
      else
      {
        masks_density = dt_conf_get_float("plugins/darkroom/masks/brush/density");
        masks_density = MAX(0.05f, MIN(masks_density * amount, 1.0f));
        dt_conf_set_float("plugins/darkroom/masks/brush/density", masks_density);
      }

      if(gui->guipoints_count > 0)
      {
        dt_masks_dynbuf_set(gui->guipoints_payload, -2, masks_density);
      }
    }

    else
    {
      float masks_border;
      float amount = 1.03f;
      if(up) amount = 0.97f;

      if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
      {
        masks_border = dt_conf_get_float("plugins/darkroom/spots/brush_border");
        masks_border = MAX(0.0005f, MIN(masks_border * amount, 0.5f));
        dt_conf_set_float("plugins/darkroom/spots/brush_border", masks_border);
      }
      else
      {
        masks_border = dt_conf_get_float("plugins/darkroom/masks/brush/border");
        masks_border = MAX(0.005f, MIN(masks_border * amount, 0.5f));
        dt_conf_set_float("plugins/darkroom/masks/brush/border", masks_border);
      }

      if(gui->guipoints_count > 0)
      {
        dt_masks_dynbuf_set(gui->guipoints_payload, -4, masks_border);
      }
    }
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->form_selected || gui->point_selected >= 0 || gui->feather_selected >= 0
          || gui->seg_selected >= 0)
  {
    // we register the current position
    if(gui->scrollx == 0.0f && gui->scrolly == 0.0f)
    {
      gui->scrollx = pzx;
      gui->scrolly = pzy;
    }
    if((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
    {
      // we try to change the opacity
      dt_masks_form_change_opacity(form, parentid, up);
    }
    else
    {
      guint nb = g_list_length(form->points);
      // resize don't care where the mouse is inside a shape
      if((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
      {
        float amount = 1.03f;
        if(up) amount = 0.97f;
        // do not exceed upper limit of 1.0 and lower limit of 0.004
        for(int k = 0; k < nb; k++)
        {
          dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)g_list_nth_data(form->points, k);
          if(amount > 1.0f && (point->border[0] > 1.0f || point->border[1] > 1.0f)) return 1;
        }
        for(int k = 0; k < nb; k++)
        {
          dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)g_list_nth_data(form->points, k);
          point->border[0] *= amount;
          point->border[1] *= amount;
        }
        if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
        {
          float masks_border = dt_conf_get_float("plugins/darkroom/spots/brush_border");
          masks_border = MAX(0.005f, MIN(masks_border * amount, 0.5f));
          dt_conf_set_float("plugins/darkroom/spots/brush_border", masks_border);
        }
        else
        {
          float masks_border = dt_conf_get_float("plugins/darkroom/masks/brush/border");
          masks_border = MAX(0.005f, MIN(masks_border * amount, 0.5f));
          dt_conf_set_float("plugins/darkroom/masks/brush/border", masks_border);
        }
      }
      else
      {
        float amount = 1.03f;
        if(up) amount = 0.97f;
        for(int k = 0; k < nb; k++)
        {
          dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)g_list_nth_data(form->points, k);
          float masks_hardness = point->hardness;
          point->hardness = MAX(0.05f, MIN(masks_hardness * amount, 1.0f));
        }
        if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
        {
          float masks_hardness = dt_conf_get_float("plugins/darkroom/spots/brush_hardness");
          masks_hardness = MAX(0.05f, MIN(masks_hardness * amount, 1.0f));
          dt_conf_set_float("plugins/darkroom/spots/brush_hardness", masks_hardness);
        }
        else
        {
          float masks_hardness = dt_conf_get_float("plugins/darkroom/masks/brush/hardness");
          masks_hardness = MAX(0.05f, MIN(masks_hardness * amount, 1.0f));
          dt_conf_set_float("plugins/darkroom/masks/brush/hardness", masks_hardness);
        }
      }

      dt_masks_write_form(form, darktable.develop);

      // we recreate the form points
      dt_masks_gui_form_remove(form, gui, index);
      dt_masks_gui_form_create(form, gui, index);

      // we save the move
      dt_masks_update_image(darktable.develop);
    }
    return 1;
  }
  return 0;
}

static int dt_brush_events_button_pressed(struct dt_iop_module_t *module, float pzx, float pzy,
                                          double pressure, int which, int type, uint32_t state,
                                          dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui,
                                          int index)
{
  if(type == GDK_2BUTTON_PRESS || type == GDK_3BUTTON_PRESS) return 1;
  if(!gui) return 0;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return 0;

  float masks_border;
  if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
    masks_border = MIN(dt_conf_get_float("plugins/darkroom/spots/brush_border"), 0.5f);
  else
    masks_border = MIN(dt_conf_get_float("plugins/darkroom/masks/brush/border"), 0.5f);

  float masks_hardness;
  if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
    masks_hardness = MIN(dt_conf_get_float("plugins/darkroom/spots/brush_hardness"), 1.0f);
  else
    masks_hardness = MIN(dt_conf_get_float("plugins/darkroom/masks/brush/hardness"), 1.0f);

  float masks_density;
  if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
    masks_density = MIN(dt_conf_get_float("plugins/darkroom/spots/brush_density"), 1.0f);
  else
    masks_density = MIN(dt_conf_get_float("plugins/darkroom/masks/brush/density"), 1.0f);

  if(gui->creation && which == 1
     && (((state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) == (GDK_CONTROL_MASK | GDK_SHIFT_MASK))
         || ((state & GDK_SHIFT_MASK) == GDK_SHIFT_MASK)))
  {
    // set some absolute or relative position for the source of the clone mask
    if(form->type & DT_MASKS_CLONE) dt_masks_set_source_pos_initial_state(gui, state, pzx, pzy);

    return 1;
  }
  else if(which == 1)
  {
    if(gui->creation)
    {
      float wd = darktable.develop->preview_pipe->backbuf_width;
      float ht = darktable.develop->preview_pipe->backbuf_height;

      if(!gui->guipoints) gui->guipoints = dt_masks_dynbuf_init(200000, "brush guipoints");
      if(!gui->guipoints) return 1;
      if(!gui->guipoints_payload) gui->guipoints_payload = dt_masks_dynbuf_init(400000, "brush guipoints_payload");
      if(!gui->guipoints_payload) return 1;
      dt_masks_dynbuf_add(gui->guipoints, pzx * wd);
      dt_masks_dynbuf_add(gui->guipoints, pzy * ht);
      dt_masks_dynbuf_add(gui->guipoints_payload, masks_border);
      dt_masks_dynbuf_add(gui->guipoints_payload, masks_hardness);
      dt_masks_dynbuf_add(gui->guipoints_payload, masks_density);
      dt_masks_dynbuf_add(gui->guipoints_payload, pressure);

      gui->guipoints_count = 1;

      // add support for clone masks
      if(form->type & DT_MASKS_CLONE)
      {
        dt_masks_set_source_pos_initial_value(gui, DT_MASKS_BRUSH, form, pzx, pzy);
      }
      else
      {
        // not used by regular masks
        form->source[0] = form->source[1] = 0.0f;
      }

      gui->pressure_sensitivity = DT_MASKS_PRESSURE_OFF;
      char *psens = dt_conf_get_string("pressure_sensitivity");
      if(psens)
      {
        if(!strcmp(psens, "hardness (absolute)"))
          gui->pressure_sensitivity = DT_MASKS_PRESSURE_HARDNESS_ABS;
        else if(!strcmp(psens, "hardness (relative)"))
          gui->pressure_sensitivity = DT_MASKS_PRESSURE_HARDNESS_REL;
        else if(!strcmp(psens, "opacity (absolute)"))
          gui->pressure_sensitivity = DT_MASKS_PRESSURE_OPACITY_ABS;
        else if(!strcmp(psens, "opacity (relative)"))
          gui->pressure_sensitivity = DT_MASKS_PRESSURE_OPACITY_REL;
        else if(!strcmp(psens, "brush size (relative)"))
          gui->pressure_sensitivity = DT_MASKS_PRESSURE_BRUSHSIZE_REL;
        g_free(psens);
      }

      dt_control_queue_redraw_center();
      return 1;
    }
    else if(gui->source_selected && gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
      if(!gpt) return 0;
      // we start the form dragging
      gui->source_dragging = TRUE;
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
      if(gui->point_edited == gui->point_selected && ((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK))
      {
        dt_masks_point_brush_t *point
            = (dt_masks_point_brush_t *)g_list_nth_data(form->points, gui->point_edited);
        if(point->state != DT_MASKS_POINT_STATE_NORMAL)
        {
          point->state = DT_MASKS_POINT_STATE_NORMAL;
          _brush_init_ctrl_points(form);
        }
        else
        {
          point->ctrl1[0] = point->ctrl2[0] = point->corner[0];
          point->ctrl1[1] = point->ctrl2[1] = point->corner[1];
          point->state = DT_MASKS_POINT_STATE_USER;
        }
        dt_masks_write_form(form, darktable.develop);

        // we recreate the form points
        dt_masks_gui_form_remove(form, gui, index);
        dt_masks_gui_form_create(form, gui, index);
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
      guint nb = g_list_length(form->points);
      gui->point_edited = -1;
      if((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
      {
        // we add a new point to the brush
        dt_masks_point_brush_t *bzpt = (dt_masks_point_brush_t *)(malloc(sizeof(dt_masks_point_brush_t)));

        float wd = darktable.develop->preview_pipe->backbuf_width;
        float ht = darktable.develop->preview_pipe->backbuf_height;
        float pts[2] = { pzx * wd, pzy * ht };
        dt_dev_distort_backtransform(darktable.develop, pts, 1);

        // set coordinates
        bzpt->corner[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
        bzpt->corner[1] = pts[1] / darktable.develop->preview_pipe->iheight;
        bzpt->ctrl1[0] = bzpt->ctrl1[1] = bzpt->ctrl2[0] = bzpt->ctrl2[1] = -1.0;
        bzpt->state = DT_MASKS_POINT_STATE_NORMAL;

        // set other attributes of the new point. we interpolate the starting and the end point of that
        // segment
        float t = _brush_get_position_in_segment(bzpt->corner[0], bzpt->corner[1], form, gui->seg_selected);
        // start and end point of the segment
        dt_masks_point_brush_t *point0
            = (dt_masks_point_brush_t *)g_list_nth_data(form->points, gui->seg_selected);
        dt_masks_point_brush_t *point1
            = (dt_masks_point_brush_t *)g_list_nth_data(form->points, gui->seg_selected + 1);
        bzpt->border[0] = point0->border[0] * (1.0f - t) + point1->border[0] * t;
        bzpt->border[1] = point0->border[1] * (1.0f - t) + point1->border[1] * t;
        bzpt->hardness = point0->hardness * (1.0f - t) + point1->hardness * t;
        bzpt->density = point0->density * (1.0f - t) + point1->density * t;

        form->points = g_list_insert(form->points, bzpt, gui->seg_selected + 1);
        _brush_init_ctrl_points(form);
        dt_masks_gui_form_remove(form, gui, index);
        dt_masks_gui_form_create(form, gui, index);
        gui->point_edited = gui->point_dragging = gui->point_selected = gui->seg_selected + 1;
        gui->seg_selected = -1;
        dt_control_queue_redraw_center();
      }
      else if(gui->seg_selected >= 0 && gui->seg_selected < nb - 1)
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
  else if(gui->creation && which == 3)
  {
    dt_masks_dynbuf_free(gui->guipoints);
    dt_masks_dynbuf_free(gui->guipoints_payload);
    gui->guipoints = NULL;
    gui->guipoints_payload = NULL;
    gui->guipoints_count = 0;

    gui->creation_continuous = FALSE;
    gui->creation_continuous_module = NULL;

    dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
    dt_masks_iop_update(module);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->point_selected >= 0 && which == 3)
  {
    // we remove the point (and the entire form if there is too few points)
    if(g_list_length(form->points) <= 2)
    {
      // if the form doesn't below to a group, we don't delete it
      if(parentid <= 0) return 1;

      // we hide the form
      if(!(darktable.develop->form_visible->type & DT_MASKS_GROUP))
        dt_masks_change_form_gui(NULL);
      else if(g_list_length(darktable.develop->form_visible->points) < 2)
        dt_masks_change_form_gui(NULL);
      else
      {
        int emode = gui->edit_mode;
        dt_masks_clear_form_gui(darktable.develop);
        GList *forms = g_list_first(darktable.develop->form_visible->points);
        while(forms)
        {
          dt_masks_point_group_t *gpt = (dt_masks_point_group_t *)forms->data;
          if(gpt->formid == form->formid)
          {
            darktable.develop->form_visible->points
                = g_list_remove(darktable.develop->form_visible->points, gpt);
            free(gpt);
            break;
          }
          forms = g_list_next(forms);
        }
        gui->edit_mode = emode;
      }

      // we delete or remove the shape
      dt_masks_form_remove(module, NULL, form);
      dt_dev_masks_list_change(darktable.develop);
      dt_control_queue_redraw_center();
      return 1;
    }
    dt_masks_point_brush_t *point
        = (dt_masks_point_brush_t *)g_list_nth_data(form->points, gui->point_selected);
    form->points = g_list_remove(form->points, point);
    free(point);
    gui->point_selected = -1;
    _brush_init_ctrl_points(form);

    dt_masks_write_form(form, darktable.develop);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);
    // we save the move
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->feather_selected >= 0 && which == 3)
  {
    dt_masks_point_brush_t *point
        = (dt_masks_point_brush_t *)g_list_nth_data(form->points, gui->feather_selected);
    if(point->state != DT_MASKS_POINT_STATE_NORMAL)
    {
      point->state = DT_MASKS_POINT_STATE_NORMAL;
      _brush_init_ctrl_points(form);

      dt_masks_write_form(form, darktable.develop);

      // we recreate the form points
      dt_masks_gui_form_remove(form, gui, index);
      dt_masks_gui_form_create(form, gui, index);
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
    else if(g_list_length(darktable.develop->form_visible->points) < 2)
      dt_masks_change_form_gui(NULL);
    else
    {
      dt_masks_clear_form_gui(darktable.develop);
      GList *forms = g_list_first(darktable.develop->form_visible->points);
      while(forms)
      {
        dt_masks_point_group_t *gpt = (dt_masks_point_group_t *)forms->data;
        if(gpt->formid == form->formid)
        {
          darktable.develop->form_visible->points
              = g_list_remove(darktable.develop->form_visible->points, gpt);
          free(gpt);
          break;
        }
        forms = g_list_next(forms);
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

static int dt_brush_events_button_released(struct dt_iop_module_t *module, float pzx, float pzy, int which,
                                           uint32_t state, dt_masks_form_t *form, int parentid,
                                           dt_masks_form_gui_t *gui, int index)
{
  if(!gui) return 0;

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return 0;

  float masks_border;
  if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
    masks_border = MIN(dt_conf_get_float("plugins/darkroom/spots/brush_border"), 0.5f);
  else
    masks_border = MIN(dt_conf_get_float("plugins/darkroom/masks/brush/border"), 0.5f);

  if(gui->creation && which == 1 && (state & (GDK_CONTROL_MASK | GDK_SHIFT_MASK)))
  {
    // user just set the source position, so just return
    return 1;
  }
  else if(gui->creation && which == 1)
  {
    dt_iop_module_t *crea_module = gui->creation_module;

    if(gui->guipoints && gui->guipoints_count > 0)
    {
      // if the path consists only of one x/y pair we add a second one close so we don't need to deal with
      // this special case later
      if(gui->guipoints_count == 1)
      {
        // add a helper node very close to the single spot
        const float x = dt_masks_dynbuf_get(gui->guipoints, -2) + 0.01f;
        const float y = dt_masks_dynbuf_get(gui->guipoints, -1) - 0.01f;
        dt_masks_dynbuf_add(gui->guipoints, x);
        dt_masks_dynbuf_add(gui->guipoints, y);
        const float border = dt_masks_dynbuf_get(gui->guipoints_payload, -4);
        const float hardness = dt_masks_dynbuf_get(gui->guipoints_payload, -3);
        const float density = dt_masks_dynbuf_get(gui->guipoints_payload, -2);
        const float pressure = dt_masks_dynbuf_get(gui->guipoints_payload, -1);
        dt_masks_dynbuf_add(gui->guipoints_payload, border);
        dt_masks_dynbuf_add(gui->guipoints_payload, hardness);
        dt_masks_dynbuf_add(gui->guipoints_payload, density);
        dt_masks_dynbuf_add(gui->guipoints_payload, pressure);
        gui->guipoints_count++;
      }

      float *guipoints = dt_masks_dynbuf_buffer(gui->guipoints);
      float *guipoints_payload = dt_masks_dynbuf_buffer(gui->guipoints_payload);

      // we transform the points
      dt_dev_distort_backtransform(darktable.develop, guipoints, gui->guipoints_count);

      for(int i = 0; i < gui->guipoints_count; i++)
      {
        guipoints[i * 2] /= darktable.develop->preview_pipe->iwidth;
        guipoints[i * 2 + 1] /= darktable.develop->preview_pipe->iheight;
      }

      // we consolidate pen pressure readings into payload
      for(int i = 0; i < gui->guipoints_count; i++)
      {
        float *payload = guipoints_payload + 4 * i;
        float pressure = payload[3];
        payload[3] = 1.0f;

        switch(gui->pressure_sensitivity)
        {
          case DT_MASKS_PRESSURE_BRUSHSIZE_REL:
            payload[0] = MAX(0.005f, payload[0] * pressure);
            break;
          case DT_MASKS_PRESSURE_HARDNESS_ABS:
            payload[1] = MAX(0.05f, pressure);
            break;
          case DT_MASKS_PRESSURE_HARDNESS_REL:
            payload[1] = MAX(0.05f, payload[1] * pressure);
            break;
          case DT_MASKS_PRESSURE_OPACITY_ABS:
            payload[2] = MAX(0.05f, pressure);
            break;
          case DT_MASKS_PRESSURE_OPACITY_REL:
            payload[2] = MAX(0.05f, payload[2] * pressure);
            break;
          default:
          case DT_MASKS_PRESSURE_OFF:
            // ignore pressure value
            break;
        }
      }

      float factor = 0.01f;
      char *smoothing = dt_conf_get_string("brush_smoothing");
      if(smoothing)
      {
        if(!strcmp(smoothing, "low"))
          factor = 0.0025f;
        else if(!strcmp(smoothing, "medium"))
          factor = 0.01f;
        else if(!strcmp(smoothing, "high"))
          factor = 0.04f;
        g_free(smoothing);
      }

      // accuracy level for node elimination, dependent on brush size
      const float epsilon2 = factor * MAX(0.005f, masks_border) * MAX(0.005f, masks_border);

      // we simplify the path and generate the nodes
      form->points = _brush_ramer_douglas_peucker(guipoints, gui->guipoints_count, guipoints_payload, epsilon2);

      // printf("guipoints_count %d, points %d\n", gui->guipoints_count, g_list_length(form->points));

      _brush_init_ctrl_points(form);

      dt_masks_dynbuf_free(gui->guipoints);
      dt_masks_dynbuf_free(gui->guipoints_payload);
      gui->guipoints = NULL;
      gui->guipoints_payload = NULL;
      gui->guipoints_count = 0;

      // we save the form and quit creation mode
      dt_masks_gui_form_save_creation(darktable.develop, crea_module, form, gui);

      if(crea_module)
      {
        dt_dev_add_history_item(darktable.develop, crea_module, TRUE);
        // and we switch in edit mode to show all the forms
        if(gui->creation_continuous)
          dt_masks_set_edit_mode_single_form(crea_module, form->formid, DT_MASKS_EDIT_FULL);
        else
          dt_masks_set_edit_mode(crea_module, DT_MASKS_EDIT_FULL);
        dt_masks_iop_update(crea_module);
        gui->creation_module = NULL;
      }
      else
      {
        dt_dev_masks_selection_change(darktable.develop, form->formid, TRUE);
      }

      if(gui->creation_continuous)
      {
        dt_masks_form_t *form_new = dt_masks_create(form->type);
        dt_masks_change_form_gui(form_new);

        darktable.develop->form_gui->creation = TRUE;
        darktable.develop->form_gui->creation_module = gui->creation_continuous_module;
      }
      else if(form->type & (DT_MASKS_CLONE | DT_MASKS_NON_CLONE))
      {
        dt_masks_form_t *grp = darktable.develop->form_visible;
        if(!grp || !(grp->type & DT_MASKS_GROUP)) return 1;
        int pos3 = 0, pos2 = -1;
        GList *fs = g_list_first(grp->points);
        while(fs)
        {
          dt_masks_point_group_t *pt = (dt_masks_point_group_t *)fs->data;
          if(pt->formid == form->formid)
          {
            pos2 = pos3;
            break;
          }
          pos3++;
          fs = g_list_next(fs);
        }
        if(pos2 < 0) return 1;
        dt_masks_form_gui_t *gui2 = darktable.develop->form_gui;
        if(!gui2) return 1;
        gui2->group_selected = pos2;

        dt_masks_select_form(crea_module, dt_masks_get_from_id(darktable.develop, form->formid));
      }
    }
    else
    {
      // unlikely case of button released but no points gathered -> no form
      dt_masks_dynbuf_free(gui->guipoints);
      dt_masks_dynbuf_free(gui->guipoints_payload);
      gui->guipoints = NULL;
      gui->guipoints_payload = NULL;
      gui->guipoints_count = 0;

      gui->creation_continuous = FALSE;
      gui->creation_continuous_module = NULL;

      dt_masks_set_edit_mode(module, DT_MASKS_EDIT_FULL);
      dt_masks_iop_update(module);

      dt_masks_change_form_gui(NULL);
    }

    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->form_dragging)
  {
    // we end the form dragging
    gui->form_dragging = FALSE;

    // we get point0 new values
    dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)g_list_first(form->points)->data;
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    float dx = pts[0] / darktable.develop->preview_pipe->iwidth - point->corner[0];
    float dy = pts[1] / darktable.develop->preview_pipe->iheight - point->corner[1];

    // we move all points
    GList *points = g_list_first(form->points);
    while(points)
    {
      point = (dt_masks_point_brush_t *)points->data;
      point->corner[0] += dx;
      point->corner[1] += dy;
      point->ctrl1[0] += dx;
      point->ctrl1[1] += dy;
      point->ctrl2[0] += dx;
      point->ctrl2[1] += dy;
      points = g_list_next(points);
    }

    dt_masks_write_form(form, darktable.develop);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);

    // we save the move
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->source_dragging)
  {
    // we end the form dragging
    gui->source_dragging = FALSE;

    // we change the source value
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    form->source[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
    form->source[1] = pts[1] / darktable.develop->preview_pipe->iheight;
    dt_masks_write_form(form, darktable.develop);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);

    // we save the move
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->seg_dragging >= 0)
  {
    gui->seg_dragging = -1;
    dt_masks_write_form(form, darktable.develop);
    dt_masks_update_image(darktable.develop);
    return 1;
  }
  else if(gui->point_dragging >= 0)
  {
    dt_masks_point_brush_t *point
        = (dt_masks_point_brush_t *)g_list_nth_data(form->points, gui->point_dragging);
    gui->point_dragging = -1;
    if(gui->scrollx != 0.0f || gui->scrolly != 0.0f)
    {
      gui->scrollx = gui->scrolly = 0;
      return 1;
    }
    gui->scrollx = gui->scrolly = 0;
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd, pzy * ht };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    float dx = pts[0] / darktable.develop->preview_pipe->iwidth - point->corner[0];
    float dy = pts[1] / darktable.develop->preview_pipe->iheight - point->corner[1];

    point->corner[0] += dx;
    point->corner[1] += dy;
    point->ctrl1[0] += dx;
    point->ctrl1[1] += dy;
    point->ctrl2[0] += dx;
    point->ctrl2[1] += dy;

    _brush_init_ctrl_points(form);

    dt_masks_write_form(form, darktable.develop);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);
    // we save the move
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->feather_dragging >= 0)
  {
    dt_masks_point_brush_t *point
        = (dt_masks_point_brush_t *)g_list_nth_data(form->points, gui->feather_dragging);
    gui->feather_dragging = -1;
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd, pzy * ht };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);

    int p1x, p1y, p2x, p2y;
    _brush_feather_to_ctrl(point->corner[0] * darktable.develop->preview_pipe->iwidth,
                           point->corner[1] * darktable.develop->preview_pipe->iheight, pts[0], pts[1], &p1x,
                           &p1y, &p2x, &p2y, TRUE);
    point->ctrl1[0] = (float)p1x / darktable.develop->preview_pipe->iwidth;
    point->ctrl1[1] = (float)p1y / darktable.develop->preview_pipe->iheight;
    point->ctrl2[0] = (float)p2x / darktable.develop->preview_pipe->iwidth;
    point->ctrl2[1] = (float)p2y / darktable.develop->preview_pipe->iheight;

    point->state = DT_MASKS_POINT_STATE_USER;

    _brush_init_ctrl_points(form);

    dt_masks_write_form(form, darktable.develop);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);
    // we save the move
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(gui->point_border_dragging >= 0)
  {
    gui->point_border_dragging = -1;

    // we save the move
    dt_masks_write_form(form, darktable.develop);
    dt_masks_update_image(darktable.develop);
    dt_control_queue_redraw_center();
    return 1;
  }

  return 0;
}

static int dt_brush_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy, double pressure,
                                       int which, dt_masks_form_t *form, int parentid,
                                       dt_masks_form_gui_t *gui, int index)
{
  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  int closeup = dt_control_get_dev_closeup();
  float zoom_scale = dt_dev_get_zoom_scale(darktable.develop, zoom, 1<<closeup, 1);
  float as = 0.005f / zoom_scale * darktable.develop->preview_pipe->backbuf_width;
  if(!gui) return 0;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return 0;

  if(gui->creation)
  {
    if(gui->guipoints)
    {
      dt_masks_dynbuf_add(gui->guipoints, pzx * darktable.develop->preview_pipe->backbuf_width);
      dt_masks_dynbuf_add(gui->guipoints, pzy * darktable.develop->preview_pipe->backbuf_height);
      const float border = dt_masks_dynbuf_get(gui->guipoints_payload, -4);
      const float hardness = dt_masks_dynbuf_get(gui->guipoints_payload, -3);
      const float density = dt_masks_dynbuf_get(gui->guipoints_payload, -2);
      dt_masks_dynbuf_add(gui->guipoints_payload, border);
      dt_masks_dynbuf_add(gui->guipoints_payload, hardness);
      dt_masks_dynbuf_add(gui->guipoints_payload, density);
      dt_masks_dynbuf_add(gui->guipoints_payload, pressure);
      gui->guipoints_count++;
    }
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->point_dragging >= 0)
  {
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd, pzy * ht };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    dt_masks_point_brush_t *bzpt
        = (dt_masks_point_brush_t *)g_list_nth_data(form->points, gui->point_dragging);
    pzx = pts[0] / darktable.develop->preview_pipe->iwidth;
    pzy = pts[1] / darktable.develop->preview_pipe->iheight;
    bzpt->ctrl1[0] += pzx - bzpt->corner[0];
    bzpt->ctrl2[0] += pzx - bzpt->corner[0];
    bzpt->ctrl1[1] += pzy - bzpt->corner[1];
    bzpt->ctrl2[1] += pzy - bzpt->corner[1];
    bzpt->corner[0] = pzx;
    bzpt->corner[1] = pzy;
    _brush_init_ctrl_points(form);
    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->seg_dragging >= 0)
  {
    // we get point0 new values
    int pos2 = (gui->seg_dragging + 1) % g_list_length(form->points);
    dt_masks_point_brush_t *point
        = (dt_masks_point_brush_t *)g_list_nth_data(form->points, gui->seg_dragging);
    dt_masks_point_brush_t *point2 = (dt_masks_point_brush_t *)g_list_nth_data(form->points, pos2);
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd + gui->dx, pzy * ht + gui->dy };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    float dx = pts[0] / darktable.develop->preview_pipe->iwidth - point->corner[0];
    float dy = pts[1] / darktable.develop->preview_pipe->iheight - point->corner[1];

    // we move all points
    point->corner[0] += dx;
    point->corner[1] += dy;
    point->ctrl1[0] += dx;
    point->ctrl1[1] += dy;
    point->ctrl2[0] += dx;
    point->ctrl2[1] += dy;
    point2->corner[0] += dx;
    point2->corner[1] += dy;
    point2->ctrl1[0] += dx;
    point2->ctrl1[1] += dy;
    point2->ctrl2[0] += dx;
    point2->ctrl2[1] += dy;

    _brush_init_ctrl_points(form);

    dt_masks_write_form(form, darktable.develop);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);

    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->feather_dragging >= 0)
  {
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd, pzy * ht };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);
    dt_masks_point_brush_t *point
        = (dt_masks_point_brush_t *)g_list_nth_data(form->points, gui->feather_dragging);

    int p1x, p1y, p2x, p2y;
    _brush_feather_to_ctrl(point->corner[0] * darktable.develop->preview_pipe->iwidth,
                           point->corner[1] * darktable.develop->preview_pipe->iheight, pts[0], pts[1], &p1x,
                           &p1y, &p2x, &p2y, TRUE);
    point->ctrl1[0] = (float)p1x / darktable.develop->preview_pipe->iwidth;
    point->ctrl1[1] = (float)p1y / darktable.develop->preview_pipe->iheight;
    point->ctrl2[0] = (float)p2x / darktable.develop->preview_pipe->iwidth;
    point->ctrl2[1] = (float)p2y / darktable.develop->preview_pipe->iheight;
    point->state = DT_MASKS_POINT_STATE_USER;

    _brush_init_ctrl_points(form);
    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->point_border_dragging >= 0)
  {
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;

    int k = gui->point_border_dragging;

    // now we want to know the position reflected on actual corner/border segment
    float a = (gpt->border[k * 6 + 1] - gpt->points[k * 6 + 3])
              / (float)(gpt->border[k * 6] - gpt->points[k * 6 + 2]);
    float b = gpt->points[k * 6 + 3] - a * gpt->points[k * 6 + 2];

    float pts[2];
    pts[0] = (a * pzy * ht + pzx * wd - b * a) / (a * a + 1.0);
    pts[1] = a * pts[0] + b;

    dt_dev_distort_backtransform(darktable.develop, pts, 1);

    dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)g_list_nth_data(form->points, k);
    float nx = point->corner[0] * darktable.develop->preview_pipe->iwidth;
    float ny = point->corner[1] * darktable.develop->preview_pipe->iheight;
    float nr = sqrtf((pts[0] - nx) * (pts[0] - nx) + (pts[1] - ny) * (pts[1] - ny));
    float bdr = nr / fminf(darktable.develop->preview_pipe->iwidth, darktable.develop->preview_pipe->iheight);

    point->border[0] = point->border[1] = bdr;

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if(gui->form_dragging || gui->source_dragging)
  {
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
  guint nb = g_list_length(form->points);

  pzx *= darktable.develop->preview_pipe->backbuf_width,
      pzy *= darktable.develop->preview_pipe->backbuf_height;

  if((gui->group_selected == index) && gui->point_edited >= 0)
  {
    int k = gui->point_edited;
    // we only select feather if the point is not "sharp"
    if(gpt->points[k * 6 + 2] != gpt->points[k * 6 + 4] && gpt->points[k * 6 + 3] != gpt->points[k * 6 + 5])
    {
      int ffx, ffy;
      _brush_ctrl2_to_feather(gpt->points[k * 6 + 2], gpt->points[k * 6 + 3], gpt->points[k * 6 + 4],
                              gpt->points[k * 6 + 5], &ffx, &ffy, TRUE);
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
    if(pzx - gpt->border[k * 6] > -as && pzx - gpt->border[k * 6] < as && pzy - gpt->border[k * 6 + 1] > -as
       && pzy - gpt->border[k * 6 + 1] < as)
    {
      gui->point_border_selected = k;
      dt_control_queue_redraw_center();
      return 1;
    }
  }

  // are we inside the form or the borders or near a segment ???
  int in, inb, near, ins;
  dt_brush_get_distance(pzx, (int)pzy, as, gui, index, nb, &in, &inb, &near, &ins);
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

static void dt_brush_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *gui, int index,
                                        int nb)
{
  double dashed[] = { 4.0, 4.0 };
  dashed[0] /= zoom_scale;
  dashed[1] /= zoom_scale;
  int len = sizeof(dashed) / sizeof(dashed[0]);

  if(!gui) return;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return;
  float dx = 0, dy = 0, dxs = 0, dys = 0;
  if((gui->group_selected == index) && gui->form_dragging)
  {
    dx = gui->posx + gui->dx - gpt->points[2];
    dy = gui->posy + gui->dy - gpt->points[3];
  }
  if((gui->group_selected == index) && gui->source_dragging)
  {
    dxs = gui->posx + gui->dx - gpt->source[2];
    dys = gui->posy + gui->dy - gpt->source[3];
  }

  // in creation mode
  if(gui->creation)
  {
    float wd = darktable.develop->preview_pipe->iwidth;
    float ht = darktable.develop->preview_pipe->iheight;

    if(gui->guipoints_count == 0)
    {
      dt_masks_form_t *form = darktable.develop->form_visible;
      if(!form) return;

      float masks_border;
      if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
        masks_border = MIN(dt_conf_get_float("plugins/darkroom/spots/brush_border"), 0.5f);
      else
        masks_border = MIN(dt_conf_get_float("plugins/darkroom/masks/brush/border"), 0.5f);

      float masks_hardness;
      if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
        masks_hardness = MIN(dt_conf_get_float("plugins/darkroom/spots/brush_hardness"), 1.0f);
      else
        masks_hardness = MIN(dt_conf_get_float("plugins/darkroom/masks/brush/hardness"), 1.0f);

      float masks_density;
      if(form->type & (DT_MASKS_CLONE|DT_MASKS_NON_CLONE))
        masks_density = MIN(dt_conf_get_float("plugins/darkroom/spots/brush_density"), 1.0f);
      else
        masks_density = MIN(dt_conf_get_float("plugins/darkroom/masks/brush/density"), 1.0f);

      float radius1 = masks_border * masks_hardness * MIN(wd, ht);
      float radius2 = masks_border * MIN(wd, ht);

      float xpos, ypos;
      if(gui->posx == -1.f && gui->posy == -1.f)
      {
        xpos = (.5f + dt_control_get_dev_zoom_x()) * darktable.develop->preview_pipe->backbuf_width;
        ypos = (.5f + dt_control_get_dev_zoom_y()) * darktable.develop->preview_pipe->backbuf_height;
      }
      else
      {
        xpos = gui->posx;
        ypos = gui->posy;
      }

      cairo_save(cr);
      dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_BRUSH_CURSOR, masks_density);
      if(masks_density < 1.0) cairo_set_line_width(cr, cairo_get_line_width(cr) * .8);
      cairo_arc(cr, xpos, ypos, radius1, 0, 2.0 * M_PI);
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, .8, .8, .8, .8);
      cairo_stroke(cr);
      cairo_set_dash(cr, dashed, len, 0);
      cairo_arc(cr, xpos, ypos, radius2, 0, 2.0 * M_PI);
      cairo_stroke(cr);

      if(form->type & DT_MASKS_CLONE)
      {
        float x = 0.f, y = 0.f;
        dt_masks_calculate_source_pos_value(gui, DT_MASKS_BRUSH, xpos, ypos, xpos, ypos, &x, &y, FALSE);
        dt_masks_draw_clone_source_pos(cr, zoom_scale, x, y);
      }

      cairo_restore(cr);
    }
    else
    {
      float masks_border, masks_hardness, masks_density;
      float radius, oldradius, opacity, oldopacity, pressure;
      int stroked = 1;

      const float *guipoints = dt_masks_dynbuf_buffer(gui->guipoints);
      const float *guipoints_payload = dt_masks_dynbuf_buffer(gui->guipoints_payload);

      cairo_save(cr);
      cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
      cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
      float linewidth = cairo_get_line_width(cr);
      masks_border = guipoints_payload[0];
      masks_hardness = guipoints_payload[1];
      masks_density = guipoints_payload[2];
      pressure = guipoints_payload[3];

      switch(gui->pressure_sensitivity)
      {
        case DT_MASKS_PRESSURE_HARDNESS_ABS:
          masks_hardness = MAX(0.05f, pressure);
          break;
        case DT_MASKS_PRESSURE_HARDNESS_REL:
          masks_hardness = MAX(0.05f, masks_hardness * pressure);
          break;
        case DT_MASKS_PRESSURE_OPACITY_ABS:
          masks_density = MAX(0.05f, pressure);
          break;
        case DT_MASKS_PRESSURE_OPACITY_REL:
          masks_density = MAX(0.05f, masks_density * pressure);
          break;
        case DT_MASKS_PRESSURE_BRUSHSIZE_REL:
          masks_border = MAX(0.005f, masks_border * pressure);
          break;
        default:
        case DT_MASKS_PRESSURE_OFF:
          // ignore pressure value
          break;
      }

      radius = oldradius = masks_border * masks_hardness * MIN(wd, ht);
      opacity = oldopacity = masks_density;

      cairo_set_line_width(cr, 2 * radius);
      dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_BRUSH_TRACE, opacity);

      cairo_move_to(cr, guipoints[0], guipoints[1]);
      for(int i = 1; i < gui->guipoints_count; i++)
      {
        cairo_line_to(cr, guipoints[i * 2], guipoints[i * 2 + 1]);
        stroked = 0;
        masks_border = guipoints_payload[i * 4];
        masks_hardness = guipoints_payload[i * 4 + 1];
        masks_density = guipoints_payload[i * 4 + 2];
        pressure = guipoints_payload[i * 4 + 3];

        switch(gui->pressure_sensitivity)
        {
          case DT_MASKS_PRESSURE_HARDNESS_ABS:
            masks_hardness = MAX(0.05f, pressure);
            break;
          case DT_MASKS_PRESSURE_HARDNESS_REL:
            masks_hardness = MAX(0.05f, masks_hardness * pressure);
            break;
          case DT_MASKS_PRESSURE_OPACITY_ABS:
            masks_density = MAX(0.05f, pressure);
            break;
          case DT_MASKS_PRESSURE_OPACITY_REL:
            masks_density = MAX(0.05f, masks_density * pressure);
            break;
          case DT_MASKS_PRESSURE_BRUSHSIZE_REL:
            masks_border = MAX(0.005f, masks_border * pressure);
            break;
          default:
          case DT_MASKS_PRESSURE_OFF:
            // ignore pressure value
            break;
        }

        radius = masks_border * masks_hardness * MIN(wd, ht);
        opacity = masks_density;

        if(radius != oldradius || opacity != oldopacity)
        {
          cairo_stroke(cr);
          stroked = 1;
          cairo_set_line_width(cr, 2 * radius);
          dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_BRUSH_TRACE, opacity);
          oldradius = radius;
          oldopacity = opacity;
          cairo_move_to(cr, guipoints[i * 2], guipoints[i * 2 + 1]);
        }
      }
      if(!stroked) cairo_stroke(cr);

      cairo_set_line_width(cr, linewidth);
      dt_gui_gtk_set_source_rgba(cr, DT_GUI_COLOR_BRUSH_CURSOR, opacity);
      if(opacity < 1.0) cairo_set_line_width(cr, cairo_get_line_width(cr) * .8);
      cairo_arc(cr, guipoints[2 * (gui->guipoints_count - 1)],
                guipoints[2 * (gui->guipoints_count - 1) + 1], radius, 0, 2.0 * M_PI);
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, .8, .8, .8, .8);
      cairo_stroke(cr);
      cairo_set_dash(cr, dashed, len, 0);
      cairo_arc(cr, guipoints[2 * (gui->guipoints_count - 1)],
                guipoints[2 * (gui->guipoints_count - 1) + 1], masks_border * MIN(wd, ht), 0,
                2.0 * M_PI);
      cairo_stroke(cr);

      if(darktable.develop->form_visible && (darktable.develop->form_visible->type & DT_MASKS_CLONE))
      {
        const int i = gui->guipoints_count - 1;
        float x = 0.f, y = 0.f;
        dt_masks_calculate_source_pos_value(gui, DT_MASKS_BRUSH, guipoints[0], guipoints[1], guipoints[i * 2],
                                            guipoints[i * 2 + 1], &x, &y, TRUE);
        dt_masks_draw_clone_source_pos(cr, zoom_scale, x, y);
      }

      cairo_restore(cr);
    }
    return;
  }

  // draw path
  if(gpt->points_count > nb * 3 + 2)
  {
    cairo_set_dash(cr, dashed, 0, 0);

    cairo_move_to(cr, gpt->points[nb * 6] + dx, gpt->points[nb * 6 + 1] + dy);
    int seg = 1, seg2 = 0;
    for(int i = nb * 3; i < gpt->points_count; i++)
    {
      cairo_line_to(cr, gpt->points[i * 2] + dx, gpt->points[i * 2 + 1] + dy);
      // we decide to highlight the form segment by segment
      if(gpt->points[i * 2 + 1] == gpt->points[seg * 6 + 3] && gpt->points[i * 2] == gpt->points[seg * 6 + 2])
      {
        // this is the end of the last segment, so we have to draw it
        if((gui->group_selected == index)
           && (gui->form_selected || gui->form_dragging || gui->seg_selected == seg2))
          cairo_set_line_width(cr, 5.0 / zoom_scale);
        else
          cairo_set_line_width(cr, 3.0 / zoom_scale);
        cairo_set_source_rgba(cr, .3, .3, .3, .8);
        cairo_stroke_preserve(cr);
        if((gui->group_selected == index)
           && (gui->form_selected || gui->form_dragging || gui->seg_selected == seg2))
          cairo_set_line_width(cr, 2.0 / zoom_scale);
        else
          cairo_set_line_width(cr, 1.0 / zoom_scale);
        cairo_set_source_rgba(cr, .8, .8, .8, .8);
        cairo_stroke(cr);
        // and we update the segment number
        seg = (seg + 1) % nb;
        seg2++;
        cairo_move_to(cr, gpt->points[i * 2] + dx, gpt->points[i * 2 + 1] + dy);
      }
    }
  }

  // draw corners
  float anchor_size;
  if(gui->group_selected == index && gpt->points_count > nb * 3 + 2)
  {
    for(int k = 0; k < nb; k++)
    {
      if(k == gui->point_dragging || k == gui->point_selected)
      {
        anchor_size = 7.0f / zoom_scale;
      }
      else
      {
        anchor_size = 5.0f / zoom_scale;
      }
      cairo_set_source_rgba(cr, .8, .8, .8, .8);
      cairo_rectangle(cr, gpt->points[k * 6 + 2] - (anchor_size * 0.5) + dx,
                      gpt->points[k * 6 + 3] - (anchor_size * 0.5) + dy, anchor_size, anchor_size);
      cairo_fill_preserve(cr);

      if((gui->group_selected == index) && (k == gui->point_dragging || k == gui->point_selected))
        cairo_set_line_width(cr, 2.0 / zoom_scale);
      else if((gui->group_selected == index)
              && ((k == 0 || k == nb) && gui->creation && gui->creation_closing_form))
        cairo_set_line_width(cr, 2.0 / zoom_scale);
      else
        cairo_set_line_width(cr, 1.0 / zoom_scale);
      cairo_set_source_rgba(cr, .3, .3, .3, .8);
      cairo_stroke(cr);
    }
  }

  // draw feathers
  if((gui->group_selected == index) && gui->point_edited >= 0)
  {
    int k = gui->point_edited;
    // uncomment this part if you want to see "real" control points
    /*cairo_move_to(cr, gui->points[k*6+2]+dx,gui->points[k*6+3]+dy);
    cairo_line_to(cr, gui->points[k*6]+dx,gui->points[k*6+1]+dy);
    cairo_stroke(cr);
    cairo_move_to(cr, gui->points[k*6+2]+dx,gui->points[k*6+3]+dy);
    cairo_line_to(cr, gui->points[k*6+4]+dx,gui->points[k*6+5]+dy);
    cairo_stroke(cr);*/
    int ffx, ffy;
    _brush_ctrl2_to_feather(gpt->points[k * 6 + 2] + dx, gpt->points[k * 6 + 3] + dy,
                            gpt->points[k * 6 + 4] + dx, gpt->points[k * 6 + 5] + dy, &ffx, &ffy, TRUE);
    cairo_move_to(cr, gpt->points[k * 6 + 2] + dx, gpt->points[k * 6 + 3] + dy);
    cairo_line_to(cr, ffx, ffy);
    cairo_set_line_width(cr, 1.5 / zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_stroke_preserve(cr);
    cairo_set_line_width(cr, 0.75 / zoom_scale);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_stroke(cr);

    if((gui->group_selected == index) && (k == gui->feather_dragging || k == gui->feather_selected))
      cairo_arc(cr, ffx, ffy, 3.0f / zoom_scale, 0, 2.0 * M_PI);
    else
      cairo_arc(cr, ffx, ffy, 1.5f / zoom_scale, 0, 2.0 * M_PI);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_fill_preserve(cr);

    cairo_set_line_width(cr, 1.0 / zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_stroke(cr);
  }

  // draw border and corners
  if((gui->group_selected == index) && gpt->border_count > nb * 3 + 2)
  {


    cairo_move_to(cr, gpt->border[nb * 6] + dx, gpt->border[nb * 6 + 1] + dy);

    for(int i = nb * 3 + 1; i < gpt->border_count; i++)
    {
      cairo_line_to(cr, gpt->border[i * 2] + dx, gpt->border[i * 2 + 1] + dy);
    }
    // we execute the drawing
    if(gui->border_selected)
      cairo_set_line_width(cr, 2.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_set_dash(cr, dashed, len, 0);
    cairo_stroke_preserve(cr);
    if(gui->border_selected)
      cairo_set_line_width(cr, 2.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_set_dash(cr, dashed, len, 4);
    cairo_stroke(cr);

#if 0
    //we draw the brush segment by segment
    for (int k=0; k<nb; k++)
    {
      //draw the point
      if (gui->point_border_selected == k)
      {
        anchor_size = 7.0f / zoom_scale;
      }
      else
      {
        anchor_size = 5.0f / zoom_scale;
      }
      cairo_set_source_rgba(cr, .8, .8, .8, .8);
      cairo_rectangle(cr,
                      gpt->border[k*6] - (anchor_size*0.5)+dx,
                      gpt->border[k*6+1] - (anchor_size*0.5)+dy,
                      anchor_size, anchor_size);
      cairo_fill_preserve(cr);

      if (gui->point_border_selected == k) cairo_set_line_width(cr, 2.0/zoom_scale);
      else cairo_set_line_width(cr, 1.0/zoom_scale);
      cairo_set_source_rgba(cr, .3, .3, .3, .8);
      cairo_set_dash(cr, dashed, 0, 0);
      cairo_stroke(cr);
    }
#endif
  }

  // draw the source if needed
  if(!gui->creation && gpt->source_count > nb * 3 + 2)
  {
    // we draw the line between source and dest
    cairo_move_to(cr, gpt->source[2] + dxs, gpt->source[3] + dys);
    cairo_line_to(cr, gpt->points[2] + dx, gpt->points[3] + dy);
    cairo_set_dash(cr, dashed, 0, 0);
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, 2.5 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.5 / zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_stroke_preserve(cr);
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 0.5 / zoom_scale);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_stroke(cr);

    // we draw the source
    cairo_set_dash(cr, dashed, 0, 0);
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, 2.5 / zoom_scale);
    else
      cairo_set_line_width(cr, 1.5 / zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_move_to(cr, gpt->source[nb * 6] + dxs, gpt->source[nb * 6 + 1] + dys);
    for(int i = nb * 3; i < gpt->source_count; i++)
      cairo_line_to(cr, gpt->source[i * 2] + dxs, gpt->source[i * 2 + 1] + dys);
    cairo_line_to(cr, gpt->source[nb * 6] + dxs, gpt->source[nb * 6 + 1] + dys);
    cairo_stroke_preserve(cr);
    if((gui->group_selected == index) && (gui->form_selected || gui->form_dragging))
      cairo_set_line_width(cr, 1.0 / zoom_scale);
    else
      cairo_set_line_width(cr, 0.5 / zoom_scale);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_stroke(cr);
  }
}

static int dt_brush_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                    dt_masks_form_t *form, int *width, int *height, int *posx, int *posy)
{
  if(!module) return 0;
  // we get buffers for all points
  float *points = NULL, *border = NULL;
  int points_count, border_count;
  if(!_brush_get_points_border(module->dev, form, module->priority, piece->pipe, &points, &points_count,
                               &border, &border_count, NULL, NULL, 1))
  {
    free(points);
    free(border);
    return 0;
  }

  // now we want to find the area, so we search min/max points
  float xmin, xmax, ymin, ymax;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  guint nb_corner = g_list_length(form->points);
  for(int i = nb_corner * 3; i < border_count; i++)
  {
    // we look at the borders
    float xx = border[i * 2];
    float yy = border[i * 2 + 1];
    xmin = fminf(xx, xmin);
    xmax = fmaxf(xx, xmax);
    ymin = fminf(yy, ymin);
    ymax = fmaxf(yy, ymax);
  }
  for(int i = nb_corner * 3; i < points_count; i++)
  {
    // we look at the brush too
    float xx = points[i * 2];
    float yy = points[i * 2 + 1];
    xmin = fminf(xx, xmin);
    xmax = fmaxf(xx, xmax);
    ymin = fminf(yy, ymin);
    ymax = fmaxf(yy, ymax);
  }

  free(points);
  free(border);
  *height = ymax - ymin + 4;
  *width = xmax - xmin + 4;
  *posx = xmin - 2;
  *posy = ymin - 2;
  return 1;
}

static int dt_brush_get_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                             int *width, int *height, int *posx, int *posy)
{
  if(!module) return 0;
  // we get buffers for all points
  float *points = NULL, *border = NULL;
  int points_count, border_count;
  if(!_brush_get_points_border(module->dev, form, module->priority, piece->pipe, &points, &points_count,
                               &border, &border_count, NULL, NULL, 0))
  {
    free(points);
    free(border);
    return 0;
  }

  // now we want to find the area, so we search min/max points
  float xmin, xmax, ymin, ymax;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  guint nb_corner = g_list_length(form->points);
  for(int i = nb_corner * 3; i < border_count; i++)
  {
    // we look at the borders
    float xx = border[i * 2];
    float yy = border[i * 2 + 1];
    xmin = fminf(xx, xmin);
    xmax = fmaxf(xx, xmax);
    ymin = fminf(yy, ymin);
    ymax = fmaxf(yy, ymax);
  }
  for(int i = nb_corner * 3; i < points_count; i++)
  {
    // we look at the brush too
    float xx = points[i * 2];
    float yy = points[i * 2 + 1];
    xmin = fminf(xx, xmin);
    xmax = fmaxf(xx, xmax);
    ymin = fminf(yy, ymin);
    ymax = fmaxf(yy, ymax);
  }

  free(points);
  free(border);

  *height = ymax - ymin + 4;
  *width = xmax - xmin + 4;
  *posx = xmin - 2;
  *posy = ymin - 2;
  return 1;
}

/** we write a falloff segment */
static void _brush_falloff(float **buffer, int *p0, int *p1, int posx, int posy, int bw, float hardness,
                           float density)
{
  // segment length
  const int l = sqrt((p1[0] - p0[0]) * (p1[0] - p0[0]) + (p1[1] - p0[1]) * (p1[1] - p0[1])) + 1;
  const int solid = (int)l * hardness;
  const int soft = l - solid;

  const float lx = p1[0] - p0[0];
  const float ly = p1[1] - p0[1];

  for(int i = 0; i < l; i++)
  {
    // position
    const int x = (int)((float)i * lx / (float)l) + p0[0] - posx;
    const int y = (int)((float)i * ly / (float)l) + p0[1] - posy;
    const float op = density * ((i <= solid) ? 1.0f : 1.0 - (float)(i - solid) / (float)soft);
    (*buffer)[y * bw + x] = fmaxf((*buffer)[y * bw + x], op);
    if(x > 0)
      (*buffer)[y * bw + x - 1]
          = fmaxf((*buffer)[y * bw + x - 1], op); // this one is to avoid gap due to int rounding
    if(y > 0)
      (*buffer)[(y - 1) * bw + x]
          = fmaxf((*buffer)[(y - 1) * bw + x], op); // this one is to avoid gap due to int rounding
  }
}

static int dt_brush_get_mask(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                             float **buffer, int *width, int *height, int *posx, int *posy)
{
  if(!module) return 0;
  double start = dt_get_wtime();
  double start2;

  // we get buffers for all points
  float *points = NULL, *border = NULL, *payload = NULL;
  int points_count, border_count, payload_count;
  if(!_brush_get_points_border(module->dev, form, module->priority, piece->pipe, &points, &points_count,
                               &border, &border_count, &payload, &payload_count, 0))
  {
    free(points);
    free(border);
    free(payload);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush points took %0.04f sec\n", form->name, dt_get_wtime() - start);
  start = start2 = dt_get_wtime();

  // now we want to find the area, so we search min/max points
  float xmin, xmax, ymin, ymax;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  guint nb_corner = g_list_length(form->points);
  for(int i = nb_corner * 3; i < border_count; i++)
  {
    // we look at the borders
    float xx = border[i * 2];
    float yy = border[i * 2 + 1];
    xmin = fminf(xx, xmin);
    xmax = fmaxf(xx, xmax);
    ymin = fminf(yy, ymin);
    ymax = fmaxf(yy, ymax);
  }
  for(int i = nb_corner * 3; i < points_count; i++)
  {
    // we look at the brush too
    float xx = points[i * 2];
    float yy = points[i * 2 + 1];
    xmin = fminf(xx, xmin);
    xmax = fmaxf(xx, xmax);
    ymin = fminf(yy, ymin);
    ymax = fmaxf(yy, ymax);
  }

  *height = ymax - ymin + 4;
  *width = xmax - xmin + 4;
  *posx = xmin - 2;
  *posy = ymin - 2;

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush_fill min max took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
//   start2 = dt_get_wtime();

  // we allocate the buffer
  *buffer = calloc((size_t)(*width) * (*height), sizeof(float));

  // now we fill the falloff
  int p0[2], p1[2];

  for(int i = nb_corner * 3; i < border_count; i++)
  {
    p0[0] = points[i * 2];
    p0[1] = points[i * 2 + 1];
    p1[0] = border[i * 2];
    p1[1] = border[i * 2 + 1];

    _brush_falloff(buffer, p0, p1, *posx, *posy, *width, payload[i * 2], payload[i * 2 + 1]);
  }

  free(points);
  free(border);
  free(payload);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush fill buffer took %0.04f sec\n", form->name,
             dt_get_wtime() - start);

  return 1;
}

/** we write a falloff segment respecting limits of buffer */
static inline void _brush_falloff_roi(float *buffer, int *p0, int *p1, int bw, int bh, float hardness,
                                      float density)
{
  // segment length (increase by 1 to avoid division-by-zero special case handling)
  const int l = sqrt((p1[0] - p0[0]) * (p1[0] - p0[0]) + (p1[1] - p0[1]) * (p1[1] - p0[1])) + 1;
  const int solid = hardness * l;

  const float lx = (float)(p1[0] - p0[0]) / (float)l;
  const float ly = (float)(p1[1] - p0[1]) / (float)l;

  const int dx = lx <= 0 ? -1 : 1;
  const int dy = ly <= 0 ? -1 : 1;
  const int dpx = dx;
  const int dpy = dy * bw;

  float fx = p0[0];
  float fy = p0[1];

  float op = density;
  float dop = density / (float)(l - solid);

  for(int i = 0; i < l; i++)
  {
    const int x = fx;
    const int y = fy;

    fx += lx;
    fy += ly;
    if(i > solid) op -= dop;

    if(x < 0 || x >= bw || y < 0 || y >= bh) continue;

    float *buf = buffer + (size_t)y * bw + x;

    *buf = fmaxf(*buf, op);
    if(x + dx >= 0 && x + dx < bw)
      buf[dpx] = fmaxf(buf[dpx], op); // this one is to avoid gaps due to int rounding
    if(y + dy >= 0 && y + dy < bh)
      buf[dpy] = fmaxf(buf[dpy], op); // this one is to avoid gaps due to int rounding
  }
}

static int dt_brush_get_mask_roi(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                 dt_masks_form_t *form, const dt_iop_roi_t *roi, float *buffer)
{
  if(!module) return 0;
  double start = dt_get_wtime();
  double start2;

  const int px = roi->x;
  const int py = roi->y;
  const int width = roi->width;
  const int height = roi->height;
  const float scale = roi->scale;

  // we get buffers for all points
  float *points = NULL, *border = NULL, *payload = NULL;

  int points_count, border_count, payload_count;
  if(!_brush_get_points_border(module->dev, form, module->priority, piece->pipe, &points, &points_count,
                               &border, &border_count, &payload, &payload_count, 0))
  {
    free(points);
    free(border);
    free(payload);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush points took %0.04f sec\n", form->name, dt_get_wtime() - start);
  start = start2 = dt_get_wtime();

  // empty the output buffer
  memset(buffer, 0, (size_t)width * height * sizeof(float));

  guint nb_corner = g_list_length(form->points);

  // we shift and scale down brush and border
  for(int i = nb_corner * 3; i < border_count; i++)
  {
    float xx = border[2 * i];
    float yy = border[2 * i + 1];
    border[2 * i] = xx * scale - px;
    border[2 * i + 1] = yy * scale - py;
  }

  for(int i = nb_corner * 3; i < points_count; i++)
  {
    float xx = points[2 * i];
    float yy = points[2 * i + 1];
    points[2 * i] = xx * scale - px;
    points[2 * i + 1] = yy * scale - py;
  }

  // now we want to find the area, so we search min/max points
  float xmin, xmax, ymin, ymax;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;

  for(int i = nb_corner * 3; i < border_count; i++)
  {
    // we look at the borders
    float xx = border[i * 2];
    float yy = border[i * 2 + 1];
    xmin = fminf(xx, xmin);
    xmax = fmaxf(xx, xmax);
    ymin = fminf(yy, ymin);
    ymax = fmaxf(yy, ymax);
  }
  for(int i = nb_corner * 3; i < points_count; i++)
  {
    // we look at the brush too
    float xx = points[i * 2];
    float yy = points[i * 2 + 1];
    xmin = fminf(xx, xmin);
    xmax = fmaxf(xx, xmax);
    ymin = fminf(yy, ymin);
    ymax = fmaxf(yy, ymax);
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush_fill min max took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
//   start2 = dt_get_wtime();

  // check if the path completely lies outside of roi -> we're done/mask remains empty
  if(xmax < 0 || ymax < 0 || xmin >= width || ymin >= height)
  {
    free(points);
    free(border);
    free(payload);
    return 1;
  }

  // now we fill the falloff
  int p0[2], p1[2];
  for(int i = nb_corner * 3; i < border_count; i++)
  {
    p0[0] = points[i * 2];
    p0[1] = points[i * 2 + 1];
    p1[0] = border[i * 2];
    p1[1] = border[i * 2 + 1];

    if(MAX(p0[0], p1[0]) < 0 || MIN(p0[0], p1[0]) >= width || MAX(p0[1], p1[1]) < 0
       || MIN(p0[1], p1[1]) >= height)
      continue;

    _brush_falloff_roi(buffer, p0, p1, width, height, payload[i * 2], payload[i * 2 + 1]);
  }

  free(points);
  free(border);
  free(payload);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] brush fill buffer took %0.04f sec\n", form->name,
             dt_get_wtime() - start);

  return 1;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
