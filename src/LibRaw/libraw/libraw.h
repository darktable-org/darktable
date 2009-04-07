/* -*- C++ -*-
 * File: libraw.h
 * Copyright 2008-2009 Alex Tutubalin <lexa@lexa.ru>
 * Created: Sat Mar  8, 2008 
 *
 * LibRaw C++ interface
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#ifndef _LIBRAW_CLASS_H
#define _LIBRAW_CLASS_H

#include <limits.h>
#include <memory.h>
#include <stdio.h>
#include <stdlib.h>


#include "libraw_datastream.h"
#include "libraw_types.h"
#include "libraw_const.h"
#include "libraw_internal.h"
#include "libraw_alloc.h"

//#define DCRAW_VERBOSE

#ifdef __cplusplus
extern "C" 
{
#endif
DllDef    const char          *libraw_strerror(int errorcode);
DllDef    const char          *libraw_strprogress(enum LibRaw_progress);
    // LibRaw C API
DllDef    libraw_data_t       *libraw_init(unsigned int flags);
DllDef    int                 libraw_open_file(libraw_data_t*, const char *);
DllDef    int                 libraw_open_buffer(libraw_data_t*, void * buffer, size_t size);
DllDef    int                 libraw_unpack(libraw_data_t*);
DllDef    int                 libraw_unpack_thumb(libraw_data_t*);
DllDef    void                libraw_recycle(libraw_data_t*);
DllDef    void                libraw_close(libraw_data_t*);
    // version helpers
DllDef    const char*               libraw_version();
DllDef    int                 libraw_versionNumber();
    // Camera list
DllDef    const char**        libraw_cameraList();
DllDef    int                 libraw_cameraCount();

DllDef    void                libraw_set_memerror_handler(libraw_data_t*, memory_callback cb, void *datap);
DllDef    void                libraw_set_dataerror_handler(libraw_data_t*,data_callback func,void *datap);
DllDef    void                libraw_set_progress_handler(libraw_data_t*,progress_callback cb,void *datap);
DllDef    int                 libraw_add_masked_borders_to_bitmap(libraw_data_t* lr);
DllDef    const char *        libraw_unpack_function_name(libraw_data_t* lr);
DllDef    int                 libraw_rotate_fuji_raw(libraw_data_t* lr);

    // DCRAW compatibility
DllDef    int                 libraw_adjust_sizes_info_only(libraw_data_t*);
DllDef    int                 libraw_dcraw_document_mode_processing(libraw_data_t*);
DllDef    int                 libraw_dcraw_ppm_tiff_writer(libraw_data_t* lr,const char *filename);
DllDef    int                 libraw_dcraw_thumb_writer(libraw_data_t* lr,const char *fname);
DllDef    int                 libraw_dcraw_process(libraw_data_t* lr);
DllDef    libraw_processed_image_t* dcraw_make_mem_image(libraw_data_t* lr, int *errc);
DllDef    libraw_processed_image_t* dcraw_make_mem_thumb(libraw_data_t* lr, int *errc);

#ifdef __cplusplus
}
#endif


#ifdef __cplusplus

class DllDef LibRaw
{
  public:
    libraw_data_t imgdata;
    int verbose;

    LibRaw(unsigned int flags = LIBRAW_OPTIONS_NONE);
    
    libraw_output_params_t*     output_params_ptr() { return &imgdata.params;}
    int                         open_file(const char *fname);
    int                         open_buffer(void *buffer, size_t size);
    int                         open_datastream(LibRaw_abstract_datastream *);
    int                         unpack(void);
    int                         unpack_thumb(void);

    int                         adjust_sizes_info_only(void);
    void                        set_memerror_handler( memory_callback cb,void *data) {callbacks.memcb_data = data; callbacks.mem_cb = cb; }
    void                        set_dataerror_handler(data_callback func, void *data) { callbacks.datacb_data = data; callbacks.data_cb = func;}
    void                        set_progress_handler(progress_callback pcb, void *data) { callbacks.progresscb_data = data; callbacks.progress_cb = pcb;}

    // helpers
    static const char*          version() { return LIBRAW_VERSION_STR;}
    static int                  versionNumber() { return LIBRAW_VERSION; }
    static const char**         cameraList();
    static int                  cameraCount();
    static const char*          strprogress(enum LibRaw_progress);
    static const char*          strerror(int p) { return libraw_strerror(p);}
    // dcraw emulation
    int                         dcraw_document_mode_processing();
    int                         dcraw_ppm_tiff_writer(const char *filename);
    int                         dcraw_thumb_writer(const char *fname);
    int                         dcraw_process(void);
    // memory writers
    libraw_processed_image_t*   dcraw_make_mem_image(int *errcode=NULL);  
    libraw_processed_image_t*   dcraw_make_mem_thumb(int *errcode=NULL);  

    // free all internal data structures
    void         recycle(); 
    ~LibRaw(void) { recycle(); delete tls; }

    int FC(int row,int col) { return (imgdata.idata.filters >> ((((row) << 1 & 14) + ((col) & 1)) << 1) & 3);}
    int         fc (int row, int col);
    int add_masked_borders_to_bitmap();
    
    const char *unpack_function_name();
    int         rotate_fuji_raw();

  private:
    void*        malloc(size_t t);
    void*        calloc(size_t n,size_t t);
    void        free(void *p);
    void        merror (void *ptr, const char *where);
    void        derror();

// data

    LibRaw_TLS  *tls;
    libraw_internal_data_t libraw_internal_data;
    decode      first_decode[2048], *second_decode, *free_decode;
    tiff_ifd_t  tiff_ifd[10];
    libraw_memmgr memmgr;
    libraw_callbacks_t callbacks;

    LibRaw_constants rgb_constants;
    void        (LibRaw:: *write_thumb)(FILE *), 
                (LibRaw:: *write_fun)(FILE *);
    void        (LibRaw:: *load_raw)(),
                (LibRaw:: *thumb_load_raw)();

    void        kodak_thumb_loader();
    void        write_thumb_ppm_tiff(FILE *); // kodak
    void        foveon_thumb_loader (void); //Sigma

    
    // moved from implementation level to private: visibility
    void init_masked_ptrs();
    ushort *get_masked_pointer(int row, int col); 
    
    int         own_filtering_supported(){ return 0;}
    void        identify();
    void        write_ppm_tiff (FILE *ofp);
    void        convert_to_rgb();
    void        kodak_ycbcr_load_raw();
    void        remove_zeroes();
#ifndef NO_LCMS
    void	apply_profile(char*,char*);
#endif
// Iterpolators
    void        pre_interpolate();
    void        border_interpolate (int border);
    void        lin_interpolate();
    void        vng_interpolate();
    void        ppg_interpolate();
    void        ahd_interpolate();

// Image filters
    void        bad_pixels(char*);
    void        subtract(char*);
    void        hat_transform (float *temp, float *base, int st, int size, int sc);
    void        wavelet_denoise();
    void        scale_colors();
    void        median_filter ();
    void        blend_highlights();
    void        recover_highlights();

    void        fuji_rotate();
    void        stretch();

// Thmbnail functions
    void        foveon_thumb (FILE *tfp);
    void        jpeg_thumb_writer (FILE *tfp,char *thumb,int thumb_length);
    void        jpeg_thumb (FILE *tfp);
    void        ppm_thumb (FILE *tfp);
    void        layer_thumb (FILE *tfp);
    void        rollei_thumb (FILE *tfp);
    void        kodak_thumb_load_raw();

    // utility for cut'n'pasted code
    void        foveon_decoder (unsigned size, unsigned code);
    unsigned    get4();

    int         flip_index (int row, int col);
    void        gamma_lut(ushort lut[0x10000]);


// == internal functions

#ifdef LIBRAW_LIBRARY_BUILD 
#include "internal/libraw_internal_funcs.h"
#endif

};

#ifdef LIBRAW_LIBRARY_BUILD 
#define RUN_CALLBACK(stage,iter,expect)  if(callbacks.progress_cb) { \
        int rr = (*callbacks.progress_cb)(callbacks.progresscb_data,stage,iter,expect); \
        if(rr!=0) throw LIBRAW_EXCEPTION_CANCELLED_BY_CALLBACK; \
    }
#endif


#endif // __cplusplus


#endif // _LIBRAW_CLASS_H
