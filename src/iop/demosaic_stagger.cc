/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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

#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef _OPENMP
#include <omp.h>
#endif

inline float normclamp(float a) {
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return a;
}

inline int sign(float a) {
    if (a > 0.0f) return 1;
    if (a < 0.0f) return -1;
    return 0;
}

////////////////////////////////////////////////////////////////
//
//			Stagger demosaic algorithm
//                  copyright (c) 2014 Gert van der Plas <gertvdplas@gmail.com>
//
//     experimental high iso demosaic code
////////////////////////////////////////////////////////////////

/** 
 * \brief high-iso demosaic code, edge-aware nearest neighbors interpolation on 
 *        pixel corners.
 * 
 * Demosaic-code for Bayer array. At array pixel corners r,g,b values are calculated.
 * r,b values for an interpolation point p are bilinear interpolated from the 
 * three closest r, b pixels. The g-value for an interpolation point p are linear 
 * interpolated from the two nearest green pixels, g2 and g3. g1 and g4 are 
 * the neigbors of resp. g2 and g3 lying on a straight line through g2 and g3.
 * 
 * Edge-awareness is implemented by estimating the average green value G at interpolation
 * point p for line 1 going through points g1;g2 and line 2 through points g3;g4.
 * Values r,g,b are scaled in ratio for thrs*G/g. 
 * 
 * TODO: - Edge-awareness for red and blue.
 *       - Borders. 
 *       - Highlight reconstruction needs manual adjustment when thrs!=0.0f.
 * 
 * in, out are a 4 floats map RGBX, with X currently unused.
 * dt_iop_roi_t is type from develop/pixelpipe_hb.h
 * 
 */
static void
demosaic_stagger(float *out, const float *in, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in, const int filters, const float thrs) {


#ifdef _OPENMP
#pragma omp parallel default(none) shared(in,out,roi_out,roi_in,stdout)
#endif
    {
        const int wonx = roi_out->x;
        const int wony = roi_out->y;
        const int wonw = roi_out->width;
        //const int wonh = roi_out->height;
        const int winx = roi_in->x;
        const int winy = roi_in->y;
        const int winw = roi_in->width;
        const int winh = roi_in->height;

        //offset of R pixel within a Bayer quartet
        int ex, ey;

        //determine GRBG coset; (ey,ex) is the offset of the R sub-array
        if (FC(winy, winx, filters) == 1) //first pixel is G
        {
            if (FC(winy + 1, winx + 1, filters) == 0) {
                ex = 1;
                ey = 0;
            } else {
                ex = 0;
                ey = 1;
            }
        } else //first pixel is R or B
        {
            if (FC(winy, winx, filters) == 0) {
                ex = 0;
                ey = 0;
            } else {
                ex = 1;
                ey = 1;
            }
        }

        //printf(" ex, ey %d , %d \n", ex, ey);
        //fflush(stdout);

        //Working test code for copying Bayer data directly into output data
        //demostrates window parameters use and confirms basic working.
        //      for (int j = ey + wony; j < wony + winh - 1; j++)
        //        for (int i = ex + wonx; i < wonx + winw - 1; i++) {
        //            out[4 * (j * wonw + i)] = in[(j * winw + i)];
        //            out[4 * (j * wonw + i)+1] = in[(j * winw + i)];
        //            out[4 * (j * wonw + i)+2] = in[(j * winw + i)];
        //        }

        /** Edge unaware staggered nearest neighbor interpolation rgb neighbors are resp. 3,2,3 */

        /*
        for (int j = ey + wony; j < wony + winh; j += 2)
            for (int i = ex + wonx; i < wonx + winw; i += 2) {
                //Bayer pattern is 2x2 repeating, thus 4 quadrants to interpolate q00, q10, q01,q11.
                if (j > 0 && i > 0 && j < winh - 3 && i < winw - 3) {
                    //demosaic q00, r 
                    out[(size_t) (i + j * wonw)*4] = (2.0f * in[i + j * (winw)] + in[i + 2 + j * (winw)] + in[i + (j + 2)*(winw)]) / 4.0f;
                    //demosaic q00, g
                    out[(size_t) 1 + (i + j * wonw)*4] = (in[1 + i + j * (winw)] + in[i + (j + 1)*(winw)]) / 2.0f;
                    //demosaic q00, b
                    out[(size_t) 2 + (i + j * wonw)*4] = (2.0f * in[1 + i + (j + 1)*(winw)] + in[1 + i + (j - 1)*(winw)] + in[i - 1 + (j + 1)*(winw)]) / 4.0f;

                    //demosaic q10, r 
                    out[(size_t) 4 * (1 + i + j * wonw)] = (2.0f * in[i + 2 + j * (winw)] + in[i + j * (winw)] + in[i + 2 + (j + 2)*(winw)]) / 4.0f;
                    //demosaic q10, g
                    out[(size_t) 1 + (1 + i + j * wonw)*4] = (in[1 + i + j * (winw)] + in[i + 2 + (j + 1)*(winw)]) / 2.0f;
                    //demosaic q10, b
                    out[(size_t) 2 + (1 + i + j * wonw)*4] = (2.0f * in[1 + i + (1 + j)*(winw)] + in[i + 1 + (j - 1)*(winw)] + in[i + 3 + (j + 1)*(winw)]) / 4.0f;

                    //demosaic q01, r 
                    out[(size_t) (i + (j + 1) * wonw)*4] = (2.0f * in[i + (2 + j)*(winw)] + in[i + j * (winw)] + in[2 + i + (2 + j)*(winw)]) / 4.0f;
                    //demosaic q01, g
                    out[(size_t) 1 + (i + (j + 1) * wonw)*4] = (in[i + (j + 1)*(winw)] + in[1 + i + (j + 2)*(winw)]) / 2.0f;
                    //demosaic q01, b
                    out[(size_t) 2 + (i + (j + 1) * wonw)*4] = (2.0f * in[1 + i + (j + 1)*(winw)] + in[1 + i + (j + 3)*(winw)] + in[i - 1 + (j + 1)*(winw)]) / 4.0f;

                    //demosaic q11, r 
                    out[(size_t) (i + 1 + (j + 1) * wonw)*4] = (2.0f * in[2 + i + (2 + j)*(winw)] + in[i + (2 + j)*(winw)] + in[2 + i + j * (winw)]) / 4.0f;
                    //demosaic q01, g
                    out[(size_t) 1 + (i + 1 + (j + 1) * wonw)*4] = (in[1 + i + (j + 2)*(winw)] + in[2 + i + (j + 1)*(winw)]) / 2.0f;
                    //demosaic q01, b
                    out[(size_t) 2 + (i + 1 + (j + 1) * wonw)*4] = (2.0f * in[1 + i + (j + 1)*(winw)] + in[1 + i + (j + 3)*(winw)] + in[3 + i + (j + 1)*(winw)]) / 4.0f;

                } else { // write zero into burder.
                    out[(size_t) (i + j * wonw)*4] = 0;
                    out[1 + (size_t) (i + j * wonw)*4] = 0;
                    out[2 + (size_t) (i + j * wonw)*4] = 0;
                }

            }
         */


        /** Edge-aware staggered nearest neigbour interpolation rgb neigbours are resp. 3,2,3 with green edge improvement*/

        //main body




#ifdef _OPENMP
#pragma omp for
#endif
        for (int j = ey + wony; j < wony + winh; j += 2)

            for (int i = ex + wonx; i < wonx + winw; i += 2) {
                //Bayer pattern is 2x2 repeating, thus 4 quadrants to interpolate q00, q10, q01,q11.
                if (j > 1 && i > 1 && j < winh - 3 && i < winw - 3) {
                    //demosaic q00, r 
                    float r = (2.0f * in[i + j * (winw)] + in[i + 2 + j * (winw)] + in[i + (j + 2)*(winw)]) / 4.0f;
                    //demosaic q00, g
                    float g = (in[1 + i + j * (winw)] + in[i + (j + 1)*(winw)]) / 2.0f;

                    float comp = 0.0f;
                    if (thrs != 0.0f && g > 0.01f) {
                        float g1, g2, g3, g4;
                        g4 = in[2 + i + (j - 1)* (winw)];
                        g3 = in[1 + i + j * (winw)];
                        g2 = in[i + (j + 1)* (winw)];
                        g1 = in[ i - 1 + (j + 2) * (winw)];
                        // by averaging the heights of lines 1 and 2 at interpolation 
                        // point p, the code gains more stability.
                        comp = (g2 + g3 - g1 - g4) / 4.0f / g;
                    }
                    //demosaic q00, b
                    float b = (2.0f * in[1 + i + (j + 1)*(winw)] + in[1 + i + (j - 1)*(winw)] + in[i - 1 + (j + 1)*(winw)]) / 4.0f;

                    //scale comp for thrs
                    comp = 10.0f * thrs * g * comp; // / (r + g + b);
                    // extremes protection
                    float d = 0.0f;
                    float max = MAX(r * (1.0f + comp), g * (1.0f + comp));
                    max = MAX(max, b * (1.0f + comp));
                    float min = MIN(r * (1.0f + comp), g * (1.0f + comp));
                    min = MIN(max, b * (1.0f + comp));
                    if (max > 1.0f) {
                        d = max;
                    } else if (min < 0.0f) {
                        d = min;
                    };

                    out[(size_t) (i + (j) * wonw)*4] = normclamp(r * (1.0f + comp) - d);
                    out[(size_t) 1 + (i + (j) * wonw)*4] = normclamp(g * (1.0f + comp) - d);
                    out[(size_t) 2 + (i + (j) * wonw)*4] = normclamp(b * (1.0f + comp) - d);

                    //demosaic q10, r 
                    r = (2.0f * in[i + 2 + j * (winw)] + in[i + j * (winw)] + in[i + 2 + (j + 2)*(winw)]) / 4.0f;
                    //demosaic q10, g
                    g = (in[1 + i + j * (winw)] + in[i + 2 + (j + 1)*(winw)]) / 2.0f;

                    comp = 0.0f;
                    if (thrs != 0.0f && g > 0.01f) {
                        float g1, g2, g3, g4;
                        g4 = in[3 + i + (j + 2)* (winw)];
                        g3 = in[2 + i + (j + 1) * (winw)];
                        g2 = in[1 + i + (j)* (winw)];
                        g1 = in[ i + (j - 1) * (winw)];
                        // by averaging the heights of lines 1 and 2 at interpolation 
                        // point p, the code gains more stability.
                        comp = (g2 + g3 - g1 - g4) / 4.0f / g;
                    }

                    //demosaic q10, b
                    b = (2.0f * in[1 + i + (1 + j)*(winw)] + in[i + 1 + (j - 1)*(winw)] + in[i + 3 + (j + 1)*(winw)]) / 4.0f;

                    //scale comp for thrs
                    comp = 10.0f * thrs * g * comp; // / (r + g + b);
                    // extremes protection
                    d = 0.0f;
                    max = MAX(r * (1.0f + comp), g * (1.0f + comp));
                    max = MAX(max, b * (1.0f + comp));
                    min = MIN(r * (1.0f + comp), g * (1.0f + comp));
                    min = MIN(max, b * (1.0f + comp));
                    if (max > 1.0f) {
                        d = max;
                    }
                    if (min < 0.0f) {
                        d = min;
                    };

                    out[(size_t) (1 + i + (j) * wonw)*4] = normclamp(r * (1.0f + comp) - d);
                    out[(size_t) 1 + (1 + i + (j) * wonw)*4] = normclamp(g * (1.0f + comp) - d);
                    out[(size_t) 2 + (1 + i + (j) * wonw)*4] = normclamp(b * (1.0f + comp) - d);

                    //demosaic q01, r 
                    r = (2.0f * in[i + (2 + j)*(winw)] + in[i + j * (winw)] + in[2 + i + (2 + j)*(winw)]) / 4.0f;
                    //demosaic q01, g
                    g = (in[i + (j + 1)*(winw)] + in[1 + i + (j + 2)*(winw)]) / 2.0f;

                    comp = 0.0f;
                    if (thrs != 0.0f && g > 0.01f) {
                        float g1, g2, g3, g4;

                        g4 = in[2 + i + (j + 3)* (winw)];
                        g3 = in[1 + i + (j + 2) * (winw)];
                        g2 = in[i + (j + 1)* (winw)];
                        g1 = in[ i - 1 + (j) * (winw)];
                        // by averaging the heights of lines 1 and 2 at interpolation 
                        // point p, the code gains more stability.
                        comp = (g2 + g3 - g1 - g4) / 4.0f / g;

                    }
                    //demosaic q01, b
                    b = (2.0f * in[1 + i + (j + 1)*(winw)] + in[1 + i + (j + 3)*(winw)] + in[i - 1 + (j + 1)*(winw)]) / 4.0f;

                    //scale comp for thrs
                    comp = 10.0f * thrs * g * comp; // / (r + g + b);
                    // extremes protection
                    d = 0.0f;
                    max = MAX(r * (1.0f + comp), g * (1.0f + comp));
                    max = MAX(max, b * (1.0f + comp));
                    min = MIN(r * (1.0f + comp), g * (1.0f + comp));
                    min = MIN(max, b * (1.0f + comp));
                    if (max > 1.0f) {
                        d = max;
                    }
                    if (min < 0.0f) {
                        d = min;
                    };

                    out[(size_t) (i + (j + 1) * wonw)*4] = normclamp(r * (1.0f + comp) - d);
                    out[(size_t) 1 + (i + (j + 1) * wonw)*4] = normclamp(g * (1.0f + comp) - d);
                    out[(size_t) 2 + (i + (j + 1) * wonw)*4] = normclamp(b * (1.0f + comp) - d);

                    //demosaic q11, r 
                    r = (2.0f * in[2 + i + (2 + j)*(winw)] + in[i + (2 + j)*(winw)] + in[2 + i + j * (winw)]) / 4.0f;
                    //demosaic q11, g
                    g = (in[1 + i + (j + 2)*(winw)] + in[2 + i + (j + 1)*(winw)]) / 2.0f;

                    comp = 0.0f;
                    if (thrs != 0.0f && g > 0.01f) {
                        float g1, g2, g3, g4;
                        g4 = in[3 + i + (j)* (winw)];
                        g3 = in[2 + i + (j + 1) * (winw)];
                        g2 = in[1 + i + (j + 2)* (winw)];
                        g1 = in[ i + (j + 3) * (winw)];
                        // by averaging the heights of lines 1 and 2 at interpolation 
                        // point p, the code gains more stability.
                        comp = (g2 + g3 - g1 - g4) / 4.0f / g;
                    }

                    //demosaic q11, b
                    b = (2.0f * in[1 + i + (j + 1)*(winw)] + in[1 + i + (j + 3)*(winw)] + in[3 + i + (j + 1)*(winw)]) / 4.0f;

                    //scale comp for thrs
                    comp = 10.0f * thrs * g * comp; // / (r + g + b);
                    // extremes protection
                    d = 0.0f;
                    max = MAX(r * (1.0f + comp), g * (1.0f + comp));
                    max = MAX(max, b * (1.0f + comp));
                    min = MIN(r * (1.0f + comp), g * (1.0f + comp));
                    min = MIN(max, b * (1.0f + comp));
                    if (max > 1.0f) {
                        d = max;
                    }
                    if (min < 0.0f) {
                        d = min;
                    };

                    out[(size_t) (i + 1 + (j + 1) * wonw)*4] = normclamp(r * (1.0f + comp) - d);
                    out[(size_t) 1 + (i + 1 + (j + 1) * wonw)*4] = normclamp(g * (1.0f + comp) - d);
                    out[(size_t) 2 + (i + 1 + (j + 1) * wonw)*4] = normclamp(b * (1.0f + comp) - d);

                } else {
                    out[(size_t) (i + (j) * wonw)*4] = 0;
                    out[1 + (size_t) (i + (j) * wonw)*4] = 0;
                    out[2 + (size_t) (i + (j) * wonw)*4] = 0;
                }

            }

        //copied from ppg border interpolate
        const int offx = MAX(ex + wonx, 3); //MAX(0, 3 - roi_out->x);
        const int offy = MAX(ey + wony, 3); //MAX(0, 3 - roi_out->y);
        const int offX = 3; //MAX(0, 3 - (roi_in->width  - (roi_out->x + roi_out->width)));
        const int offY = 3; //MAX(0, 3 - (roi_in->height - (roi_out->y + roi_out->height)));

        float sum[8];
        for (int j = 0; j < roi_out->height; j++) for (int i = 0; i < roi_out->width; i++) {
                if (i == offx && j >= offy && j < roi_out->height - offY)
                    i = roi_out->width - offX;
                if (i == roi_out->width) break;
                memset(sum, 0, sizeof (float)*8);
                for (int y = j - 1; y != j + 2; y++) for (int x = i - 1; x != i + 2; x++) {
                        const int yy = y + roi_out->y, xx = x + roi_out->x;
                        if (yy >= 0 && xx >= 0 && yy < roi_in->height && xx < roi_in->width) {
                            int f = FC(y, x, filters);
                            sum[f] += in[yy * roi_in->width + xx];
                            sum[f + 4]++;
                        }
                    }
                int f = FC(j, i, filters);
                for (int c = 0; c < 3; c++) {
                    if (c != f && sum[c + 4] > 0.0f)
                        out[4 * (j * roi_out->width + i) + c] = sum[c] / sum[c + 4];
                    else
                        out[4 * (j * roi_out->width + i) + c] = in[(j + roi_out->y) * roi_in->width + i + roi_out->x];
                }
            }
    }// end of parallelization

}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
