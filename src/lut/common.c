#include "lut/common.h"

point_t transform_coords(point_t p, point_t *bb)
{
  point_t result;
  float e, f;

  e = (bb[BOTTOM_LEFT].x - bb[TOP_LEFT].x) * p.y + bb[TOP_LEFT].x;
  f = (bb[BOTTOM_RIGHT].x - bb[TOP_RIGHT].x) * p.y + bb[TOP_RIGHT].x;
  result.x = (f - e) * p.x + e;

  e = (bb[TOP_RIGHT].y - bb[TOP_LEFT].y) * p.x + bb[TOP_LEFT].y;
  f = (bb[BOTTOM_RIGHT].y - bb[BOTTOM_LEFT].y) * p.x + bb[BOTTOM_LEFT].y;
  result.y = (f - e) * p.y + e;

  return result;
}
