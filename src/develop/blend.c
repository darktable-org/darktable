/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson.

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
#include "control/control.h"
#include "blend.h"

typedef float (_blend_func)(float max,float a,float b);

/* normal blend */
static float _blend_normal(float max,float a,float b) { return b; }
/* lighten */
static float _blend_lighten(float max, float a,float b) { return (a>b)?a:b; }
/* darken */
static float _blend_darken(float max, float a,float b) { return (a<b)?a:b; }
/* multiply */
static float _blend_multiply(float max, float a,float b) { return (a*b)/max; }
/* average */
static float _blend_average(float max, float a,float b) { return (a+b)/2.0; }
/* add */
static float _blend_add(float max, float a,float b) { return fmin(max,a+b); }
/* substract */
static float _blend_substract(float max, float a,float b) { return ((a+b<max) ? 0:(b+a-max)); }
/* difference */
static float _blend_difference(float max, float a,float b) { return fabs(a-b); }

/* screen */
static float _blend_screen(float max, float a,float b) { return max-(((max-a)*(max-b))/max); }
/* overlay */
static float _blend_overlay(float max, float a,float b) { return ((a < (max/2.0)) ? (2*b*a/max):(max-2*(max-b)*(max-a)/max)); }


void dt_develop_blend_process (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const struct dt_iop_roi_t *roi_in, const struct dt_iop_roi_t *roi_out)
{
	float *in =(float *)i;
	float *out =(float *)o;
	const int ch = piece->colors;
	_blend_func *blend = NULL;
	dt_develop_blend_params_t *d = (dt_develop_blend_params_t *)piece->blendop_data;
	/* check if blend is disabled */
	if (!d || d->mode==0) return;
	
	/* select the blend operator */
	switch (d->mode)
	{
		case DEVELOP_BLEND_LIGHTEN:
			blend = _blend_lighten;
		break;
		
		case DEVELOP_BLEND_DARKEN:
			blend = _blend_darken;
		break;
		
		case DEVELOP_BLEND_MULTIPLY:
			blend = _blend_multiply;
		break;
		
		case DEVELOP_BLEND_AVERAGE:
			blend = _blend_average;
		break;
		case DEVELOP_BLEND_ADD:
			blend = _blend_add;
		break;
		case DEVELOP_BLEND_SUBSTRACT:
			blend = _blend_substract;
		break;
		case DEVELOP_BLEND_DIFFERENCE:
			blend = _blend_difference;
		break;
		case DEVELOP_BLEND_SCREEN:
			blend = _blend_screen;
		break;
		case DEVELOP_BLEND_OVERLAY:
			blend = _blend_overlay;
		break;
		
		/* fallback to normal blend */
		case DEVELOP_BLEND_NORMAL:
		default:
			blend = _blend_normal;
		break;
	}
	
	if (!(d->mode & DEVELOP_BLEND_MASK_FLAG))
	{
		/* blending without mask */
		
		/* get the clipped opacity value  0 - 1 */
		const float opacity = fmin(fmax(0,(d->opacity/100.0)),1.0);

		
		/* get channel max values depending on colorspace */
		float max[3] = {1.0,1.0,1.0};
		dt_iop_colorspace_type_t cst = dt_iop_module_colorspace(self);
		
		/* if module is in lab space, lets set max for L */
		if (cst == iop_cs_Lab) max[0] = 100.0;
		
#ifdef _OPENMP
#pragma omp parallel for default(none) shared(in,roi_out,out,blend,d,max)
#endif
		for (int y=0;y<roi_out->height;y++)
			for (int x=0;x<roi_out->width;x++)
			{
				int index=(y*roi_out->width+x)*ch;
				for (int k=index;k<(index+3);k++)
					out[k] = (in[k] * (1.0-opacity)) + (blend(max[k-index], in[k], out[k]) * opacity);
			}	
	}
	else
	{
		/* blending without mask */
		dt_control_log("blending using masks is not yet implemented.");
		
	}
}