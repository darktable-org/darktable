/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson.
    copyright (c) 2012 aldric renaudin.

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
#include "develop/imageop.h"
#include "develop/blend.h"
#include "control/control.h"
#include "control/conf.h"
#include "develop/masks.h"
#include "common/debug.h"

/** a poor man's memory management: just a sloppy monitoring of buffer usage with automatic reallocation */
static gboolean _path_buffer_grow(float **buffer, int *buffer_count, int *buffer_max)
{
  const int stepsize = 1000000;
  const int reserve = 100000;

  // printf("buffer %p, buffer_count %d, buffer_max %d\n", *buffer, *buffer_count, *buffer_max);

  if(*buffer == NULL)
  {
    *buffer = calloc(stepsize, sizeof(float));
    *buffer_count = 0;
    *buffer_max = stepsize;
    return (*buffer != NULL);
  }

  if(*buffer_count > *buffer_max)
  {
    fprintf(stderr, "_path_buffer_grow: memory size exceeded and detected too late :(\n");
  }

  if(*buffer_count + reserve > *buffer_max)
  {
    float *oldbuffer = *buffer;
    *buffer_max += stepsize;
    *buffer = calloc(*buffer_max, sizeof(float));
    if(*buffer == NULL)
    {
      free(oldbuffer);
      return FALSE;
    }
    memcpy(*buffer, oldbuffer, *buffer_count * sizeof(float));
    free(oldbuffer);
  }

  return TRUE;
}


/** get the point of the path at pos t [0,1]  */
static void _path_get_XY(float p0x, float p0y, float p1x, float p1y, float p2x, float p2y, float p3x,
                         float p3y, float t, float *x, float *y)
{
  float a = (1 - t) * (1 - t) * (1 - t);
  float b = 3 * t * (1 - t) * (1 - t);
  float c = 3 * t * t * (1 - t);
  float d = t * t * t;
  *x = p0x * a + p1x * b + p2x * c + p3x * d;
  *y = p0y * a + p1y * b + p2y * c + p3y * d;
}

/** get the point of the path at pos t [0,1]  AND the corresponding border point */
static void _path_border_get_XY(float p0x, float p0y, float p1x, float p1y, float p2x, float p2y, float p3x,
                                float p3y, float t, float rad, float *xc, float *yc, float *xb, float *yb)
{
  // we get the point
  _path_get_XY(p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y, t, xc, yc);

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
    *xb = -9999999;
    *yb = -9999999;
    return;
  }
  float l = 1.0 / sqrtf(dx * dx + dy * dy);
  *xb = (*xc) + rad * dy * l;
  *yb = (*yc) - rad * dx * l;
}

/** get feather extremity from the control point n°2 */
/** the values should be in orthonormal space */
static void _path_ctrl2_to_feather(int ptx, int pty, int ctrlx, int ctrly, int *fx, int *fy,
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
static void _path_feather_to_ctrl(int ptx, int pty, int fx, int fy, int *ctrl1x, int *ctrl1y, int *ctrl2x,
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
  if(g_list_length(form->points) < 2) return;

  guint nb = g_list_length(form->points);
  for(int k = 0; k < nb; k++)
  {
    dt_masks_point_path_t *point3 = (dt_masks_point_path_t *)g_list_nth_data(form->points, k);
    // if the point as not be set manually, we redfine it
    if(point3->state & DT_MASKS_POINT_STATE_NORMAL)
    {
      // we want to get point-2, point-1, point+1, point+2
      int k1, k2, k4, k5;
      k1 = (k - 2) < 0 ? nb + (k - 2) : k - 2;
      k2 = (k - 1) < 0 ? nb - 1 : k - 1;
      k4 = (k + 1) % nb;
      k5 = (k + 2) % nb;
      dt_masks_point_path_t *point1 = (dt_masks_point_path_t *)g_list_nth_data(form->points, k1);
      dt_masks_point_path_t *point2 = (dt_masks_point_path_t *)g_list_nth_data(form->points, k2);
      dt_masks_point_path_t *point4 = (dt_masks_point_path_t *)g_list_nth_data(form->points, k4);
      dt_masks_point_path_t *point5 = (dt_masks_point_path_t *)g_list_nth_data(form->points, k5);

      float bx1, by1, bx2, by2;
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
  }
}

static gboolean _path_is_clockwise(dt_masks_form_t *form)
{
  if(g_list_length(form->points) > 2)
  {
    float sum = 0.0f;
    guint nb = g_list_length(form->points);
    for(int k = 0; k < nb; k++)
    {
      int k2 = (k + 1) % nb;
      dt_masks_point_path_t *point1 = (dt_masks_point_path_t *)g_list_nth_data(form->points, k);
      dt_masks_point_path_t *point2 = (dt_masks_point_path_t *)g_list_nth_data(form->points, k2);
      // edge k
      sum += (point2->corner[0] - point1->corner[0]) * (point2->corner[1] + point1->corner[1]);
    }
    return (sum < 0);
  }
  // return dummy answer
  return TRUE;
}

/** fill eventual gaps between 2 points with a line */
static int _path_fill_gaps(int lastx, int lasty, int x, int y, float *points, int *pts_count)
{
  points[0] = x;
  points[1] = y;
  int points_count = *pts_count = 1;
  if(lastx == -999999) return 1;
  // now we want to be sure everything is continuous
  if(points[0] - lastx > 1)
  {
    for(int j = points[0] - 1; j > lastx; j--)
    {
      int yyy = (j - lastx) * (points[1] - lasty) / (float)(points[0] - lastx) + lasty;
      int lasty2 = points[(points_count - 1) * 2 + 1];
      if(lasty2 - yyy > 1)
      {
        for(int jj = lasty2 + 1; jj < yyy; jj++)
        {
          points[points_count * 2] = j;
          points[points_count * 2 + 1] = jj;
          points_count++;
        }
      }
      else if(lasty2 - yyy < -1)
      {
        for(int jj = lasty2 - 1; jj > yyy; jj--)
        {
          points[points_count * 2] = j;
          points[points_count * 2 + 1] = jj;
          points_count++;
        }
      }
      points[points_count * 2] = j;
      points[points_count * 2 + 1] = yyy;
      points_count++;
    }
  }
  else if(points[0] - lastx < -1)
  {
    for(int j = points[0] + 1; j < lastx; j++)
    {
      int yyy = (j - lastx) * (points[1] - lasty) / (float)(points[0] - lastx) + lasty;
      int lasty2 = points[(points_count - 1) * 2 + 1];
      if(lasty2 - yyy > 1)
      {
        for(int jj = lasty2 + 1; jj < yyy; jj++)
        {
          points[points_count * 2] = j;
          points[points_count * 2 + 1] = jj;
          points_count++;
        }
      }
      else if(lasty2 - yyy < -1)
      {
        for(int jj = lasty2 - 1; jj > yyy; jj--)
        {
          points[points_count * 2] = j;
          points[points_count * 2 + 1] = jj;
          points_count++;
        }
      }
      points[points_count * 2] = j;
      points[points_count * 2 + 1] = yyy;
      points_count++;
    }
  }
  *pts_count = points_count;
  return 1;
}

/** fill the gap between 2 points with an arc of circle */
/** this function is here because we can have gap in border, esp. if the corner is very sharp */
static void _path_points_recurs_border_gaps(float *cmax, float *bmin, float *bmin2, float *bmax, float *path,
                                            int *pos_path, float *border, int *pos_border, gboolean clockwise)
{
  // we want to find the start and end angles
  double a1 = atan2(bmin[1] - cmax[1], bmin[0] - cmax[0]);
  double a2 = atan2(bmax[1] - cmax[1], bmax[0] - cmax[0]);
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
    path[*pos_path] = cmax[0];
    path[*pos_path + 1] = cmax[1];
    *pos_path += 2;
    border[*pos_border] = cmax[0] + rr * cosf(aa);
    border[*pos_border + 1] = cmax[1] + rr * sinf(aa);
    *pos_border += 2;
    rr += incrr;
    aa += incra;
  }
}

/** recursive function to get all points of the path AND all point of the border */
/** the function take care to avoid big gaps between points */
static void _path_points_recurs(float *p1, float *p2, double tmin, double tmax, float *path_min,
                                float *path_max, float *border_min, float *border_max, float *rpath,
                                float *rborder, float *path, float *border, int *pos_path, int *pos_border,
                                int withborder)
{
  // we calculate points if needed
  if(path_min[0] == -99999)
  {
    _path_border_get_XY(p1[0], p1[1], p1[2], p1[3], p2[2], p2[3], p2[0], p2[1], tmin,
                        p1[4] + (p2[4] - p1[4]) * tmin * tmin * (3.0 - 2.0 * tmin), path_min, path_min + 1,
                        border_min, border_min + 1);
  }
  if(path_max[0] == -99999)
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
    path[*pos_path] = path_max[0];
    path[*pos_path + 1] = path_max[1];
    if(withborder)
    {
      rborder[0] = border[*pos_border] = border_max[0];
      rborder[1] = border[*pos_border + 1] = border_max[1];
      *pos_border += 2;
    }
    *pos_path += 2;
    rpath[0] = path_max[0];
    rpath[1] = path_max[1];
    return;
  }

  // we split in two part
  double tx = (tmin + tmax) / 2.0;
  float c[2] = { -99999, -99999 }, b[2] = { -99999, -99999 };
  float rc[2], rb[2];
  _path_points_recurs(p1, p2, tmin, tx, path_min, c, border_min, b, rc, rb, path, border, pos_path,
                      pos_border, withborder);
  _path_points_recurs(p1, p2, tx, tmax, rc, path_max, rb, border_max, rpath, rborder, path, border, pos_path,
                      pos_border, withborder);
}

/** find all self intersections in a path */
static int _path_find_self_intersection(int *inter, int nb_corners, float *border, int border_count)
{
  int inter_count = 0;

  // we search extreme points in x and y
  int xmin, xmax, ymin, ymax;
  xmin = ymin = INT_MAX;
  xmax = ymax = INT_MIN;
  int posextr[4] = { -1 }; // xmin,xmax,ymin,ymax

  for(int i = nb_corners * 3; i < border_count; i++)
  {
    if(border[i * 2] < -999999 || border[i * 2 + 1] < -999999)
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
  const int ss = hb * wb;
  if(ss < 10) return 0;

  int *binter = calloc(ss, sizeof(int));
  if(binter == NULL) return 0;

  int lastx = border[(posextr[1] - 1) * 2];
  int lasty = border[(posextr[1] - 1) * 2 + 1];

  float *extra = NULL;
  int extra_max = 0;
  int extra_count = 0;
  int extrap = 0;

  if(!_path_buffer_grow(&extra, &extrap, &extra_max))
  {
    free(binter);
    return 0;
  }

  for(int ii = nb_corners * 3; ii < border_count; ii++)
  {
    // we want to loop from one border extremity
    int i = ii - nb_corners * 3 + posextr[1];
    if(i >= border_count) i = i - border_count + nb_corners * 3;

    if(inter_count >= nb_corners * 4) break;
    // we want to be sure everything is continuous
    _path_fill_gaps(lastx, lasty, border[i * 2], border[i * 2 + 1], extra, &extra_count);

    extrap = 2 * extra_count;

    if(!_path_buffer_grow(&extra, &extrap, &extra_max))
    {
      free(binter);
      return 0;
    }

    // we now search intersections for all the point in extra
    for(int j = extra_count - 1; j >= 0; j--)
    {
      int xx = extra[j * 2];
      int yy = extra[j * 2 + 1];
      int v[3] = { 0 };
      v[0] = binter[(yy - ymin) * wb + (xx - xmin)];
      if(xx > xmin) v[1] = binter[(yy - ymin) * wb + (xx - xmin - 1)];
      if(yy > ymin) v[2] = binter[(yy - ymin - 1) * wb + (xx - xmin)];
      for(int k = 0; k < 3; k++)
      {
        if(v[k] > 0)
        {
          if((xx == lastx && yy == lasty) || v[k] == i - 1)
          {
            binter[(yy - ymin) * wb + (xx - xmin)] = i;
          }
          else if((i > v[k]
                   && ((posextr[0] < i || posextr[0] > v[k]) && (posextr[1] < i || posextr[1] > v[k])
                       && (posextr[2] < i || posextr[2] > v[k]) && (posextr[3] < i || posextr[3] > v[k])))
                  || (i < v[k] && posextr[0] < v[k] && posextr[0] > i && posextr[1] < v[k] && posextr[1] > i
                      && posextr[2] < v[k] && posextr[2] > i && posextr[3] < v[k] && posextr[3] > i))
          {
            if(inter_count > 0)
            {
              if((v[k] - i) * (inter[inter_count * 2 - 2] - inter[inter_count * 2 - 1]) > 0
                 && inter[inter_count * 2 - 2] >= v[k] && inter[inter_count * 2 - 1] <= i)
              {
                inter[inter_count * 2 - 2] = v[k];
                inter[inter_count * 2 - 1] = i;
              }
              else
              {
                inter[inter_count * 2] = v[k];
                inter[inter_count * 2 + 1] = i;
                inter_count++;
              }
            }
            else
            {
              inter[inter_count * 2] = v[k];
              inter[inter_count * 2 + 1] = i;
              inter_count++;
            }
          }
        }
        else
        {
          binter[(yy - ymin) * wb + (xx - xmin)] = i;
        }
      }
      lastx = xx;
      lasty = yy;
    }
  }

  free(extra);
  free(binter);

  // and we return the number of self-intersection found
  return inter_count;
}

/** get all points of the path and the border */
/** this take care of gaps and self-intersection and iop distortions */
static int _path_get_points_border(dt_develop_t *dev, dt_masks_form_t *form, int prio_max,
                                   dt_dev_pixelpipe_t *pipe, float **points, int *points_count,
                                   float **border, int *border_count, int source)
{
  double start2 = dt_get_wtime();

  float wd = pipe->iwidth, ht = pipe->iheight;

  int points_max, pos;
  int border_max, posb;

  *points = NULL;
  points_max = 0;
  pos = 0;
  if(!_path_buffer_grow(points, &pos, &points_max)) return 0;

  if(border)
  {
    *border = NULL;
    border_max = 0;
    posb = 0;
    if(!_path_buffer_grow(border, &posb, &border_max)) return 0;
  }

  // we store all points
  float dx, dy;
  dx = dy = 0.0f;
  guint nb = g_list_length(form->points);
  if(source && nb > 0)
  {
    dt_masks_point_path_t *pt = (dt_masks_point_path_t *)g_list_nth_data(form->points, 0);
    dx = (pt->corner[0] - form->source[0]) * wd;
    dy = (pt->corner[1] - form->source[1]) * ht;
  }
  for(int k = 0; k < nb; k++)
  {
    dt_masks_point_path_t *pt = (dt_masks_point_path_t *)g_list_nth_data(form->points, k);
    (*points)[k * 6] = pt->ctrl1[0] * wd - dx;
    (*points)[k * 6 + 1] = pt->ctrl1[1] * ht - dy;
    (*points)[k * 6 + 2] = pt->corner[0] * wd - dx;
    (*points)[k * 6 + 3] = pt->corner[1] * ht - dy;
    (*points)[k * 6 + 4] = pt->ctrl2[0] * wd - dx;
    (*points)[k * 6 + 5] = pt->ctrl2[1] * ht - dy;
  }
  // for the border, we store value too
  if(border)
  {
    for(int k = 0; k < nb; k++)
    {
      (*border)[k * 6] = 0.0; // x position of the border point
      (*border)[k * 6 + 1] = 0.0; // y position of the border point
      (*border)[k * 6 + 2]
          = 0.0; // start index for the initial gap. if <0 this mean we have to skip to index (-x)
      (*border)[k * 6 + 3] = 0.0; // end index for the initial gap
      (*border)[k * 6 + 4]
          = 0.0; // start index for the final gap. if <0 this mean we have to stop at index (-x)
      (*border)[k * 6 + 5] = 0.0; // end index for the final gap
    }
  }

  pos = 6 * nb;
  posb = 6 * nb;
  float border_init[6 * nb];
  int cw = _path_is_clockwise(form);
  if(cw == 0) cw = -1;

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] path_points init took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we render all segments
  for(int k = 0; k < nb; k++)
  {
    int pb = posb;
    border_init[k * 6 + 2] = -posb;
    int k2 = (k + 1) % nb;
    int k3 = (k + 2) % nb;
    dt_masks_point_path_t *point1 = (dt_masks_point_path_t *)g_list_nth_data(form->points, k);
    dt_masks_point_path_t *point2 = (dt_masks_point_path_t *)g_list_nth_data(form->points, k2);
    dt_masks_point_path_t *point3 = (dt_masks_point_path_t *)g_list_nth_data(form->points, k3);
    float p1[5] = { point1->corner[0] * wd - dx, point1->corner[1] * ht - dy, point1->ctrl2[0] * wd - dx,
                    point1->ctrl2[1] * ht - dy, cw * point1->border[1] * MIN(wd, ht) };
    float p2[5] = { point2->corner[0] * wd - dx, point2->corner[1] * ht - dy, point2->ctrl1[0] * wd - dx,
                    point2->ctrl1[1] * ht - dy, cw * point2->border[0] * MIN(wd, ht) };
    float p3[5] = { point2->corner[0] * wd - dx, point2->corner[1] * ht - dy, point2->ctrl2[0] * wd - dx,
                    point2->ctrl2[1] * ht - dy, cw * point2->border[1] * MIN(wd, ht) };
    float p4[5] = { point3->corner[0] * wd - dx, point3->corner[1] * ht - dy, point3->ctrl1[0] * wd - dx,
                    point3->ctrl1[1] * ht - dy, cw * point3->border[0] * MIN(wd, ht) };

    // and we determine all points by recursion (to be sure the distance between 2 points is <=1)
    float rc[2], rb[2];
    float bmin[2] = { -99999, -99999 };
    float bmax[2] = { -99999, -99999 };
    float cmin[2] = { -99999, -99999 };
    float cmax[2] = { -99999, -99999 };
    if(border)
    {
      _path_points_recurs(p1, p2, 0.0, 1.0, cmin, cmax, bmin, bmax, rc, rb, *points, *border, &pos, &posb,
                          (nb >= 3));
      if(!_path_buffer_grow(points, &pos, &points_max)) return 0;

      if(!_path_buffer_grow(border, &posb, &border_max)) return 0;
    }
    else
    {
      _path_points_recurs(p1, p2, 0.0, 1.0, cmin, cmax, bmin, bmax, rc, rb, *points, NULL, &pos, &posb, FALSE);
      if(!_path_buffer_grow(points, &pos, &points_max)) return 0;
    }


    // we check gaps in the border (sharp edges)
    if(border && ((*border)[posb - 2] - rb[0] > 1 || (*border)[posb - 2] - rb[0] < -1
                  || (*border)[posb - 1] - rb[1] > 1 || (*border)[posb - 1] - rb[1] < -1))
    {
      bmin[0] = (*border)[posb - 2];
      bmin[1] = (*border)[posb - 1];
      //_path_points_recurs_border_gaps(rc,bmin,rb,*points,&pos,*border,&posb);
    }
    (*points)[pos++] = rc[0];
    (*points)[pos++] = rc[1];
    border_init[k * 6 + 4] = -posb;


    if(border)
    {

      if(rb[0] == -9999999.0f)
      {
        if((*border)[posb - 2] == -9999999.0f)
        {
          (*border)[posb - 2] = (*border)[posb - 4];
          (*border)[posb - 1] = (*border)[posb - 3];
        }
        rb[0] = (*border)[posb - 2];
        rb[1] = (*border)[posb - 1];
      }
      (*border)[posb++] = rb[0];
      (*border)[posb++] = rb[1];

      (*border)[k * 6] = border_init[k * 6] = (*border)[pb];
      (*border)[k * 6 + 1] = border_init[k * 6 + 1] = (*border)[pb + 1];

      if(!_path_buffer_grow(border, &posb, &border_max)) return 0;
    }

    // we first want to be sure that there are no gaps in border
    if(border && nb >= 3)
    {
      // we get the next point (start of the next segment)
      _path_border_get_XY(p3[0], p3[1], p3[2], p3[3], p4[2], p4[3], p4[0], p4[1], 0, p3[4], cmin, cmin + 1,
                          bmax, bmax + 1);
      if(bmax[0] == -9999999.0f)
      {
        _path_border_get_XY(p3[0], p3[1], p3[2], p3[3], p4[2], p4[3], p4[0], p4[1], 0.0001, p3[4], cmin,
                            cmin + 1, bmax, bmax + 1);
      }
      if(bmax[0] - rb[0] > 1 || bmax[0] - rb[0] < -1 || bmax[1] - rb[1] > 1 || bmax[1] - rb[1] < -1)
      {
        float bmin2[2] = { (*border)[posb - 22], (*border)[posb - 21] };
        _path_points_recurs_border_gaps(rc, rb, bmin2, bmax, *points, &pos, *border, &posb,
                                        _path_is_clockwise(form));
        if(!_path_buffer_grow(points, &pos, &points_max)) return 0;

        if(!_path_buffer_grow(border, &posb, &border_max)) return 0;
      }
    }
  }
  *points_count = pos / 2;
  if(border) *border_count = posb / 2;

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] path_points point recurs %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we don't want the border to self-intersect
  int intersections[nb * 8];
  int inter_count = 0;
  if(border)
  {
    inter_count = _path_find_self_intersection(intersections, nb, *border, *border_count);

    if(darktable.unmuted & DT_DEBUG_PERF)
      dt_print(DT_DEBUG_MASKS, "[masks %s] path_points self-intersect took %0.04f sec\n", form->name,
               dt_get_wtime() - start2);
    start2 = dt_get_wtime();
  }

  // and we transform them with all distorted modules
  if(dt_dev_distort_transform_plus(dev, pipe, 0, prio_max, *points, *points_count))
  {
    if(!border || dt_dev_distort_transform_plus(dev, pipe, 0, prio_max, *border, *border_count))
    {
      if(darktable.unmuted & DT_DEBUG_PERF)
        dt_print(DT_DEBUG_MASKS, "[masks %s] path_points transform took %0.04f sec\n", form->name,
                 dt_get_wtime() - start2);
      start2 = dt_get_wtime();

      // we don't want to copy the falloff points
      if(border)
        for(int k = 0; k < nb; k++)
          for(int i = 2; i < 6; i++) (*border)[k * 6 + i] = border_init[k * 6 + i];
      // now we want to write the skipping zones
      for(int i = 0; i < inter_count; i++)
      {
        int v = intersections[i * 2];
        int w = intersections[i * 2 + 1];
        if(v <= w)
        {
          (*border)[v * 2] = -999999;
          (*border)[v * 2 + 1] = w;
        }
        else
        {
          if(w > nb * 3)
          {
            if((*border)[nb * 6] == -999999)
              (*border)[nb * 6 + 1] = MAX((*border)[nb * 6 + 1], w);
            else
              (*border)[nb * 6 + 1] = w;
            (*border)[nb * 6] = -999999;
          }
          (*border)[v * 2] = -999999;
          (*border)[v * 2 + 1] = -999999;
        }
      }

      if(darktable.unmuted & DT_DEBUG_PERF)
        dt_print(DT_DEBUG_MASKS, "[masks %s] path_points end took %0.04f sec\n", form->name,
                 dt_get_wtime() - start2);
      start2 = dt_get_wtime();
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
  return 0;
}

/** get the distance between point (x,y) and the path */
static void dt_path_get_distance(float x, int y, float as, dt_masks_form_gui_t *gui, int index,
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
  if(dt_masks_point_in_form_exact(x,yf,gpt->source,corner_count * 6,gpt->source_count))
  {
    *inside_source = 1;
    *inside = 1;
    return;
  }

  // we check if it's inside borders
  if(!dt_masks_point_in_form_exact(x,yf,gpt->border,corner_count * 3,gpt->border_count)) return;

  *inside = 1;

  // and we check if it's inside form
  if(gpt->points_count > 2 + corner_count * 3)
  {
    float as2 = as * as;
    //float as2 = 1600.0 * as1;
    float last = gpt->points[gpt->points_count * 2 - 1];
    int nb = 0;
    int near_form = 0;
    int current_seg = 1;
    for(int i = corner_count * 3; i < gpt->points_count; i++)
    {
      //if we need to jump to skip points (in case of deleted point, because of self-intersection)
      if(gpt->points[i * 2] == -999999.0)
      {
        if(gpt->points[i * 2 + 1] == -999999.0) break;
        i = (int)gpt->points[i * 2 + 1] - 1;
        continue;
      }
      // do we change of path segment ?
      if(gpt->points[i * 2 + 1] == gpt->points[current_seg * 6 + 3] && gpt->points[i * 2] == gpt->points[current_seg * 6 + 2])
      {
        current_seg = (current_seg + 1) % corner_count;
      }
      //distance from tested point to current form point
      float yy = gpt->points[i * 2 + 1];
      float dd = (gpt->points[i * 2] - x) * (gpt->points[i * 2] - x)
                  + (yy - yf) * (yy - yf);

      if(dd < as2)
      {
        near_form = 1;
        if(current_seg == 0)
          *near = corner_count - 1;
        else
          *near = current_seg - 1;
      }

      if (((yf<=yy && yf>last) || (yf>=yy && yf<last)) && (gpt->points[i * 2] > x)) nb++;

      last = yy;
    }
    *inside_border = !((nb & 1) || (near_form));
  }
  else *inside_border = 1;
}

static int dt_path_get_points_border(dt_develop_t *dev, dt_masks_form_t *form, float **points,
                                     int *points_count, float **border, int *border_count, int source)
{
  return _path_get_points_border(dev, form, 999999, dev->preview_pipe, points, points_count, border,
                                 border_count, source);
}

static int dt_path_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up,
                                         uint32_t state, dt_masks_form_t *form, int parentid,
                                         dt_masks_form_gui_t *gui, int index)
{
  if(gui->form_selected)
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
      float amount = 1.03;
      if(up) amount = 0.97;
      guint nb = g_list_length(form->points);
      if(gui->border_selected)
      {
        for(int k = 0; k < nb; k++)
        {
          dt_masks_point_path_t *point = (dt_masks_point_path_t *)g_list_nth_data(form->points, k);
          point->border[0] *= amount;
          point->border[1] *= amount;
        }
        if(form->type & DT_MASKS_CLONE)
        {
          const float masks_border = dt_conf_get_float("plugins/darkroom/spots/path_border");
          dt_conf_set_float("plugins/darkroom/spots/path_border", masks_border * amount);
        }
        else
        {
          const float masks_border = dt_conf_get_float("plugins/darkroom/masks/path/border");
          dt_conf_set_float("plugins/darkroom/masks/path/border", masks_border * amount);
        }
      }
      else if(gui->edit_mode == DT_MASKS_EDIT_FULL)
      {
        // get the center of gravity of the form (like if it was a simple polygon)
        float bx = 0.0f;
        float by = 0.0f;
        float surf = 0.0f;

        for(int k = 0; k < nb; k++)
        {
          int k2 = (k + 1) % nb;
          dt_masks_point_path_t *point1 = (dt_masks_point_path_t *)g_list_nth_data(form->points, k);
          dt_masks_point_path_t *point2 = (dt_masks_point_path_t *)g_list_nth_data(form->points, k2);
          surf += point1->corner[0] * point2->corner[1] - point2->corner[0] * point1->corner[1];

          bx += (point1->corner[0] + point2->corner[0])
                * (point1->corner[0] * point2->corner[1] - point2->corner[0] * point1->corner[1]);
          by += (point1->corner[1] + point2->corner[1])
                * (point1->corner[0] * point2->corner[1] - point2->corner[0] * point1->corner[1]);
        }
        bx /= 3.0 * surf;
        by /= 3.0 * surf;

        if(amount < 1.0 && surf < 0.00001 && surf > -0.00001) return 1;

        // now we move each point
        for(int k = 0; k < nb; k++)
        {
          dt_masks_point_path_t *point = (dt_masks_point_path_t *)g_list_nth_data(form->points, k);
          float x = (point->corner[0] - bx) * amount;
          float y = (point->corner[1] - by) * amount;

          // we stretch ctrl points
          float ct1x = (point->ctrl1[0] - point->corner[0]) * amount;
          float ct1y = (point->ctrl1[1] - point->corner[1]) * amount;
          float ct2x = (point->ctrl2[0] - point->corner[0]) * amount;
          float ct2y = (point->ctrl2[1] - point->corner[1]) * amount;

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
      }
      else
      {
        return 0;
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

static int dt_path_events_button_pressed(struct dt_iop_module_t *module, float pzx, float pzy,
                                         double pressure, int which, int type, uint32_t state,
                                         dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui,
                                         int index)
{
  if(type == GDK_2BUTTON_PRESS || type == GDK_3BUTTON_PRESS) return 1;
  if(!gui) return 0;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return 0;

  float masks_border;
  if(form->type & DT_MASKS_CLONE)
    masks_border = MIN(dt_conf_get_float("plugins/darkroom/spots/path_border"), 0.5f);
  else
    masks_border = MIN(dt_conf_get_float("plugins/darkroom/masks/path/border"), 0.5f);

  if(gui->creation && (which == 3 || gui->creation_closing_form))
  {
    // we don't want a form with less than 3 points
    if(g_list_length(form->points) < 4)
    {
      // we remove the form
      dt_masks_free_form(form);
      darktable.develop->form_visible = NULL;
      dt_masks_clear_form_gui(darktable.develop);
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
      dt_masks_gui_form_save_creation(crea_module, form, gui);
      if(crea_module)
      {
        dt_dev_add_history_item(darktable.develop, crea_module, TRUE);
        // and we switch in edit mode to show all the forms
        dt_masks_set_edit_mode(crea_module, DT_MASKS_EDIT_FULL);
        dt_masks_iop_update(crea_module);
        gui->creation_module = NULL;
      }
      else
      {
        dt_dev_masks_selection_change(darktable.develop, form->formid, TRUE);
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
      float wd = darktable.develop->preview_pipe->backbuf_width;
      float ht = darktable.develop->preview_pipe->backbuf_height;
      float pts[2] = { pzx * wd, pzy * ht };
      dt_dev_distort_backtransform(darktable.develop, pts, 1);

      bzpt->corner[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
      bzpt->corner[1] = pts[1] / darktable.develop->preview_pipe->iheight;
      bzpt->ctrl1[0] = bzpt->ctrl1[1] = bzpt->ctrl2[0] = bzpt->ctrl2[1] = -1.0;
      bzpt->state = DT_MASKS_POINT_STATE_NORMAL;

      bzpt->border[0] = bzpt->border[1] = MAX(0.005f, masks_border);

      // if that's the first point we should had another one as base point
      if(nb == 0)
      {
        dt_masks_point_path_t *bzpt2 = (dt_masks_point_path_t *)(malloc(sizeof(dt_masks_point_path_t)));
        bzpt2->corner[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
        bzpt2->corner[1] = pts[1] / darktable.develop->preview_pipe->iheight;
        bzpt2->ctrl1[0] = bzpt2->ctrl1[1] = bzpt2->ctrl2[0] = bzpt2->ctrl2[1] = -1.0;
        bzpt2->border[0] = bzpt2->border[1] = MAX(0.005f, masks_border);
        bzpt2->state = DT_MASKS_POINT_STATE_NORMAL;
        form->points = g_list_append(form->points, bzpt2);
        form->source[0] = bzpt->corner[0] + 0.02f;
        form->source[1] = bzpt->corner[1] + 0.02f;
        nb++;
      }
      form->points = g_list_append(form->points, bzpt);

      // if this is a ctrl click, the last created point is a sharp one
      if((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
      {
        dt_masks_point_path_t *bzpt3 = g_list_nth_data(form->points, nb - 1);
        bzpt3->ctrl1[0] = bzpt3->ctrl2[0] = bzpt3->corner[0];
        bzpt3->ctrl1[1] = bzpt3->ctrl2[1] = bzpt3->corner[1];
        bzpt3->state = DT_MASKS_POINT_STATE_USER;
      }

      gui->point_dragging = nb;

      _path_init_ctrl_points(form);

      // we recreate the form points
      dt_masks_gui_form_remove(form, gui, index);
      dt_masks_gui_form_create(form, gui, index);

      dt_control_queue_redraw_center();
      return 1;
    }
    else if(gui->source_selected && gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
      if(!gpt) return 0;
      // we start the form dragging
      gui->source_dragging = TRUE;
      gui->posx = pzx * darktable.develop->preview_pipe->backbuf_width;
      gui->posy = pzy * darktable.develop->preview_pipe->backbuf_height;
      gui->dx = gpt->source[2] - gui->posx;
      gui->dy = gpt->source[3] - gui->posy;
      return 1;
    }
    else if(gui->form_selected && gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      gui->form_dragging = TRUE;
      gui->point_edited = -1;
      gui->posx = pzx * darktable.develop->preview_pipe->backbuf_width;
      gui->posy = pzy * darktable.develop->preview_pipe->backbuf_height;
      gui->dx = gpt->points[2] - gui->posx;
      gui->dy = gpt->points[3] - gui->posy;
      return 1;
    }
    else if(gui->point_selected >= 0)
    {
      // if ctrl is pressed, we change the type of point
      if(gui->point_edited == gui->point_selected && ((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK))
      {
        dt_masks_point_path_t *point
            = (dt_masks_point_path_t *)g_list_nth_data(form->points, gui->point_edited);
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
        dt_masks_write_form(form, darktable.develop);

        // we recreate the form points
        dt_masks_gui_form_remove(form, gui, index);
        dt_masks_gui_form_create(form, gui, index);
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
      if((state & GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
      {
        // we add a new point to the path
        dt_masks_point_path_t *bzpt = (dt_masks_point_path_t *)(malloc(sizeof(dt_masks_point_path_t)));
        // change the values
        float wd = darktable.develop->preview_pipe->backbuf_width;
        float ht = darktable.develop->preview_pipe->backbuf_height;
        float pts[2] = { pzx * wd, pzy * ht };
        dt_dev_distort_backtransform(darktable.develop, pts, 1);

        bzpt->corner[0] = pts[0] / darktable.develop->preview_pipe->iwidth;
        bzpt->corner[1] = pts[1] / darktable.develop->preview_pipe->iheight;
        bzpt->ctrl1[0] = bzpt->ctrl1[1] = bzpt->ctrl2[0] = bzpt->ctrl2[1] = -1.0;
        bzpt->state = DT_MASKS_POINT_STATE_NORMAL;
        bzpt->border[0] = bzpt->border[1] = MAX(0.005f, masks_border);
        form->points = g_list_insert(form->points, bzpt, gui->seg_selected + 1);
        _path_init_ctrl_points(form);
        dt_masks_gui_form_remove(form, gui, index);
        dt_masks_gui_form_create(form, gui, index);
        gui->point_edited = gui->point_dragging = gui->point_selected = gui->seg_selected + 1;
        gui->seg_selected = -1;
        dt_control_queue_redraw_center();
      }
      else
      {
        // we move the entire segment
        gui->seg_dragging = gui->seg_selected;
        gui->posx = pzx * darktable.develop->preview_pipe->backbuf_width;
        gui->posy = pzy * darktable.develop->preview_pipe->backbuf_height;
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
    if(g_list_length(form->points) < 4)
    {
      // if the form doesn't belong to a group, we don't delete it
      if(parentid <= 0) return 1;

      dt_masks_clear_form_gui(darktable.develop);
      // we hide the form
      if(!(darktable.develop->form_visible->type & DT_MASKS_GROUP))
        darktable.develop->form_visible = NULL;
      else if(g_list_length(darktable.develop->form_visible->points) < 2)
        darktable.develop->form_visible = NULL;
      else
      {
        GList *forms = g_list_first(darktable.develop->form_visible->points);
        while(forms)
        {
          dt_masks_point_group_t *gpt = (dt_masks_point_group_t *)forms->data;
          if(gpt->formid == form->formid)
          {
            darktable.develop->form_visible->points
                = g_list_remove(darktable.develop->form_visible->points, gpt);
            break;
          }
          forms = g_list_next(forms);
        }
      }

      // we delete or remove the shape
      dt_masks_form_remove(module, NULL, form);
      dt_dev_masks_list_change(darktable.develop);
      dt_control_queue_redraw_center();
      return 1;
    }
    form->points = g_list_delete_link(form->points, g_list_nth(form->points, gui->point_selected));
    gui->point_selected = -1;
    _path_init_ctrl_points(form);

    dt_masks_write_form(form, darktable.develop);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);
    gpt->clockwise = _path_is_clockwise(form);
    // we save the move
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if(which == 3 && gui->feather_selected >= 0)
  {
    dt_masks_point_path_t *point
        = (dt_masks_point_path_t *)g_list_nth_data(form->points, gui->feather_selected);
    if(point->state != DT_MASKS_POINT_STATE_NORMAL)
    {
      point->state = DT_MASKS_POINT_STATE_NORMAL;
      _path_init_ctrl_points(form);

      dt_masks_write_form(form, darktable.develop);

      // we recreate the form points
      dt_masks_gui_form_remove(form, gui, index);
      dt_masks_gui_form_create(form, gui, index);
      gpt->clockwise = _path_is_clockwise(form);
      // we save the move
      dt_masks_update_image(darktable.develop);
    }
    return 1;
  }
  else if(which == 3 && parentid > 0 && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    dt_masks_clear_form_gui(darktable.develop);
    // we hide the form
    if(!(darktable.develop->form_visible->type & DT_MASKS_GROUP))
      darktable.develop->form_visible = NULL;
    else if(g_list_length(darktable.develop->form_visible->points) < 2)
      darktable.develop->form_visible = NULL;
    else
    {
      GList *forms = g_list_first(darktable.develop->form_visible->points);
      while(forms)
      {
        dt_masks_point_group_t *gpt = (dt_masks_point_group_t *)forms->data;
        if(gpt->formid == form->formid)
        {
          darktable.develop->form_visible->points
              = g_list_remove(darktable.develop->form_visible->points, gpt);
          break;
        }
        forms = g_list_next(forms);
      }
    }

    // we remove the shape
    dt_masks_form_remove(module, dt_masks_get_from_id(darktable.develop, parentid), form);
    dt_dev_masks_list_remove(darktable.develop, form->formid, parentid);
    return 1;
  }

  return 0;
}

static int dt_path_events_button_released(struct dt_iop_module_t *module, float pzx, float pzy, int which,
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
    dt_masks_point_path_t *point = (dt_masks_point_path_t *)g_list_first(form->points)->data;
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
      point = (dt_masks_point_path_t *)points->data;
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
    gpt->clockwise = _path_is_clockwise(form);
    dt_masks_write_form(form, darktable.develop);
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

    _path_init_ctrl_points(form);

    dt_masks_write_form(form, darktable.develop);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);
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
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd, pzy * ht };
    dt_dev_distort_backtransform(darktable.develop, pts, 1);

    int p1x, p1y, p2x, p2y;
    _path_feather_to_ctrl(point->corner[0] * darktable.develop->preview_pipe->iwidth,
                          point->corner[1] * darktable.develop->preview_pipe->iheight, pts[0], pts[1], &p1x,
                          &p1y, &p2x, &p2y, gpt->clockwise);
    point->ctrl1[0] = (float)p1x / darktable.develop->preview_pipe->iwidth;
    point->ctrl1[1] = (float)p1y / darktable.develop->preview_pipe->iheight;
    point->ctrl2[0] = (float)p2x / darktable.develop->preview_pipe->iwidth;
    point->ctrl2[1] = (float)p2y / darktable.develop->preview_pipe->iheight;

    point->state = DT_MASKS_POINT_STATE_USER;

    _path_init_ctrl_points(form);

    dt_masks_write_form(form, darktable.develop);

    // we recreate the form points
    dt_masks_gui_form_remove(form, gui, index);
    dt_masks_gui_form_create(form, gui, index);
    gpt->clockwise = _path_is_clockwise(form);
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

static int dt_path_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy, double pressure,
                                      int which, dt_masks_form_t *form, int parentid,
                                      dt_masks_form_gui_t *gui, int index)
{
  dt_dev_zoom_t zoom = dt_control_get_dev_zoom();
  int closeup = dt_control_get_dev_closeup();
  float zoom_scale = dt_dev_get_zoom_scale(darktable.develop, zoom, closeup ? 2 : 1, 1);
  float as = 0.005f / zoom_scale * darktable.develop->preview_pipe->backbuf_width;
  if(!gui) return 0;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *)g_list_nth_data(gui->points, index);
  if(!gpt) return 0;

  if(gui->point_dragging >= 0)
  {
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = { pzx * wd, pzy * ht };
    if(gui->creation && g_list_length(form->points) > 3)
    {
      // if we are near the first point, we have to say that the form should be closed
      if(pts[0] - gpt->points[2] < as && pts[0] - gpt->points[2] > -as && pts[1] - gpt->points[3] < as
         && pts[1] - gpt->points[3] > -as)
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
    bzpt->ctrl1[0] += pzx - bzpt->corner[0];
    bzpt->ctrl2[0] += pzx - bzpt->corner[0];
    bzpt->ctrl1[1] += pzy - bzpt->corner[1];
    bzpt->ctrl2[1] += pzy - bzpt->corner[1];
    bzpt->corner[0] = pzx;
    bzpt->corner[1] = pzy;
    _path_init_ctrl_points(form);
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
    dt_masks_point_path_t *point = (dt_masks_point_path_t *)g_list_nth_data(form->points, gui->seg_dragging);
    dt_masks_point_path_t *point2 = (dt_masks_point_path_t *)g_list_nth_data(form->points, pos2);
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

    _path_init_ctrl_points(form);

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
    dt_masks_point_path_t *point
        = (dt_masks_point_path_t *)g_list_nth_data(form->points, gui->feather_dragging);

    int p1x, p1y, p2x, p2y;
    _path_feather_to_ctrl(point->corner[0] * darktable.develop->preview_pipe->iwidth,
                          point->corner[1] * darktable.develop->preview_pipe->iheight, pts[0], pts[1], &p1x,
                          &p1y, &p2x, &p2y, gpt->clockwise);
    point->ctrl1[0] = (float)p1x / darktable.develop->preview_pipe->iwidth;
    point->ctrl1[1] = (float)p1y / darktable.develop->preview_pipe->iheight;
    point->ctrl2[0] = (float)p2x / darktable.develop->preview_pipe->iwidth;
    point->ctrl2[1] = (float)p2y / darktable.develop->preview_pipe->iheight;
    point->state = DT_MASKS_POINT_STATE_USER;

    _path_init_ctrl_points(form);
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

    dt_masks_point_path_t *point = (dt_masks_point_path_t *)g_list_nth_data(form->points, k);
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
    gui->posx = pzx * darktable.develop->preview_pipe->backbuf_width;
    gui->posy = pzy * darktable.develop->preview_pipe->backbuf_height;
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
  dt_path_get_distance(pzx, (int)pzy, as, gui, index, nb, &in, &inb, &near, &ins);
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

static void dt_path_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *gui, int index,
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

  // draw path
  if(gpt->points_count > nb * 3 + 6)
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
  if(gui->group_selected == index && gpt->points_count > nb * 3 + 6)
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
    _path_ctrl2_to_feather(gpt->points[k * 6 + 2] + dx, gpt->points[k * 6 + 3] + dy,
                           gpt->points[k * 6 + 4] + dx, gpt->points[k * 6 + 5] + dy, &ffx, &ffy,
                           gpt->clockwise);
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
  if((gui->group_selected == index) && gpt->border_count > nb * 3 + 6)
  {
    int dep = 1;
    for(int i = nb * 3; i < gpt->border_count; i++)
    {
      if(gpt->border[i * 2] == -999999)
      {
        if(gpt->border[i * 2 + 1] == -999999) break;
        i = gpt->border[i * 2 + 1] - 1;
        continue;
      }
      if(dep)
      {
        cairo_move_to(cr, gpt->border[i * 2] + dx, gpt->border[i * 2 + 1] + dy);
        dep = 0;
      }
      else
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

    // we draw the path segment by segment
    for(int k = 0; k < nb; k++)
    {
      // draw the point
      if(gui->point_border_selected == k)
      {
        anchor_size = 7.0f / zoom_scale;
      }
      else
      {
        anchor_size = 5.0f / zoom_scale;
      }
      cairo_set_source_rgba(cr, .8, .8, .8, .8);
      cairo_rectangle(cr, gpt->border[k * 6] - (anchor_size * 0.5) + dx,
                      gpt->border[k * 6 + 1] - (anchor_size * 0.5) + dy, anchor_size, anchor_size);
      cairo_fill_preserve(cr);

      if(gui->point_border_selected == k)
        cairo_set_line_width(cr, 2.0 / zoom_scale);
      else
        cairo_set_line_width(cr, 1.0 / zoom_scale);
      cairo_set_source_rgba(cr, .3, .3, .3, .8);
      cairo_set_dash(cr, dashed, 0, 0);
      cairo_stroke(cr);
    }
  }

  // draw the source if needed
  if(!gui->creation && gpt->source_count > nb * 3 + 6)
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

static int dt_path_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece,
                                   dt_masks_form_t *form, int *width, int *height, int *posx, int *posy)
{
  if(!module) return 0;
  // we get buffers for all points
  float *points = NULL, *border = NULL;
  int points_count, border_count;
  if(!_path_get_points_border(module->dev, form, module->priority, piece->pipe, &points, &points_count,
                              &border, &border_count, 1))
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
    if(xx == -999999)
    {
      if(yy == -999999) break; // that means we have to skip the end of the border path
      i = yy - 1;
      continue;
    }
    xmin = fminf(xx, xmin);
    xmax = fmaxf(xx, xmax);
    ymin = fminf(yy, ymin);
    ymax = fmaxf(yy, ymax);
  }
  for(int i = nb_corner * 3; i < points_count; i++)
  {
    // we look at the path too
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

static int dt_path_get_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                            int *width, int *height, int *posx, int *posy)
{
  if(!module) return 0;
  // we get buffers for all points
  float *points = NULL, *border = NULL;
  int points_count, border_count;
  if(!_path_get_points_border(module->dev, form, module->priority, piece->pipe, &points, &points_count,
                              &border, &border_count, 0))
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
    if(xx == -999999)
    {
      if(yy == -999999) break; // that means we have to skip the end of the border path
      i = yy - 1;
      continue;
    }
    xmin = fminf(xx, xmin);
    xmax = fmaxf(xx, xmax);
    ymin = fminf(yy, ymin);
    ymax = fmaxf(yy, ymax);
  }
  for(int i = nb_corner * 3; i < points_count; i++)
  {
    // we look at the path too
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
static void _path_falloff(float **buffer, int *p0, int *p1, int posx, int posy, int bw)
{
  // segment length
  int l = sqrt((p1[0] - p0[0]) * (p1[0] - p0[0]) + (p1[1] - p0[1]) * (p1[1] - p0[1])) + 1;

  float lx = p1[0] - p0[0];
  float ly = p1[1] - p0[1];

  for(int i = 0; i < l; i++)
  {
    // position
    int x = (int)((float)i * lx / (float)l) + p0[0] - posx;
    int y = (int)((float)i * ly / (float)l) + p0[1] - posy;
    float op = 1.0 - (float)i / (float)l;
    (*buffer)[y * bw + x] = fmaxf((*buffer)[y * bw + x], op);
    if(x > 0)
      (*buffer)[y * bw + x - 1]
          = fmaxf((*buffer)[y * bw + x - 1], op); // this one is to avoid gap due to int rounding
    if(y > 0)
      (*buffer)[(y - 1) * bw + x]
          = fmaxf((*buffer)[(y - 1) * bw + x], op); // this one is to avoid gap due to int rounding
  }
}

static int dt_path_get_mask(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                            float **buffer, int *width, int *height, int *posx, int *posy)
{
  if(!module) return 0;
  double start = dt_get_wtime();
  double start2;

  // we get buffers for all points
  float *points = NULL, *border = NULL;
  int points_count, border_count;
  if(!_path_get_points_border(module->dev, form, module->priority, piece->pipe, &points, &points_count,
                              &border, &border_count, 0))
  {
    free(points);
    free(border);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] path points took %0.04f sec\n", form->name, dt_get_wtime() - start);
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
    if(xx == -999999)
    {
      if(yy == -999999) break; // that means we have to skip the end of the border path
      i = yy - 1;
      continue;
    }
    xmin = fminf(xx, xmin);
    xmax = fmaxf(xx, xmax);
    ymin = fminf(yy, ymin);
    ymax = fmaxf(yy, ymax);
  }
  for(int i = nb_corner * 3; i < points_count; i++)
  {
    // we look at the path too
    float xx = points[i * 2];
    float yy = points[i * 2 + 1];
    xmin = fminf(xx, xmin);
    xmax = fmaxf(xx, xmax);
    ymin = fminf(yy, ymin);
    ymax = fmaxf(yy, ymax);
  }

  const int hb = *height = ymax - ymin + 4;
  const int wb = *width = xmax - xmin + 4;
  *posx = xmin - 2;
  *posy = ymin - 2;

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill min max took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // we allocate the buffer
  *buffer = calloc((*width) * (*height), sizeof(float));

  // we write all the point around the path into the buffer
  int nbp = border_count;
  int lastx, lasty, lasty2, nx;
  if(nbp > 2)
  {
    lastx = (int)points[(nbp - 1) * 2];
    lasty = (int)points[(nbp - 1) * 2 + 1];
    lasty2 = (int)points[(nbp - 2) * 2 + 1];

    int just_change_dir = 0;
    for(int ii = nb_corner * 3; ii < 2 * nbp; ii++)
    {
      // we are writing more than 1 loop in the case the dir in y change
      // exactly at start/end point
      int i = ii;
      if(ii >= nbp) i = ii - nbp + nb_corner * 3;
      int xx = (int)points[i * 2];
      int yy = (int)points[i * 2 + 1];

      // we don't store the point if it has the same y value as the last one
      if(yy == lasty) continue;

      // we want to be sure that there is no y jump
      if(yy - lasty > 1 || yy - lasty < -1)
      {
        if(yy < lasty)
        {
          for(int j = yy + 1; j < lasty; j++)
          {
            int nx = (j - yy) * (lastx - xx) / (float)(lasty - yy) + xx;
            (*buffer)[(j - (*posy)) * (*width) + nx - (*posx)] = 1.0f;
          }
          lasty2 = yy + 2;
          lasty = yy + 1;
        }
        else
        {
          for(int j = lasty + 1; j < yy; j++)
          {
            nx = (j - lasty) * (xx - lastx) / (float)(yy - lasty) + lastx;
            (*buffer)[(j - (*posy)) * (*width) + nx - (*posx)] = 1.0f;
          }
          lasty2 = yy - 2;
          lasty = yy - 1;
        }
      }
      // if we change the direction of the path (in y), then we add a extra point
      if((lasty - lasty2) * (lasty - yy) > 0)
      {
        (*buffer)[(lasty - (*posy)) * (*width) + lastx + 1 - (*posx)] = 1.0f;
        just_change_dir = 1;
      }
      // we add the point
      if(just_change_dir && ii == i)
      {
        // if we have changed the direction, we have to be carefull that point can be at the same place
        // as the previous one , especially on sharp edges
        float v = (*buffer)[(yy - (*posy)) * (*width) + xx - (*posx)];
        if(v > 0.0)
        {
          if(xx - (*posx) > 0)
            (*buffer)[(yy - (*posy)) * (*width) + xx - 1 - (*posx)] = 1.0f;
          else if(xx - (*posx) < (*width) - 1)
            (*buffer)[(yy - (*posy)) * (*width) + xx + 1 - (*posx)] = 1.0f;
        }
        else
        {
          (*buffer)[(yy - (*posy)) * (*width) + xx - (*posx)] = 1.0f;
          just_change_dir = 0;
        }
      }
      else
        (*buffer)[(yy - (*posy)) * (*width) + xx - (*posx)] = 1.0f;
      // we change last values
      lasty2 = lasty;
      lasty = yy;
      lastx = xx;
      if(ii != i) break;
    }
  }
  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill draw path took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  for(int yy = 0; yy < hb; yy++)
  {
    int state = 0;
    for(int xx = 0; xx < wb; xx++)
    {
      float v = (*buffer)[yy * wb + xx];
      if(v == 1.0f) state = !state;
      if(state) (*buffer)[yy * wb + xx] = 1.0f;
    }
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill fill plain took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // now we fill the falloff
  int p0[2], p1[2];
  int last0[2] = { -100, -100 }, last1[2] = { -100, -100 };
  nbp = 0;
  int next = 0;
  for(int i = nb_corner * 3; i < border_count; i++)
  {
    p0[0] = points[i * 2], p0[1] = points[i * 2 + 1];
    if(next > 0)
      p1[0] = border[next * 2], p1[1] = border[next * 2 + 1];
    else
      p1[0] = border[i * 2], p1[1] = border[i * 2 + 1];

    // now we check p1 value to know if we have to skip a part
    if(next == i) next = 0;
    while(p1[0] == -999999)
    {
      if(p1[1] == -999999)
        next = i - 1;
      else
        next = p1[1];
      p1[0] = border[next * 2], p1[1] = border[next * 2 + 1];
    }

    // and we draw the falloff
    if(last0[0] != p0[0] || last0[1] != p0[1] || last1[0] != p1[0] || last1[1] != p1[1])
    {
      _path_falloff(buffer, p0, p1, *posx, *posy, *width);
      last0[0] = p0[0], last0[1] = p0[1];
      last1[0] = p1[0], last1[1] = p1[1];
    }
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill fill falloff took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);

  free(points);
  free(border);

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

    if(x >= xmin + 1 && y >= ymin + 1 && x <= xmax - 1 && y <= ymax - 1)
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
    int kk = (k + point_start) % point_count;

    if(l < 0 && path[2 * kk] < xmin) l = k;       // where we leave roi
    if(l >= 0 && path[2 * kk] >= xmin) r = k - 1; // where we re-enter roi

    // replace that segment
    if(l >= 0 && r >= 0)
    {
      int count = r - l + 1;
      int ll = (l - 1 + point_start) % point_count;
      int rr = (r + 1 + point_start) % point_count;
      float delta_y = (count == 1) ? 0 : (path[2 * rr + 1] - path[2 * ll + 1]) / (count - 1);
      float start_y = path[2 * ll + 1];

      for(int n = 0; n < count; n++)
      {
        int nn = (n + l + point_start) % point_count;
        path[2 * nn] = xmin;
        path[2 * nn + 1] = start_y + n * delta_y;
      }

      l = r = -1;
    }
  }

  // find the crossing points with xmax and replace segment by nodes on border
  for(int k = 0; k < point_count; k++)
  {
    int kk = (k + point_start) % point_count;

    if(l < 0 && path[2 * kk] > xmax) l = k;       // where we leave roi
    if(l >= 0 && path[2 * kk] <= xmax) r = k - 1; // where we re-enter roi

    // replace that segment
    if(l >= 0 && r >= 0)
    {
      int count = r - l + 1;
      int ll = (l - 1 + point_start) % point_count;
      int rr = (r + 1 + point_start) % point_count;
      float delta_y = (count == 1) ? 0 : (path[2 * rr + 1] - path[2 * ll + 1]) / (count - 1);
      float start_y = path[2 * ll + 1];

      for(int n = 0; n < count; n++)
      {
        int nn = (n + l + point_start) % point_count;
        path[2 * nn] = xmax;
        path[2 * nn + 1] = start_y + n * delta_y;
      }

      l = r = -1;
    }
  }

  // find the crossing points with ymin and replace segment by nodes on border
  for(int k = 0; k < point_count; k++)
  {
    int kk = (k + point_start) % point_count;

    if(l < 0 && path[2 * kk + 1] < ymin) l = k;       // where we leave roi
    if(l >= 0 && path[2 * kk + 1] >= ymin) r = k - 1; // where we re-enter roi

    // replace that segment
    if(l >= 0 && r >= 0)
    {
      int count = r - l + 1;
      int ll = (l - 1 + point_start) % point_count;
      int rr = (r + 1 + point_start) % point_count;
      float delta_x = (count == 1) ? 0 : (path[2 * rr] - path[2 * ll]) / (count - 1);
      float start_x = path[2 * ll];

      for(int n = 0; n < count; n++)
      {
        int nn = (n + l + point_start) % point_count;
        path[2 * nn] = start_x + n * delta_x;
        path[2 * nn + 1] = ymin;
      }

      l = r = -1;
    }
  }

  // find the crossing points with ymax and replace segment by nodes on border
  for(int k = 0; k < point_count; k++)
  {
    int kk = (k + point_start) % point_count;

    if(l < 0 && path[2 * kk + 1] > ymax) l = k;       // where we leave roi
    if(l >= 0 && path[2 * kk + 1] <= ymax) r = k - 1; // where we re-enter roi

    // replace that segment
    if(l >= 0 && r >= 0)
    {
      int count = r - l + 1;
      int ll = (l - 1 + point_start) % point_count;
      int rr = (r + 1 + point_start) % point_count;
      float delta_x = (count == 1) ? 0 : (path[2 * rr] - path[2 * ll]) / (count - 1);
      float start_x = path[2 * ll];

      for(int n = 0; n < count; n++)
      {
        int nn = (n + l + point_start) % point_count;
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
    const float op = 1.0 - (float)i / (float)l;
    float *buf = buffer + (size_t)y * bw + x;
    if(x >= 0 && x < bw && y >= 0 && y < bh) buf[0] = fmaxf(buf[0], op);
    if(x + dx >= 0 && x + dx < bw && y >= 0 && y < bh)
      buf[dx] = fmaxf(buf[dx], op); // this one is to avoid gap due to int rounding
    if(x >= 0 && x < bw && y + dy >= 0 && y + dy < bh)
      buf[dpy] = fmaxf(buf[dpy], op); // this one is to avoid gap due to int rounding
  }
}

static int dt_path_get_mask_roi(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form,
                                const dt_iop_roi_t *roi, float *buffer)
{
  if(!module) return 0;
  double start = dt_get_wtime();
  double start2;

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
  float *points = NULL, *border = NULL, *cpoints = NULL;
  int points_count, border_count;
  if(!_path_get_points_border(module->dev, form, module->priority, piece->pipe, &points, &points_count,
                              &border, &border_count, 0) || (points_count <= 2))
  {
    free(points);
    free(border);
    return 0;
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] path points took %0.04f sec\n", form->name, dt_get_wtime() - start);
  start = start2 = dt_get_wtime();

  // empty the output buffer
  memset(buffer, 0, (size_t)width * height * sizeof(float));

  guint nb_corner = g_list_length(form->points);

  // we shift and scale down path and border
  for(int i = nb_corner * 3; i < border_count; i++)
  {
    float xx = border[2 * i];
    float yy = border[2 * i + 1];
    if(xx == -999999)
    {
      if(yy == -999999) break; // that means we have to skip the end of the border path
      i = yy - 1;
      continue;
    }
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

  // now check if path is at least partially within roi
  for(int i = nb_corner * 3; i < points_count; i++)
  {
    int xx = points[i * 2];
    int yy = points[i * 2 + 1];
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
    int x = width / 2;
    int y = height / 2;

    for(int i = nb_corner * 3; i < points_count; i++)
    {
      int yy = (int)points[2 * i + 1];
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
    int xx = border[i * 2];
    int yy = border[i * 2 + 1];
    if(xx == -999999)
    {
      if(yy == -999999) break; // that means we have to skip the end of the border path
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
    free(points);
    free(border);
    return 1;
  }

  // now get min/max values
  float xmin, xmax, ymin, ymax;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  for(int i = nb_corner * 3; i < points_count; i++)
  {
    float xx = points[i * 2];
    float yy = points[i * 2 + 1];
    xmin = fminf(xx, xmin);
    xmax = fmaxf(xx, xmax);
    ymin = fminf(yy, ymin);
    ymax = fmaxf(yy, ymax);
  }
  for(int i = nb_corner * 3; i < border_count; i++)
  {
    float xx = border[i * 2];
    float yy = border[i * 2 + 1];
    if(xx == -999999)
    {
      if(yy == -999999) break; // that means we have to skip the end of the border path
      i = yy - 1;
      continue;
    }
    xmin = fminf(xx, xmin);
    xmax = fmaxf(xx, xmax);
    ymin = fminf(yy, ymin);
    ymax = fmaxf(yy, ymax);
  }

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill min max took %0.04f sec\n", form->name,
             dt_get_wtime() - start2);
  start2 = dt_get_wtime();

  // deal with path if it does not lie outside of roi
  if(path_in_roi)
  {
    // second copy of path which we can modify when cropping to roi
    cpoints = malloc(2 * points_count * sizeof(float));
    if(cpoints == NULL)
    {
      free(points);
      free(border);
      return 0;
    }
    memcpy(cpoints, points, 2 * points_count * sizeof(float));

    // now we clip cpoints to roi -> catch special case when roi lies completely within path.
    // dirty trick: we allow path to extend one pixel beyond height-1. this avoids need of special handling
    // of the last roi line in the following edge-flag polygon fill algorithm.
    int crop_success = _path_crop_to_roi(cpoints + 2 * (nb_corner * 3), points_count - nb_corner * 3, 0,
                                         width - 1, 0, height);
    path_encircles_roi = path_encircles_roi || !crop_success;

    if(darktable.unmuted & DT_DEBUG_PERF)
      dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill crop to roi took %0.04f sec\n", form->name,
               dt_get_wtime() - start2);
    start2 = dt_get_wtime();

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

          size_t index = (size_t)yy * width + xx;

          buffer[index] = 1.0f - buffer[index];
        }
      }

      if(darktable.unmuted & DT_DEBUG_PERF)
        dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill draw path took %0.04f sec\n", form->name,
                 dt_get_wtime() - start2);
      start2 = dt_get_wtime();

      // we fill the inside plain
      // we don't need to deal with parts of shape outside of roi
      xmin = fmaxf(xmin, 0);
      xmax = fminf(xmax, width - 1);
      ymin = fmaxf(ymin, 0);
      ymax = fminf(ymax, height - 1);

      for(int yy = ymin; yy <= ymax; yy++)
      {
        int state = 0;
        for(int xx = xmin; xx <= xmax; xx++)
        {
          size_t index = (size_t)yy * width + xx;
          float v = buffer[index];
          if(v > 0.5f) state = !state;
          if(state) buffer[index] = 1.0f;
        }
      }

      if(darktable.unmuted & DT_DEBUG_PERF)
        dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill fill plain took %0.04f sec\n", form->name,
                 dt_get_wtime() - start2);
      start2 = dt_get_wtime();
    }
    free(cpoints);
  }

  // deal with feather if it does not lie outside of roi
  if(!path_encircles_roi)
  {
    int p0[2], p1[2];
    int last0[2] = { -100, -100 };
    int last1[2] = { -100, -100 };
    int next = 0;
    for(int i = nb_corner * 3; i < border_count; i++)
    {
      p0[0] = floorf(points[i * 2] + 0.5f);
      p0[1] = ceilf(points[i * 2 + 1]);
      if(next > 0)
      {
        p1[0] = border[next * 2];
        p1[1] = border[next * 2 + 1];
      }
      else
      {
        p1[0] = border[i * 2];
        p1[1] = border[i * 2 + 1];
      }

      // now we check p1 value to know if we have to skip a part
      if(next == i) next = 0;
      while(p1[0] == -999999)
      {
        if(p1[1] == -999999)
          next = i - 1;
        else
          next = p1[1];
        p1[0] = border[next * 2];
        p1[1] = border[next * 2 + 1];
      }

      // and we draw the falloff
      if(last0[0] != p0[0] || last0[1] != p0[1] || last1[0] != p1[0] || last1[1] != p1[1])
      {
        _path_falloff_roi(buffer, p0, p1, width, height);
        last0[0] = p0[0];
        last0[1] = p0[1];
        last1[0] = p1[0];
        last1[1] = p1[1];
      }
    }

    if(darktable.unmuted & DT_DEBUG_PERF)
      dt_print(DT_DEBUG_MASKS, "[masks %s] path_fill fill falloff took %0.04f sec\n", form->name,
               dt_get_wtime() - start2);
  }

  free(points);
  free(border);

  if(darktable.unmuted & DT_DEBUG_PERF)
    dt_print(DT_DEBUG_MASKS, "[masks %s] path fill buffer took %0.04f sec\n", form->name,
             dt_get_wtime() - start);

  return 1;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
