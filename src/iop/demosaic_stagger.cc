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

static void
demosaic_stagger(float *out, const float *in, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in, const int filters, const float thrs) {
    //NOTES
    // in, out is 4 floats RGBX, with X unused.
    //  dt_iop_roi_t is type from develop/pixelpipe_hb.h


    int wonx = roi_out->x;
    int wony = roi_out->y;
    int wonw = roi_out->width;
    //    int wonh = roi_out->height;
    int winx = roi_in->x;
    int winy = roi_in->y;
    int winw = roi_in->width;
    int winh = roi_in->height;



    //printf(" winx %d winy %d winw %d winh %d wonx %d wony %d wonw %d wonh %d\n",
    //        winx, winy, winw, winh, wonx, wony, wonw, wonh);
    //fflush(stdout);

    //const float clip_pt = fminf(piece->pipe->processed_maximum[0], fminf(piece->pipe->processed_maximum[1], piece->pipe->processed_maximum[2]));


    //offset of R pixel within a Bayer quartet
    int ex, ey;

    //tolerance to avoid dividing by zero
    //  static const float eps=1e-5, epssq=1e-10;			//tolerance to avoid dividing by zero


    //determine GRBG coset; (ey,ex) is the offset of the R subarray
    if (FC(winx, winy, filters) == 1) //first pixel is G
    {
        if (FC(winx, winy + 1, filters) == 0) {
            ex = 1;
            ey = 0;
        } else {
            ex = 0;
            ey = 1;
        }
    } else //first pixel is R or B
    {
        if (FC(winx, winy, filters) == 0) {
            ex = 0;
            ey = 0;
        } else {
            ex = 1;
            ey = 1;
        }
    }

    //printf(" ex, ey %d , %d \n", ex, ey);
    //fflush(stdout);

    //    /** working test code for copying bayer data directly into output data */
    //        for (int j = ey + wony; j < wony + winh - 1; j++)
    //        for (int i = ex + wonx; i < wonx + winw - 1; i++) {
    //            out[4 * (j * wonw + i)] = in[(j * winw + i)];
    //            out[4 * (j * wonw + i)+1] = in[(j * winw + i)];
    //            out[4 * (j * wonw + i)+2] = in[(j * winw + i)];
    //        }

    /** test code for 1 in 4 pixels in for nearest neighbor code */
    /*    for (int j = ey + wony; j < wony + winh - 1; j++)
            for (int i = ex + wonx; i < wonx + winw - 1; i++) {
                if (j % 2 == 0 && i % 2 == 0) {
                    out[4 * ((size_t) j * wonw + i)] = in[(size_t) j * winw + i];
                    out[4 * ((size_t) j * wonw + i) + 1] = in[(size_t) j * winw + i + 1];
                    out[4 * ((size_t) j * wonw + i) + 2] = in[(size_t) (1 + j) * winw + i + 1];

                    //         max = MAX(max, in[(j * (wonx + winw) + i + 1)]);
                } else {
                    out[4 * ((size_t) j * wonw + i)] = 0;
                    out[4 * ((size_t) j * wonw + i) + 1] = 0;
                    out[4 * ((size_t) j * wonw + i) + 2] = 0;
                    out[4 * ((size_t) j * wonw + i) + 3] = 0;
                }
            }
     */

    /** Staggered nearest neigbour interpolation rgb neigbours are resp. 3,2,3 */

    //main body
    /*
    for (int j = ey + wony; j < wony + winh; j += 2)
        for (int i = ex + wonx; i < wonx + winw; i += 2) {
            //Bayer pattern is 2x2 repeating, thus 4 quadrants to interpolate q00, q10, q01,q11.
            if (j > 0 && i > 0 && j < winh - 3 && i < winw - 3) {
                //demosaic q00, r 
                out[(size_t) (i + j * wonw)*4] = (2.0f * in[i + j * (winw)] + in[i + 2 + j * (winw)] + in[i + (j + 2)*(winw)]) / 4.0f;
                //demosaic q00, g
                out[(size_t) 1 + (i + j * wonw)*4] = (in[1 + i + j * (winw)] + in[i + (j + 1)*(winw)]) / 2.0f;
                //out[(size_t) 1 + (i + j * wonw)*4] = MAX(in[1 + i + j * (winw)],in[i + (j + 1)*(winw)]);
                
                //demosaic q00, b
                out[(size_t) 2 + (i + j * wonw)*4] = (2.0f * in[1 + i + (j + 1)*(winw)] + in[1 + i + (j - 1)*(winw)] + in[i - 1 + (j + 1)*(winw)]) / 4.0f;

                //demosaic q10, r 
                out[(size_t) 4 * (1 + i + j * wonw)] = (2.0f * in[i + 2 + j * (winw)] + in[i + j * (winw)] + in[i + 2 + (j + 2)*(winw)]) / 4.0f;
                //demosaic q10, g
                out[(size_t) 1 + (1 + i + j * wonw)*4] = (in[1 + i + j * (winw)] + in[i + 2 + (j + 1)*(winw)]) / 2.0f;
                //out[(size_t) 1 + (1 + i + j * wonw)*4] = MAX(in[1 + i + j * (winw)],in[i + 2 + (j + 1)*(winw)]);
                //demosaic q10, b
                out[(size_t) 2 + (1 + i + j * wonw)*4] = (2.0f * in[1 + i + (1 + j)*(winw)] + in[i + 1 + (j - 1)*(winw)] + in[i + 3 + (j + 1)*(winw)]) / 4.0f;

                //demosaic q01, r 
                out[(size_t) (i + (j + 1) * wonw)*4] = (2.0f * in[i + (2 + j)*(winw)] + in[i + j * (winw)] + in[2 + i + (2 + j)*(winw)]) / 4.0f;
                //demosaic q01, g
                out[(size_t) 1 + (i + (j + 1) * wonw)*4] = (in[i + (j + 1)*(winw)] + in[1 + i + (j + 2)*(winw)]) / 2.0f;
                //out[(size_t) 1 + (i + (j + 1) * wonw)*4] = MAX(in[i + (j + 1)*(winw)],in[1 + i + (j + 2)*(winw)]);
                //demosaic q01, b
                out[(size_t) 2 + (i + (j + 1) * wonw)*4] = (2.0f * in[1 + i + (j + 1)*(winw)] + in[1 + i + (j + 3)*(winw)] + in[i - 1 + (j + 1)*(winw)]) / 4.0f;

                //demosaic q11, r 
                out[(size_t) (i + 1 + (j + 1) * wonw)*4] = (2.0f * in[2 + i + (2 + j)*(winw)] + in[i + (2 + j)*(winw)] + in[2 + i + j * (winw)]) / 4.0f;
                //demosaic q01, g
                out[(size_t) 1 + (i + 1 + (j + 1) * wonw)*4] = (in[1 + i + (j + 2)*(winw)] + in[2 + i + (j + 1)*(winw)]) / 2.0f;
                //out[(size_t) 1 + (i + 1 + (j + 1) * wonw)*4] = MAX(in[1 + i + (j + 2)*(winw)],in[2 + i + (j + 1)*(winw)]);
                //demosaic q01, b
                out[(size_t) 2 + (i + 1 + (j + 1) * wonw)*4] = (2.0f * in[1 + i + (j + 1)*(winw)] + in[1 + i + (j + 3)*(winw)] + in[3 + i + (j + 1)*(winw)]) / 4.0f;

            } else {
                out[(size_t) (i + j * wonw)*4] = 0;
                out[1 + (size_t) (i + j * wonw)*4] = 0;
                out[2 + (size_t) (i + j * wonw)*4] = 0;
            }

        }
     */
    /** Staggered nearest neigbour interpolation rgb neigbours are resp. 3,2,3 with green edge improvement*/

    //main body

    for (int j = ey + wony; j < wony + winh; j += 2)
        for (int i = ex + wonx; i < wonx + winw; i += 2) {
            //Bayer pattern is 2x2 repeating, thus 4 quadrants to interpolate q00, q10, q01,q11.
            if (j > 1 && i > 1 && j < winh - 3 && i < winw - 3) {
                //demosaic q00, r 
                float r = (2.0f * in[i + j * (winw)] + in[i + 2 + j * (winw)] + in[i + (j + 2)*(winw)]) / 4.0f;
                //demosaic q00, g
                float g = (in[1 + i + j * (winw)] + in[i + (j + 1)*(winw)]) / 2.0f;

                float comp = 0.0f;
                if (thrs != 0.0f) {
                    float g1, g2, g3, g4;
                    g4 = in[2 + i + (j - 1)* (winw)];
                    g3 = in[1 + i + j * (winw)];
                    g2 = in[i + (j + 1)* (winw)];
                    g1 = in[ i - 1 + (j + 2) * (winw)];

                    //                        if (abs(g1 - g2) < abs(g4 - g3)) {
                    //                            comp = (g2 - g1) / 2.0f / g;
                    //                        } else {
                    //                            comp = (g3 - g4) / 2.0f / g;
                    //                        };
                    comp = (g2 + g3 - g1 - g4) / 4.0f / g;

                }
                //demosaic q00, b
                float b = (2.0f * in[1 + i + (j + 1)*(winw)] + in[1 + i + (j - 1)*(winw)] + in[i - 1 + (j + 1)*(winw)]) / 4.0f;

                //scale comp for thrs
                comp = 10.0f * thrs * g * comp / (r + g + b);
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

                // could scale comp here

                out[(size_t) (i + j * wonw)*4] = normclamp(r * (1.0f + comp) - d);
                out[(size_t) 1 + (i + j * wonw)*4] = normclamp(g * (1.0f + comp) - d);
                out[(size_t) 2 + (i + j * wonw)*4] = normclamp(b * (1.0f + comp) - d);

                //demosaic q10, r 
                r = (2.0f * in[i + 2 + j * (winw)] + in[i + j * (winw)] + in[i + 2 + (j + 2)*(winw)]) / 4.0f;
                //demosaic q10, g
                g = (in[1 + i + j * (winw)] + in[i + 2 + (j + 1)*(winw)]) / 2.0f;

                comp = 0.0f;
                if (thrs != 0.0f) {
                    float g1, g2, g3, g4;
                    g4 = in[3 + i + (j + 2)* (winw)];
                    g3 = in[2 + i + (j + 1) * (winw)];
                    g2 = in[1 + i + (j)* (winw)];
                    g1 = in[ i + (j - 1) * (winw)];

                    //                        if (abs(g1 - g2) < abs(g4 - g3)) {
                    //                            comp = (g2 - g1) / 2.0f / g;
                    //                        } else {
                    //                            comp = (g3 - g4) / 2.0f / g;
                    //                        };
                    comp = (g2 + g3 - g1 - g4) / 4.0f / g;
                }

                //demosaic q10, b
                b = (2.0f * in[1 + i + (1 + j)*(winw)] + in[i + 1 + (j - 1)*(winw)] + in[i + 3 + (j + 1)*(winw)]) / 4.0f;

                //scale comp for thrs
                comp = 10.0f * thrs * g * comp / (r + g + b);
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

                // could scale comp here

                out[(size_t) (1 + i + j * wonw)*4] = normclamp(r * (1.0f + comp) - d);
                out[(size_t) 1 + (1 + i + j * wonw)*4] = normclamp(g * (1.0f + comp) - d);
                out[(size_t) 2 + (1 + i + j * wonw)*4] = normclamp(b * (1.0f + comp) - d);

                //demosaic q01, r 
                r = (2.0f * in[i + (2 + j)*(winw)] + in[i + j * (winw)] + in[2 + i + (2 + j)*(winw)]) / 4.0f;
                //demosaic q01, g
                g = (in[i + (j + 1)*(winw)] + in[1 + i + (j + 2)*(winw)]) / 2.0f;

                comp = 0.0f;
                if (thrs != 0.0f) {
                    float g1, g2, g3, g4;

                    g4 = in[2 + i + (j + 3)* (winw)];
                    g3 = in[1 + i + (j + 2) * (winw)];
                    g2 = in[i + (j + 1)* (winw)];
                    g1 = in[ i - 1 + (j) * (winw)];

                    //                        if (abs(g1 - g2) < abs(g4 - g3)) {
                    //                            comp = (g2 - g1) / 2.0f / g;
                    //                        } else {
                    //                            comp = (g3 - g4) / 2.0f / g;
                    //                        };
                    comp = (g2 + g3 - g1 - g4) / 4.0f / g;

                }


                //demosaic q01, b
                b = (2.0f * in[1 + i + (j + 1)*(winw)] + in[1 + i + (j + 3)*(winw)] + in[i - 1 + (j + 1)*(winw)]) / 4.0f;

                //scale comp for thrs
                comp = 10.0f * thrs * g * comp / (r + g + b);
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

                // could scale comp here

                out[(size_t) (i + (j + 1) * wonw)*4] = normclamp(r * (1.0f + comp) - d);
                out[(size_t) 1 + (i + (j + 1) * wonw)*4] = normclamp(g * (1.0f + comp) - d);
                out[(size_t) 2 + (i + (j + 1) * wonw)*4] = normclamp(b * (1.0f + comp) - d);

                //demosaic q11, r 
                r = (2.0f * in[2 + i + (2 + j)*(winw)] + in[i + (2 + j)*(winw)] + in[2 + i + j * (winw)]) / 4.0f;
                //demosaic q11, g
                g = (in[1 + i + (j + 2)*(winw)] + in[2 + i + (j + 1)*(winw)]) / 2.0f;

                comp = 0.0f;
                if (thrs != 0.0f) {
                    float g1, g2, g3, g4;
                    g4 = in[3 + i + (j)* (winw)];
                    g3 = in[2 + i + (j + 1) * (winw)];
                    g2 = in[1 + i + (j + 2)* (winw)];
                    g1 = in[ i + (j + 3) * (winw)];

                    //                        if (abs(g1 - g2) < abs(g4 - g3)) {
                    //                            comp = (g2 - g1) / 2.0f / g;
                    //                        } else {
                    //                            comp = (g3 - g4) / 2.0f / g;
                    //                        };
                    comp = (g2 + g3 - g1 - g4) / 4.0f / g;
                }

                //demosaic q11, b
                b = (2.0f * in[1 + i + (j + 1)*(winw)] + in[1 + i + (j + 3)*(winw)] + in[3 + i + (j + 1)*(winw)]) / 4.0f;

                //scale comp for thrs
                comp = 10.0f * thrs * g * comp / (r + g + b);
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
                // could scale comp here

                out[(size_t) (i + 1 + (j + 1) * wonw)*4] = normclamp(r * (1.0f + comp) - d);
                out[(size_t) 1 + (i + 1 + (j + 1) * wonw)*4] = normclamp(g * (1.0f + comp) - d);
                out[(size_t) 2 + (i + 1 + (j + 1) * wonw)*4] = normclamp(b * (1.0f + comp) - d);

            } else {
                out[(size_t) (i + j * wonw)*4] = 0;
                out[1 + (size_t) (i + j * wonw)*4] = 0;
                out[2 + (size_t) (i + j * wonw)*4] = 0;
            }

        }

    //TODO     // top border from 0 to winsize
    //        for (int i = ex + wonx; i < wonx + winw; i += 2) {
    //            int j =0;
    //            //demosaic q00, r 
    //            out[(size_t) (i + j * wonw)*4] = (2.0f * in[i + j * (winw)] + in[i + 2 + j * (winw)] + in[i + (j + 2)*(winw)]) / 4.0f;
    //            //demosaic q00, g
    //            out[(size_t) 1 + (i + j * wonw)*4] = (in[1 + i + j * (winw)] + in[i + (j + 1)*(winw)]) / 2.0f;
    //            //demosaic q00, b
    //            out[(size_t) 2 + (i + j * wonw)*4] = in[1 + i + (j + 1)*(winw)];
    //
    //            //demosaic q10, r 
    //            out[(size_t) 4 * (1 + i + j * wonw)] = (2.0f * in[i + 2 + j * (winw)] + in[i + j * (winw)] + in[i + 2 + (j + 2)*(winw)]) / 4.0f;
    //            //demosaic q10, g
    //            out[(size_t) 1 + (1 + i + j * wonw)*4] = (in[1 + i + j * (winw)] + in[i + 2 + (j + 1)*(winw)]) / 2.0f;
    //            //demosaic q10, b
    //            out[(size_t) 2 + (1 + i + j * wonw)*4] = in[1 + i + (1 + j)*(winw)];
    //        }
    //
    //    // bottom border from 0 to wonw
    //     for (int j = ey + wony; j < wony + winh; j += 2)
    //        for (int i = ex + wonx; i < wonx + winw; i += 2) {
    //           if (j > 0 && i > 0 j < winh - 3 && i < winw - 3) {
    // 
    //                 //demosaic q00, r 
    //                out[(size_t) (i + j * wonw)*4] = (2.0f * in[i + j * (winw)] + in[i + 2 + j * (winw)] + in[i + (j + 2)*(winw)]) / 4.0f;
    //                //demosaic q00, g
    //                out[(size_t) 1 + (i + j * wonw)*4] = (in[1 + i + j * (winw)] + in[i + (j + 1)*(winw)]) / 2.0f;
    //                //demosaic q00, b
    //                out[(size_t) 2 + (i + j * wonw)*4] = (2.0f * in[1 + i + (j + 1)*(winw)] + in[1 + i + (j - 1)*(winw)] + in[i - 1 + (j + 1)*(winw)]) / 4.0f;
    //
    //                //demosaic q10, r 
    //                out[(size_t) 4 * (1 + i + j * wonw)] = (2.0f * in[i + 2 + j * (winw)] + in[i + j * (winw)] + in[i + 2 + (j + 2)*(winw)]) / 4.0f;
    //                //demosaic q10, g
    //                out[(size_t) 1 + (1 + i + j * wonw)*4] = (in[1 + i + j * (winw)] + in[i + 2 + (j + 1)*(winw)]) / 2.0f;
    //                //demosaic q10, b
    //                out[(size_t) 2 + (1 + i + j * wonw)*4] = (2.0f * in[1 + i + (1 + j)*(winw)] + in[i + 1 + (j - 1)*(winw)] + in[i + 3 + (j + 1)*(winw)]) / 4.0f;
    //
    //                //demosaic q01, r 
    //                out[(size_t) (i + (j + 1) * wonw)*4] = (2.0f * in[i + (2 + j)*(winw)] + in[i + j * (winw)] + in[2 + i + (2 + j)*(winw)]) / 4.0f;
    //                //demosaic q01, g
    //                out[(size_t) 1 + (i + (j + 1) * wonw)*4] = (in[i + (j + 1)*(winw)] + in[1 + i + (j + 2)*(winw)]) / 2.0f;
    //                //demosaic q01, b
    //                out[(size_t) 2 + (i + (j + 1) * wonw)*4] = (2.0f * in[1 + i + (j + 1)*(winw)] + in[1 + i + (j + 3)*(winw)] + in[i - 1 + (j + 1)*(winw)]) / 4.0f;
    //
    //                //demosaic q11, r 
    //                out[(size_t) (i + 1 + (j + 1) * wonw)*4] = (2.0f * in[2 + i + (2 + j)*(winw)] + in[i + (2 + j)*(winw)] + in[2 + i + j * (winw)]) / 4.0f;
    //                //demosaic q01, g
    //                out[(size_t) 1 + (i + 1 + (j + 1) * wonw)*4] = (in[1 + i + (j + 2)*(winw)] + in[2 + i + (j + 1)*(winw)]) / 2.0f;
    //                //demosaic q01, b
    //                out[(size_t) 2 + (i + 1 + (j + 1) * wonw)*4] = (2.0f * in[1 + i + (j + 1)*(winw)] + in[1 + i + (j + 3)*(winw)] + in[3 + i + (j + 1)*(winw)]) / 4.0f;
    //           }
    //    }

}



// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
