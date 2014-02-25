
/*==================================================================================
 * begin raw therapee code, hg checkout of june 04, 2013 branch master.
 *==================================================================================*/


#ifdef _OPENMP
#include <omp.h>
#endif

static void
demosaic_igv_RT(struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, const float *const in, float *out, const dt_iop_roi_t * const roi_in, const dt_iop_roi_t * const roi_out, const int filters) {
#define SQR(x) ((x)*(x))
    //#define MIN(a,b) ((a) < (b) ? (a) : (b))
    //#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define LIM(x,min,max) MAX(min,MIN(x,max))
#define ULIM(x,y,z) ((y) < (z) ? LIM(x,y,z) : LIM(x,z,y))
#define CLIP(x) LIM(x,0,65535)
#define HCLIP(x) x //is this still necessary???
    //MIN(clip_pt,x)

    //    int winx = roi_out->x;
    //    int winy = roi_out->y;
    int winw = roi_in->width;
    int winh = roi_in->height;

    //    int wonx = roi_out->x;
    //    int wony = roi_out->y;
    const int wonw = roi_out->width;
    //    int wonh = roi_out->height;    


    static const float eps = 1e-5f, epssq = 1e-5f; //mod epssq -10f =>-5f Jacques 3/2013 to prevent artifact (divide by zero)
    static const int h1 = 1, h2 = 2, h3 = 3, h4 = 4, h5 = 5, h6 = 6;
    const int width = winw, height = winh;
    const int v1 = 1 * width, v2 = 2 * width, v3 = 3 * width, v4 = 4 * width, v5 = 5 * width, v6 = 6 * width;
    float* rgb[3];
    float* chr[2];
    float (*rgbarray), *vdif, *hdif, (*chrarray);

    rgbarray = (float (*)) calloc(width * height * 3, sizeof ( float));
    rgb[0] = rgbarray;
    rgb[1] = rgbarray + (width * height);
    rgb[2] = rgbarray + 2 * (width * height);

    chrarray = (float (*)) calloc(width * height * 2, sizeof ( float));
    chr[0] = chrarray;
    chr[1] = chrarray + (width * height);

    vdif = (float (*)) calloc(width * height / 2, sizeof *vdif);
    hdif = (float (*)) calloc(width * height / 2, sizeof *hdif);

    //    border_interpolate2(winw, winh, 7);

    //    if (plistener) {
    //        plistener->setProgressStr(Glib::ustring::compose(M("TP_RAW_DMETHOD_PROGRESSBAR"), RAWParams::methodstring[RAWParams::igv]));
    //        plistener->setProgress(0.0);
    //    }
#ifdef _OPENMP
#pragma omp parallel default(none) shared(rgb,vdif,hdif,chr,out,stdout)
#endif
    {

        float ng, eg, wg, sg, nv, ev, wv, sv, nwg, neg, swg, seg, nwv, nev, swv, sev;
#ifdef _OPENMP
#pragma omp single
#endif
        printf("Going to copy in");
        fflush(stdout);

        //#ifdef _OPENMP
        //#pragma omp for
        //#endif
        for (int row = 0; row < height - 0; row++) {
            for (int col = 0, indx = row * width + col; col < width - 0; col++, indx++) {
                int c = FC(row, col, filters);
 //           printf("c %d, row %d, col %d", c, row, col);
 //           fflush(stdout);

                //                rgb[c][indx] = CLIP(rawData[row][col]); //rawData = RT datas
            //TODO what does FC in RT and such
                rgb[c][indx] = in[indx]; //darktable input data                                        
            }
        }
        //	border_interpolate2(7, rgb);

#ifdef _OPENMP
#pragma omp single
#endif

        printf("in copied, ready to rumble");
        fflush(stdout);

        //        {
        //            if (plistener) plistener->setProgress(0.13);
        //       }


#ifdef _OPENMP
#pragma omp for
#endif
        for (int row = 5; row < height - 5; row++)
            for (int col = 5 + (FC(row, 1, filters)&1), indx = row * width + col, c = FC(row, col, filters); col < width - 5; col += 2, indx += 2) {
                //N,E,W,S Gradients
                ng = (eps + (fabsf(rgb[1][indx - v1] - rgb[1][indx - v3]) + fabsf(rgb[c][indx] - rgb[c][indx - v2])) / 65535.f);
                ;
                eg = (eps + (fabsf(rgb[1][indx + h1] - rgb[1][indx + h3]) + fabsf(rgb[c][indx] - rgb[c][indx + h2])) / 65535.f);
                wg = (eps + (fabsf(rgb[1][indx - h1] - rgb[1][indx - h3]) + fabsf(rgb[c][indx] - rgb[c][indx - h2])) / 65535.f);
                sg = (eps + (fabsf(rgb[1][indx + v1] - rgb[1][indx + v3]) + fabsf(rgb[c][indx] - rgb[c][indx + v2])) / 65535.f);
                //N,E,W,S High Order Interpolation (Li & Randhawa)  
                //N,E,W,S Hamilton Adams Interpolation
                // (48.f * 65535.f) = 3145680.f
                nv = LIM(((23.0f * rgb[1][indx - v1] + 23.0f * rgb[1][indx - v3] + rgb[1][indx - v5] + rgb[1][indx + v1] + 40.0f * rgb[c][indx] - 32.0f * rgb[c][indx - v2] - 8.0f * rgb[c][indx - v4])) / 3145680.f, 0.0f, 1.0f);
                ev = LIM(((23.0f * rgb[1][indx + h1] + 23.0f * rgb[1][indx + h3] + rgb[1][indx + h5] + rgb[1][indx - h1] + 40.0f * rgb[c][indx] - 32.0f * rgb[c][indx + h2] - 8.0f * rgb[c][indx + h4])) / 3145680.f, 0.0f, 1.0f);
                wv = LIM(((23.0f * rgb[1][indx - h1] + 23.0f * rgb[1][indx - h3] + rgb[1][indx - h5] + rgb[1][indx + h1] + 40.0f * rgb[c][indx] - 32.0f * rgb[c][indx - h2] - 8.0f * rgb[c][indx - h4])) / 3145680.f, 0.0f, 1.0f);
                sv = LIM(((23.0f * rgb[1][indx + v1] + 23.0f * rgb[1][indx + v3] + rgb[1][indx + v5] + rgb[1][indx - v1] + 40.0f * rgb[c][indx] - 32.0f * rgb[c][indx + v2] - 8.0f * rgb[c][indx + v4])) / 3145680.f, 0.0f, 1.0f);
                //Horizontal and vertical color differences
                vdif[indx >> 1] = (sg * nv + ng * sv) / (ng + sg)-(rgb[c][indx]) / 65535.f;
                hdif[indx >> 1] = (wg * ev + eg * wv) / (eg + wg)-(rgb[c][indx]) / 65535.f;
            }

        //#ifdef _OPENMP
        //#pragma omp single
        //#endif
        //        {
        //            if (plistener) plistener->setProgress(0.26);
        //        }

#ifdef _OPENMP
#pragma omp for
#endif
        for (int row = 7; row < height - 7; row++)
            for (int col = 7 + (FC(row, 1, filters)&1), indx = row * width + col, c = FC(row, col, filters), d = c / 2; col < width - 7; col += 2, indx += 2) {
                //H&V integrated gaussian vector over variance on color differences
                //Mod Jacques 3/2013
                ng = LIM(epssq + 78.0f * SQR(vdif[indx >> 1]) + 69.0f * (SQR(vdif[(indx - v2) >> 1]) + SQR(vdif[(indx + v2) >> 1])) + 51.0f * (SQR(vdif[(indx - v4) >> 1]) + SQR(vdif[(indx + v4) >> 1])) + 21.0f * (SQR(vdif[(indx - v6) >> 1]) + SQR(vdif[(indx + v6) >> 1])) - 6.0f * SQR(vdif[(indx - v2) >> 1] + vdif[indx >> 1] + vdif[(indx + v2) >> 1])
                        - 10.0f * (SQR(vdif[(indx - v4) >> 1] + vdif[(indx - v2) >> 1] + vdif[indx >> 1]) + SQR(vdif[indx >> 1] + vdif[(indx + v2) >> 1] + vdif[(indx + v4) >> 1])) - 7.0f * (SQR(vdif[(indx - v6) >> 1] + vdif[(indx - v4) >> 1] + vdif[(indx - v2) >> 1]) + SQR(vdif[(indx + v2) >> 1] + vdif[(indx + v4) >> 1] + vdif[(indx + v6) >> 1])), 0.f, 1.f);
                eg = LIM(epssq + 78.0f * SQR(hdif[indx >> 1]) + 69.0f * (SQR(hdif[(indx - h2) >> 1]) + SQR(hdif[(indx + h2) >> 1])) + 51.0f * (SQR(hdif[(indx - h4) >> 1]) + SQR(hdif[(indx + h4) >> 1])) + 21.0f * (SQR(hdif[(indx - h6) >> 1]) + SQR(hdif[(indx + h6) >> 1])) - 6.0f * SQR(hdif[(indx - h2) >> 1] + hdif[indx >> 1] + hdif[(indx + h2) >> 1])
                        - 10.0f * (SQR(hdif[(indx - h4) >> 1] + hdif[(indx - h2) >> 1] + hdif[indx >> 1]) + SQR(hdif[indx >> 1] + hdif[(indx + h2) >> 1] + hdif[(indx + h4) >> 1])) - 7.0f * (SQR(hdif[(indx - h6) >> 1] + hdif[(indx - h4) >> 1] + hdif[(indx - h2) >> 1]) + SQR(hdif[(indx + h2) >> 1] + hdif[(indx + h4) >> 1] + hdif[(indx + h6) >> 1])), 0.f, 1.f);
                //Limit chrominance using H/V neighbourhood
                nv = ULIM(0.725f * vdif[indx >> 1] + 0.1375f * vdif[(indx - v2) >> 1] + 0.1375f * vdif[(indx + v2) >> 1], vdif[(indx - v2) >> 1], vdif[(indx + v2) >> 1]);
                ev = ULIM(0.725f * hdif[indx >> 1] + 0.1375f * hdif[(indx - h2) >> 1] + 0.1375f * hdif[(indx + h2) >> 1], hdif[(indx - h2) >> 1], hdif[(indx + h2) >> 1]);
                //Chrominance estimation
                chr[d][indx] = (eg * nv + ng * ev) / (ng + eg);
                //Green channel population
                rgb[1][indx] = rgb[c][indx] + 65535.f * chr[d][indx];
            }

        //#ifdef _OPENMP
        //#pragma omp single
        //#endif
        //        {
        //            if (plistener) plistener->setProgress(0.39);
        //        }

        //	free(vdif); free(hdif);
#ifdef _OPENMP
#pragma omp for
#endif
        for (int row = 7; row < height - 7; row += 2)
            for (int col = 7 + (FC(row, 1, filters)&1), indx = row * width + col, c = 1 - FC(row, col, filters) / 2; col < width - 7; col += 2, indx += 2) {
                //NW,NE,SW,SE Gradients
                nwg = 1.0f / (eps + fabsf(chr[c][indx - v1 - h1] - chr[c][indx - v3 - h3]) + fabsf(chr[c][indx + v1 + h1] - chr[c][indx - v3 - h3]));
                neg = 1.0f / (eps + fabsf(chr[c][indx - v1 + h1] - chr[c][indx - v3 + h3]) + fabsf(chr[c][indx + v1 - h1] - chr[c][indx - v3 + h3]));
                swg = 1.0f / (eps + fabsf(chr[c][indx + v1 - h1] - chr[c][indx + v3 + h3]) + fabsf(chr[c][indx - v1 + h1] - chr[c][indx + v3 - h3]));
                seg = 1.0f / (eps + fabsf(chr[c][indx + v1 + h1] - chr[c][indx + v3 - h3]) + fabsf(chr[c][indx - v1 - h1] - chr[c][indx + v3 + h3]));
                //Limit NW,NE,SW,SE Color differences
                nwv = ULIM(chr[c][indx - v1 - h1], chr[c][indx - v3 - h1], chr[c][indx - v1 - h3]);
                nev = ULIM(chr[c][indx - v1 + h1], chr[c][indx - v3 + h1], chr[c][indx - v1 + h3]);
                swv = ULIM(chr[c][indx + v1 - h1], chr[c][indx + v3 - h1], chr[c][indx + v1 - h3]);
                sev = ULIM(chr[c][indx + v1 + h1], chr[c][indx + v3 + h1], chr[c][indx + v1 + h3]);
                //Interpolate chrominance: R@B and B@R
                chr[c][indx] = (nwg * nwv + neg * nev + swg * swv + seg * sev) / (nwg + neg + swg + seg);
            }
        //#ifdef _OPENMP
        //#pragma omp single
        //#endif
        //        {
        //            if (plistener) plistener->setProgress(0.52);
        //        }
#ifdef _OPENMP
#pragma omp for
#endif
        for (int row = 8; row < height - 7; row += 2)
            for (int col = 7 + (FC(row, 1, filters)&1), indx = row * width + col, c = 1 - FC(row, col, filters) / 2; col < width - 7; col += 2, indx += 2) {
                //NW,NE,SW,SE Gradients
                nwg = 1.0f / (eps + fabsf(chr[c][indx - v1 - h1] - chr[c][indx - v3 - h3]) + fabsf(chr[c][indx + v1 + h1] - chr[c][indx - v3 - h3]));
                neg = 1.0f / (eps + fabsf(chr[c][indx - v1 + h1] - chr[c][indx - v3 + h3]) + fabsf(chr[c][indx + v1 - h1] - chr[c][indx - v3 + h3]));
                swg = 1.0f / (eps + fabsf(chr[c][indx + v1 - h1] - chr[c][indx + v3 + h3]) + fabsf(chr[c][indx - v1 + h1] - chr[c][indx + v3 - h3]));
                seg = 1.0f / (eps + fabsf(chr[c][indx + v1 + h1] - chr[c][indx + v3 - h3]) + fabsf(chr[c][indx - v1 - h1] - chr[c][indx + v3 + h3]));
                //Limit NW,NE,SW,SE Color differences
                nwv = ULIM(chr[c][indx - v1 - h1], chr[c][indx - v3 - h1], chr[c][indx - v1 - h3]);
                nev = ULIM(chr[c][indx - v1 + h1], chr[c][indx - v3 + h1], chr[c][indx - v1 + h3]);
                swv = ULIM(chr[c][indx + v1 - h1], chr[c][indx + v3 - h1], chr[c][indx + v1 - h3]);
                sev = ULIM(chr[c][indx + v1 + h1], chr[c][indx + v3 + h1], chr[c][indx + v1 + h3]);
                //Interpolate chrominance: R@B and B@R
                chr[c][indx] = (nwg * nwv + neg * nev + swg * swv + seg * sev) / (nwg + neg + swg + seg);
            }
        //#ifdef _OPENMP
        //#pragma omp single
        //#endif
        //        {
        //            if (plistener) plistener->setProgress(0.65);
        //        }
#ifdef _OPENMP
#pragma omp for
#endif
        for (int row = 7; row < height - 7; row++)
            for (int col = 7 + (FC(row, 0, filters)&1), indx = row * width + col; col < width - 7; col += 2, indx += 2) {
                //N,E,W,S Gradients
                ng = 1.0f / (eps + fabsf(chr[0][indx - v1] - chr[0][indx - v3]) + fabsf(chr[0][indx + v1] - chr[0][indx - v3]));
                eg = 1.0f / (eps + fabsf(chr[0][indx + h1] - chr[0][indx + h3]) + fabsf(chr[0][indx - h1] - chr[0][indx + h3]));
                wg = 1.0f / (eps + fabsf(chr[0][indx - h1] - chr[0][indx - h3]) + fabsf(chr[0][indx + h1] - chr[0][indx - h3]));
                sg = 1.0f / (eps + fabsf(chr[0][indx + v1] - chr[0][indx + v3]) + fabsf(chr[0][indx - v1] - chr[0][indx + v3]));
                //Interpolate chrominance: R@G and B@G
                chr[0][indx] = ((ng * chr[0][indx - v1] + eg * chr[0][indx + h1] + wg * chr[0][indx - h1] + sg * chr[0][indx + v1]) / (ng + eg + wg + sg));
            }
        //#ifdef _OPENMP
        //#pragma omp single
        //#endif
        //        {
        //            if (plistener) plistener->setProgress(0.78);
        //        }
#ifdef _OPENMP
#pragma omp for
#endif
        for (int row = 7; row < height - 7; row++)
            for (int col = 7 + (FC(row, 0, filters)&1), indx = row * width + col; col < width - 7; col += 2, indx += 2) {

                //N,E,W,S Gradients
                ng = 1.0f / (eps + fabsf(chr[1][indx - v1] - chr[1][indx - v3]) + fabsf(chr[1][indx + v1] - chr[1][indx - v3]));
                eg = 1.0f / (eps + fabsf(chr[1][indx + h1] - chr[1][indx + h3]) + fabsf(chr[1][indx - h1] - chr[1][indx + h3]));
                wg = 1.0f / (eps + fabsf(chr[1][indx - h1] - chr[1][indx - h3]) + fabsf(chr[1][indx + h1] - chr[1][indx - h3]));
                sg = 1.0f / (eps + fabsf(chr[1][indx + v1] - chr[1][indx + v3]) + fabsf(chr[1][indx - v1] - chr[1][indx + v3]));
                //Interpolate chrominance: R@G and B@G
                chr[1][indx] = ((ng * chr[1][indx - v1] + eg * chr[1][indx + h1] + wg * chr[1][indx - h1] + sg * chr[1][indx + v1]) / (ng + eg + wg + sg));
            }
        //#ifdef _OPENMP
        //#pragma omp single
        //#endif
        //        {
        //            if (plistener) plistener->setProgress(0.91);
        //
        //            //Interpolate borders
        //            //	border_interpolate2(7, rgb);
        //        }
        /*
        #ifdef _OPENMP
        #pragma omp for
        #endif	
                for (int row=0; row < height; row++)  //borders
                        for (int col=0; col < width; col++) {
                                if (col==7 && row >= 7 && row < height-7)
                                        col = width-7;
                                int indxc=row*width+col;
                                red  [row][col] = rgb[indxc][0];
                                green[row][col] = rgb[indxc][1];
                                blue [row][col] = rgb[indxc][2];
                        }
         */

#ifdef _OPENMP
#pragma omp for
#endif	
        for (int row = 7; row < height - 7; row++)
            for (int col = 7, indx = row * width + col; col < width - 7; col++, indx++) {
                //                red [row][col] = CLIP(rgb[1][indx] - 65535.f * chr[0][indx]);
                //                green[row][col] = CLIP(rgb[1][indx]);
                //                blue [row][col] = CLIP(rgb[1][indx] - 65535.f * chr[1][indx]);
                out[(row * wonw + col)*4] = CLIP(rgb[1][indx] - 65535.f * chr[0][indx]);
                out[1 + (row * wonw + col)*4] = CLIP(rgb[1][indx]);
                out[2 + (row * wonw + col)*4] = CLIP(rgb[1][indx] - 65535.f * chr[1][indx]);
            }
        //#ifdef _OPENMP
        //#pragma omp single
        //#endif        
    }// End of parallelization

    //    if (plistener) plistener->setProgress(1.0);

    free(chrarray);
    free(rgbarray);
    free(vdif);
    free(hdif);
}
