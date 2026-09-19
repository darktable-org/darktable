/*
    This file is part of darktable,
    Copyright (C) 2026 darktable developers.

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

/* Per-pixel math of the film grain module's spektrafilm method, compiled by
 * src/iop/filmgrain.c and by data/kernels/filmgrain.cl from this one copy.
 *
 * The two paths must agree bit-for-bit. Everything here feeds
 * grain_layer_particle, whose Poisson accept/reject loop turns a one-ULP
 * difference in the density it is handed into a whole-integer grain count
 * difference, so this file follows the rules grain.h sets out: plain scalar
 * arithmetic, correctly-rounded operations only, and no transcendental
 * library calls (grain_log2f / grain_exp2f stand in for log2 / exp2).
 *
 * grain.h must be included first; this header builds on its functions and
 * on its GRAIN_INLINE. Both includers are covered by the FP-contraction
 * guards grain.h describes: the .cl sets the pragma, and filmgrain.c is in
 * the SPEKTRA_FP_CONTRACT_FLAGS list in src/CMakeLists.txt.
 */

#pragma once

#if (defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)) && !defined(GRAIN_CL)
#error "filmgrain.h: OpenCL translation unit must '#define GRAIN_CL 1' before including this header"
#endif

/* Scene-linear value that lands in the middle of the density scale. The
   usual 18.45 % grey, so an image exposed for its midtones puts them where
   the emulsion has the most latitude either side. */
#define FILMGRAIN_MIDDLE_GREY 0.1845f

/* Exposure range, in EV, that the characteristic curve spans from clear
   base to full density, centred on FILMGRAIN_MIDDLE_GREY. 11 EV is typical of
   a colour negative's usable latitude. It is also the factor that turns a
   density deviation back into an exposure change, so it sets how many EV a
   given amount of grain moves a pixel by. */
#define FILMGRAIN_LATITUDE_EV 11.0f

/* Maximum density of the modelled emulsion. Not exposed: it only fixes the
   scale that grain strength then works against. */
#define FILMGRAIN_DMAX 2.0f

/* Pointer address-space qualifier for the tone table argument below. */
#ifdef GRAIN_CL
#define FILMGRAIN_GMEM global
#else
#define FILMGRAIN_GMEM
#endif

/* The tone curve is a table over the whole characteristic curve,
   FILMGRAIN_TONE_LUT_SIZE entries from its toe (entry 0) to its shoulder
   (last entry), and is controlled by FILMGRAIN_TONE_NODES nodes one EV
   apart, centred on middle grey. With an 11 EV latitude the outer nodes sit
   half an EV inside toe and shoulder, and the curve is flat beyond them.

   The size gives 24 entries per EV with the half-EV margins falling on
   whole entries, so every node lands exactly on an entry and reads back its
   own value. Between nodes, linear reads of the table stay within 0.25 % of
   the cubic even for a curve alternating 0 % and 200 % from node to node. */
#define FILMGRAIN_TONE_LUT_SIZE 265
#define FILMGRAIN_TONE_NODES 11
#define FILMGRAIN_TONE_EV_FIRST -5.0f

/* Film density for a scene-linear channel value: a straight characteristic
   curve in log exposure, rising from 0 at LATITUDE/2 below middle grey to
   FILMGRAIN_DMAX at LATITUDE/2 above it, and flat outside that range.

   The inner clamp keeps grain_log2f inside its domain (positive, finite,
   normal) whatever the pixel holds: zero, negative and NaN values all
   collapse to the floor through fmax's NaN handling, and +inf to the
   ceiling. */
GRAIN_INLINE float filmgrain_density(float value)
{
  const float rel = grain_clampf(value / FILMGRAIN_MIDDLE_GREY, 1e-10f, 1e30f);
  const float t = 0.5f + grain_log2f(rel) / FILMGRAIN_LATITUDE_EV;
  return FILMGRAIN_DMAX * grain_clampf(t, 0.0f, 1.0f);
}

/* One emulsion sub-layer's grain at one pixel, as a deviation from the
   density that sub-layer was drawn against.

   `share` is the fraction of the total density this sub-layer carries, and
   `dmin` its density floor, so the sub-layer sits at share * density + dmin.
   grain_layer_particle's mean is exactly that value, so subtracting it
   leaves a zero-mean field with no image content in it: the blurs that
   follow act on grain alone rather than on the photograph.

   `dmax` is the sub-layer's density at full exposure and `npart` the number
   of crystals the pixel covers, already scaled to the pixel size being
   rendered. */
GRAIN_INLINE float filmgrain_sublayer_deviation(float density,
                                                float share,
                                                float dmin,
                                                float dmax,
                                                float npart,
                                                float unif,
                                                uint32_t seed)
{
  const float d_abs = density * share + dmin;
  return grain_layer_particle(d_abs, dmax, npart, unif, seed) - d_abs;
}

/* Exposure change, in EV, for one channel from the three dye layers' grain.

   The layers' mean carries the luminance grain and each layer's departure
   from that mean carries its colour grain, so `chroma` can blend from the
   achromatic grain of a black & white emulsion (0) to fully independent dye
   layers (1) and past that. `k_lum` and `k_chroma` fold in strength, the
   density-to-EV scale and the normalisations that keep both parts at the
   deviation of a single layer; see _spektra_gains in filmgrain.c. */
GRAIN_INLINE float filmgrain_channel_ev(float g0,
                                        float g1,
                                        float g2,
                                        float g_self,
                                        float k_lum,
                                        float k_chroma)
{
  const float sum = g0 + g1 + g2;
  const float mean = sum / 3.0f;
  const float lum = k_lum * sum;
  const float chroma = k_chroma * (g_self - mean);
  return lum + chroma;
}

/* Luminance of a working-profile RGB pixel from the Y row of the profile's
   RGB to XYZ matrix. Only valid for a linear working profile; filmgrain.c
   keeps the device path off for any other. */
GRAIN_INLINE float filmgrain_luminance(float r,
                                       float g,
                                       float b,
                                       float coeff_r,
                                       float coeff_g,
                                       float coeff_b)
{
  const float pr = coeff_r * r;
  const float pg = coeff_g * g;
  const float pb = coeff_b * b;
  return pr + pg + pb;
}

/* How much of the grain a tone gets, from the tone curve table, at a
   position `t` in [0, 1] along the characteristic curve (a density divided
   by FILMGRAIN_DMAX). Linear between table entries. */
GRAIN_INLINE float filmgrain_tone_factor(FILMGRAIN_GMEM const float *lut, float t)
{
  const float pos = grain_clampf(t, 0.0f, 1.0f) * (float)(FILMGRAIN_TONE_LUT_SIZE - 1);
  int i = (int)pos;
  if(i > FILMGRAIN_TONE_LUT_SIZE - 2) i = FILMGRAIN_TONE_LUT_SIZE - 2;
  const float frac = pos - (float)i;
  const float lo = lut[i] * (1.0f - frac);
  const float hi = lut[i + 1] * frac;
  return lo + hi;
}
