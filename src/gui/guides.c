#include "guides.h"

void
dt_guides_q_rect(dt_QRect_t *R1, float left, float top, float width, float height)
{
  R1->left=left;
  R1->top=top;
  R1->right=left+width;
  R1->bottom=top+height;
  R1->width=width;
  R1->height=height;
}


void
dt_guides_draw_simple_grid(cairo_t *cr, const float left, const float top,  const float right, const float bottom, float zoom_scale)
{
  // cairo_set_operator(cr, CAIRO_OPERATOR_XOR);
  cairo_set_line_width(cr, 1.0/zoom_scale);
  cairo_set_source_rgb(cr, .2, .2, .2);
  dt_draw_grid(cr, 3, left, top, right, bottom);
  cairo_translate(cr, 1.0/zoom_scale, 1.0/zoom_scale);
  cairo_set_source_rgb(cr, .8, .8, .8);
  dt_draw_grid(cr, 3, left, top, right, bottom);
  cairo_set_source_rgba(cr, .8, .8, .8, 0.5);
  double dashes = 5.0/zoom_scale;
  cairo_set_dash(cr, &dashes, 1, 0);
  dt_draw_grid(cr, 9, left, top, right, bottom);
}


void
dt_guides_draw_diagonal_method(cairo_t *cr, const float x, const float y, const float w, const float h)
{
  if (w > h)
  {
    dt_draw_line(cr, x, y, x+h, y+h);
    dt_draw_line(cr, x, y+h, x+h, y);
    dt_draw_line(cr, x+w-h, y, x+w, y+h);
    dt_draw_line(cr, x+w-h, y+h, x+w, y);
  }
  else
  {
    dt_draw_line(cr, x, y, x+w, y+w);
    dt_draw_line(cr, x, y+w, x+w, y);
    dt_draw_line(cr, x, y+h-w, x+w, y+h);
    dt_draw_line(cr, x, y+h, x+w, y+h-w);
  }
}


void
dt_guides_draw_rules_of_thirds(cairo_t *cr, const float left, const float top,  const float right, const float bottom, const float xThird, const float yThird)
{
  dt_draw_line(cr, left + xThird, top, left + xThird, bottom);
  dt_draw_line(cr, left + 2*xThird, top, left + 2*xThird, bottom);

  dt_draw_line(cr, left, top + yThird, right, top + yThird);
  dt_draw_line(cr, left, top + 2*yThird, right, top + 2*yThird);
}


void
dt_guides_draw_harmonious_triangles(cairo_t *cr, const float left, const float top,  const float right, const float bottom, const float dst)
{
  float width, height;
  width = right - left;
  height = bottom - top;

  dt_draw_line(cr, -width/2, -height/2, width/2,  height/2);
  dt_draw_line(cr, -width/2+dst, -height/2, -width/2,  height/2);
  dt_draw_line(cr, width/2, -height/2, width/2-dst,  height/2);
}


#define RADIANS(degrees) ((degrees) * (M_PI / 180.))
void
dt_guides_draw_golden_mean(cairo_t *cr, dt_QRect_t* R1, dt_QRect_t* R2, dt_QRect_t* R3, dt_QRect_t* R4, dt_QRect_t* R5, dt_QRect_t* R6, dt_QRect_t* R7, gboolean goldenSection, gboolean goldenTriangle, gboolean goldenSpiralSection, gboolean goldenSpiral)
{
  // Drawing Golden sections.
  if (goldenSection)
  {
    // horizontal lines:
    dt_draw_line(cr, R1->left, R2->top, R2->right, R2->top);
    dt_draw_line(cr, R1->left, R1->top + R2->height, R2->right, R1->top + R2->height);

    // vertical lines:
    dt_draw_line(cr, R1->right, R1->top, R1->right, R1->bottom);
    dt_draw_line(cr, R1->left+R2->width, R1->top, R1->left+R2->width, R1->bottom);
  }

  // Drawing Golden triangle guides.
  if (goldenTriangle)
  {
    dt_draw_line(cr, R1->left, R1->bottom, R2->right, R1->top);
    dt_draw_line(cr, R1->left, R1->top, R2->right-R1->width, R1->bottom);
    dt_draw_line(cr, R1->left + R1->width, R1->top, R2->right, R1->bottom);
  }

  // Drawing Golden spiral sections.
  if (goldenSpiralSection)
  {
    dt_draw_line(cr, R1->right, R1->top,    R1->right, R1->bottom);
    dt_draw_line(cr, R2->left,  R2->top,    R2->right, R2->top);
    dt_draw_line(cr, R3->left,  R3->top,    R3->left, R3->bottom);
    dt_draw_line(cr, R4->left,  R4->bottom, R4->right, R4->bottom);
    dt_draw_line(cr, R5->right, R5->top,    R5->right, R5->bottom);
    dt_draw_line(cr, R6->left,  R6->top,    R6->right, R6->top);
    dt_draw_line(cr, R7->left,  R7->top,    R7->left, R7->bottom);
  }

  // Drawing Golden Spiral.
  if (goldenSpiral)
  {
    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, R1->width/R1->height, 1);
    cairo_arc ( cr, R1->right/R1->width*R1->height, R1->top, R1->height, RADIANS(90), RADIANS(180) );
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, R2->width/R2->height, 1);
    cairo_arc ( cr, R2->left/R2->width*R2->height, R2->top, R2->height, RADIANS(0), RADIANS(90));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, R3->width/R3->height, 1);
    cairo_arc ( cr, R3->left/R3->width*R3->height, R3->bottom, R3->height, RADIANS(270), RADIANS(360));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, 1, R4->height/R4->width);
    cairo_arc ( cr, R4->right, R4->bottom/R4->height*R4->width, R4->width, RADIANS(180), RADIANS(270));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, 1, R5->height/R5->width);
    cairo_arc ( cr, R5->right, R5->top/R5->height*R5->width, R5->width, RADIANS(90), RADIANS(180));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, 1, R6->height/R6->width);
    cairo_arc ( cr, R6->left, R6->top/R6->height*R6->width, R6->width, RADIANS(0), RADIANS(90));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, R7->width/R7->height, 1);
    cairo_arc ( cr, R7->left/R7->width*R7->height, R7->bottom, R7->height, RADIANS(270), RADIANS(360));
    cairo_restore(cr);

    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_scale(cr, (R6->width-R7->width)/R7->height, 1);
    cairo_arc ( cr, R7->left/(R6->width-R7->width)*R7->height, R7->bottom, R7->height, RADIANS(210), RADIANS(270));
    cairo_restore(cr);
  }
}
#undef RADIANS
// These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
