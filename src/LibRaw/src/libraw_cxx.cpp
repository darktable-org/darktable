/* -*- C++ -*-
 * File: libraw_cxx.cpp
 * Copyright 2008-2010 LibRaw LLC (info@libraw.org)
 * Created: Sat Mar  8 , 2008
 *
 * LibRaw C++ interface (implementation)

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of three licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

3. LibRaw Software License 27032010
   (See file LICENSE.LibRaw.pdf provided in LibRaw distribution archive for details).

 */

#include <errno.h>
#include <float.h>
#include <math.h>
#ifndef WIN32
#include <netinet/in.h>
#else
#include <winsock2.h>
#endif
#define LIBRAW_LIBRARY_BUILD
#include "libraw/libraw.h"

#ifdef __cplusplus
extern "C" 
{
#endif
    void default_memory_callback(void *,const char *file,const char *where)
    {
        fprintf (stderr,"%s: Out of memory in %s\n", file?file:"unknown file", where);
    }

    void default_data_callback(void*,const char *file, const int offset)
    {
        if(offset < 0)
            fprintf (stderr,"%s: Unexpected end of file\n", file?file:"unknown file");
        else
            fprintf (stderr,"%s: data corrupted at %d\n",file?file:"unknown file",offset); 
    }
    const char *libraw_strerror(int e)
    {
        enum LibRaw_errors errorcode = (LibRaw_errors)e;
        switch(errorcode)
            {
            case        LIBRAW_SUCCESS:
                return "No error";
            case        LIBRAW_UNSPECIFIED_ERROR:
                return "Unspecified error";
            case        LIBRAW_FILE_UNSUPPORTED:
                return "Unsupported file format or not RAW file";
            case        LIBRAW_REQUEST_FOR_NONEXISTENT_IMAGE:
                return "Request for nonexisting image number";
            case        LIBRAW_OUT_OF_ORDER_CALL:
                return "Out of order call of libraw function";
            case    LIBRAW_NO_THUMBNAIL:
                return "No thumbnail in file";
            case    LIBRAW_UNSUPPORTED_THUMBNAIL:
                return "Unsupported thumbnail format";
            case LIBRAW_CANNOT_ADDMASK:
                return "Cannot add masked pixels to resized image";
            case    LIBRAW_UNSUFFICIENT_MEMORY:
                return "Unsufficient memory";
            case    LIBRAW_DATA_ERROR:
                return "Corrupted data or unexpected EOF";
            case    LIBRAW_IO_ERROR:
                return "Input/output error";
            case LIBRAW_CANCELLED_BY_CALLBACK:
                return "Cancelled by user callback";
            default:
                return "Unknown error code";
        }
    }

#ifdef __cplusplus
}
#endif


const double LibRaw_constants::xyz_rgb[3][3] = 
{
    { 0.412453, 0.357580, 0.180423 },
    { 0.212671, 0.715160, 0.072169 },
    { 0.019334, 0.119193, 0.950227 } 
};

const float LibRaw_constants::d65_white[3] =  { 0.950456, 1, 1.088754 };

#define P1 imgdata.idata
#define S imgdata.sizes
#define O imgdata.params
#define C imgdata.color
#define M imgdata.masked_pixels
#define T imgdata.thumbnail
#define IO libraw_internal_data.internal_output_params
#define ID libraw_internal_data.internal_data

#define EXCEPTION_HANDLER(e) do{                        \
        fprintf(stderr,"Exception %d caught\n",e);      \
        switch(e)                                       \
            {                                           \
            case LIBRAW_EXCEPTION_ALLOC:                \
                recycle();                              \
                return LIBRAW_UNSUFFICIENT_MEMORY;      \
            case LIBRAW_EXCEPTION_DECODE_RAW:           \
            case LIBRAW_EXCEPTION_DECODE_JPEG:          \
                recycle();                              \
                return LIBRAW_DATA_ERROR;               \
            case LIBRAW_EXCEPTION_IO_EOF:               \
            case LIBRAW_EXCEPTION_IO_CORRUPT:           \
                recycle();                              \
                return LIBRAW_IO_ERROR;                 \
            case LIBRAW_EXCEPTION_CANCELLED_BY_CALLBACK:\
                recycle();                              \
                return LIBRAW_CANCELLED_BY_CALLBACK;    \
            default:                                    \
                return LIBRAW_UNSPECIFIED_ERROR;        \
            } \
    }while(0)

const char* LibRaw::version() { return LIBRAW_VERSION_STR;}
int LibRaw::versionNumber() { return LIBRAW_VERSION; }
const char* LibRaw::strerror(int p) { return libraw_strerror(p);}


void LibRaw::derror()
{
    if (!libraw_internal_data.unpacker_data.data_error && libraw_internal_data.internal_data.input) 
        {
            if (libraw_internal_data.internal_data.input->eof())
                {
                    if(callbacks.data_cb)(*callbacks.data_cb)(callbacks.datacb_data,
                                                              libraw_internal_data.internal_data.input->fname(),-1);
                    throw LIBRAW_EXCEPTION_IO_EOF;
                }
            else
                {
                    if(callbacks.data_cb)(*callbacks.data_cb)(callbacks.datacb_data,
                                                              libraw_internal_data.internal_data.input->fname(),
                                                              libraw_internal_data.internal_data.input->tell());
                    throw LIBRAW_EXCEPTION_IO_CORRUPT;
                }
        }
    libraw_internal_data.unpacker_data.data_error++;
}

#define ZERO(a) memset(&a,0,sizeof(a))


LibRaw:: LibRaw(unsigned int flags)
{
    double aber[4] = {1,1,1,1};
    double gamm[6] = { 0.45,4.5,0,0,0,0 };
    unsigned greybox[4] =  { 0, 0, UINT_MAX, UINT_MAX };
#ifdef DCRAW_VERBOSE
    verbose = 1;
#else
    verbose = 0;
#endif
    ZERO(imgdata);
    ZERO(libraw_internal_data);
    ZERO(callbacks);
    callbacks.mem_cb = (flags & LIBRAW_OPIONS_NO_MEMERR_CALLBACK) ? NULL:  &default_memory_callback;
    callbacks.data_cb = (flags & LIBRAW_OPIONS_NO_DATAERR_CALLBACK)? NULL : &default_data_callback;
    memmove(&imgdata.params.aber,&aber,sizeof(aber));
    memmove(&imgdata.params.gamm,&gamm,sizeof(gamm));
    memmove(&imgdata.params.greybox,&greybox,sizeof(greybox));
    
    imgdata.params.bright=1;
    imgdata.params.use_camera_matrix=-1;
    imgdata.params.user_flip=-1;
    imgdata.params.user_black=-1;
    imgdata.params.user_sat=-1;
    imgdata.params.user_qual=-1;
    imgdata.params.output_color=1;
    imgdata.params.output_bps=8;
    imgdata.params.use_fuji_rotate=1;
    imgdata.params.auto_bright_thr = LIBRAW_DEFAULT_AUTO_BRIGHTNESS_THRESHOLD;
    imgdata.params.adjust_maximum_thr= LIBRAW_DEFAULT_ADJUST_MAXIMUM_THRESHOLD;
    imgdata.parent_class = this;
    imgdata.progress_flags = 0;
    tls = new LibRaw_TLS;
    tls->init();
}


void* LibRaw:: malloc(size_t t)
{
    void *p = memmgr.malloc(t);
    return p;
}
void* LibRaw::       calloc(size_t n,size_t t)
{
    void *p = memmgr.calloc(n,t);
    return p;
}
void  LibRaw::      free(void *p)
{
    memmgr.free(p);
}


int LibRaw:: fc (int row, int col)
{
    static const char filter[16][16] =
        { { 2,1,1,3,2,3,2,0,3,2,3,0,1,2,1,0 },
          { 0,3,0,2,0,1,3,1,0,1,1,2,0,3,3,2 },
          { 2,3,3,2,3,1,1,3,3,1,2,1,2,0,0,3 },
          { 0,1,0,1,0,2,0,2,2,0,3,0,1,3,2,1 },
          { 3,1,1,2,0,1,0,2,1,3,1,3,0,1,3,0 },
          { 2,0,0,3,3,2,3,1,2,0,2,0,3,2,2,1 },
          { 2,3,3,1,2,1,2,1,2,1,1,2,3,0,0,1 },
          { 1,0,0,2,3,0,0,3,0,3,0,3,2,1,2,3 },
          { 2,3,3,1,1,2,1,0,3,2,3,0,2,3,1,3 },
          { 1,0,2,0,3,0,3,2,0,1,1,2,0,1,0,2 },
          { 0,1,1,3,3,2,2,1,1,3,3,0,2,1,3,2 },
          { 2,3,2,0,0,1,3,0,2,0,1,2,3,0,1,0 },
          { 1,3,1,2,3,2,3,2,0,2,0,1,1,0,3,0 },
          { 0,2,0,3,1,0,0,1,1,3,3,2,3,2,2,1 },
          { 2,1,3,2,3,1,2,1,0,3,0,2,0,2,0,2 },
          { 0,3,1,0,0,2,0,3,2,1,3,1,1,3,1,3 } };
    
    if (imgdata.idata.filters != 1) return FC(row,col);
    return filter[(row+imgdata.sizes.top_margin) & 15][(col+imgdata.sizes.left_margin) & 15];
}

void LibRaw:: recycle() 
{
    if(libraw_internal_data.internal_data.input && libraw_internal_data.internal_data.input_internal) 
        { 
            delete libraw_internal_data.internal_data.input; 
            libraw_internal_data.internal_data.input = NULL;
        }
    libraw_internal_data.internal_data.input_internal = 0;
#define FREE(a) do { if(a) { free(a); a = NULL;} }while(0)
            
    FREE(imgdata.image); 
    FREE(imgdata.thumbnail.thumb);
    FREE(libraw_internal_data.internal_data.meta_data);
    FREE(libraw_internal_data.output_data.histogram);
    FREE(libraw_internal_data.output_data.oprof);
    FREE(imgdata.color.profile);
    FREE(imgdata.masked_pixels.buffer);
    FREE(imgdata.masked_pixels.ph1_black);
#undef FREE
    ZERO(imgdata.masked_pixels);
    ZERO(imgdata.sizes);
    ZERO(imgdata.color);
    ZERO(libraw_internal_data.internal_output_params);
    memmgr.cleanup();
    imgdata.thumbnail.tformat = LIBRAW_THUMBNAIL_UNKNOWN;
    imgdata.progress_flags = 0;
    
    tls->init();
}

const char * LibRaw::unpack_function_name()
{
    if(!load_raw) return "Function not set";

    // sorted names order
    if (load_raw == &LibRaw::adobe_dng_load_raw_lj)     return "adobe_dng_load_raw_lj()"; //+
    if (load_raw == &LibRaw::adobe_dng_load_raw_nc)     return "adobe_dng_load_raw_nc()"; //+
    if (load_raw == &LibRaw::canon_600_load_raw)        return "canon_600_load_raw()";    //+

    if (load_raw == &LibRaw::canon_compressed_load_raw) return "canon_compressed_load_raw()"; //+
    if (load_raw == &LibRaw::canon_sraw_load_raw)       return "canon_sraw_load_raw()"; //+

    if (load_raw == &LibRaw::eight_bit_load_raw )       return "eight_bit_load_raw()"; //+
    if (load_raw == &LibRaw::fuji_load_raw )            return "fuji_load_raw()"; //+
    // 10
    if (load_raw == &LibRaw::hasselblad_load_raw )      return "hasselblad_load_raw()"; //+
    if (load_raw == &LibRaw::imacon_full_load_raw )     return "imacon_full_load_raw()"; //+ (untested)
    if (load_raw == &LibRaw::kodak_262_load_raw )       return "kodak_262_load_raw()"; //+

    if (load_raw == &LibRaw::kodak_65000_load_raw )     return "kodak_65000_load_raw()";//+
    if (load_raw == &LibRaw::kodak_dc120_load_raw )     return "kodak_dc120_load_raw()"; //+
    if (load_raw == &LibRaw::kodak_jpeg_load_raw )      return "kodak_jpeg_load_raw()"; //+ (untested)

    if (load_raw == &LibRaw::kodak_radc_load_raw )      return "kodak_radc_load_raw()"; //+
    if (load_raw == &LibRaw::kodak_rgb_load_raw )       return "kodak_rgb_load_raw()"; //+ (untested)
    if (load_raw == &LibRaw::kodak_yrgb_load_raw )      return "kodak_yrgb_load_raw()"; //+
    if (load_raw == &LibRaw::kodak_ycbcr_load_raw )     return "kodak_ycbcr_load_raw()"; //+ (untested)
    // 20
    if (load_raw == &LibRaw::leaf_hdr_load_raw )        return "leaf_hdr_load_raw()"; //+
    if (load_raw == &LibRaw::lossless_jpeg_load_raw)    return "lossless_jpeg_load_raw()"; //+
    if (load_raw == &LibRaw::minolta_rd175_load_raw )   return "minolta_rd175_load_raw()"; //+

    if (load_raw == &LibRaw::nikon_compressed_load_raw) return "nikon_compressed_load_raw()";//+
    if (load_raw == &LibRaw::nokia_load_raw )           return "nokia_load_raw()";//+ (untested)

    if (load_raw == &LibRaw::olympus_load_raw )    return "olympus_load_raw()"; //+
    if (load_raw == &LibRaw::packed_load_raw )       return "packed_load_raw()"; //+
    if (load_raw == &LibRaw::panasonic_load_raw )       return "panasonic_load_raw()";//+
    // 30
    if (load_raw == &LibRaw::pentax_load_raw )          return "pentax_load_raw()"; //+
    if (load_raw == &LibRaw::phase_one_load_raw )       return "phase_one_load_raw()"; //+
    if (load_raw == &LibRaw::phase_one_load_raw_c )     return "phase_one_load_raw_c()"; //+

    if (load_raw == &LibRaw::quicktake_100_load_raw )   return "quicktake_100_load_raw()";//+ (untested)
    if (load_raw == &LibRaw::rollei_load_raw )          return "rollei_load_raw()"; //+ (untested)
    if (load_raw == &LibRaw::sinar_4shot_load_raw )     return "sinar_4shot_load_raw()";//+

    if (load_raw == &LibRaw::smal_v6_load_raw )         return "smal_v6_load_raw()";//+ (untested)
    if (load_raw == &LibRaw::smal_v9_load_raw )         return "smal_v9_load_raw()";//+ (untested)
    if (load_raw == &LibRaw::sony_load_raw )            return "sony_load_raw()"; //+
    if (load_raw == &LibRaw::sony_arw_load_raw )        return "sony_arw_load_raw()";//+
    // 40
    if (load_raw == &LibRaw::sony_arw2_load_raw )       return "sony_arw2_load_raw()";//+
    if (load_raw == &LibRaw::unpacked_load_raw )        return "unpacked_load_raw()"; //+
    // 42 total
        
    return "Unknown unpack function";
}

int LibRaw::adjust_maximum()
{
    int i;
    ushort real_max;
    float  auto_threshold;

    if(O.adjust_maximum_thr < 0.00001)
        return LIBRAW_SUCCESS;
    else if (O.adjust_maximum_thr > 0.99999)
        auto_threshold = LIBRAW_DEFAULT_ADJUST_MAXIMUM_THRESHOLD;
    else
        auto_threshold = O.adjust_maximum_thr;
        
    
    real_max = C.channel_maximum[0];
    for(i = 1; i< 4; i++)
        if(real_max < C.channel_maximum[i])
            real_max = C.channel_maximum[i];

    if (real_max > 0 && real_max < C.maximum && real_max > C.maximum* auto_threshold)
        {
            C.maximum = real_max;
        }
    return LIBRAW_SUCCESS;
}


void LibRaw:: merror (void *ptr, const char *where)
{
    if (ptr) return;
    if(callbacks.mem_cb)(*callbacks.mem_cb)(callbacks.memcb_data,
                                            libraw_internal_data.internal_data.input
                                            ?libraw_internal_data.internal_data.input->fname()
                                            :NULL,
                                            where);
    throw LIBRAW_EXCEPTION_ALLOC;
}

ushort * LibRaw::get_masked_pointer(int row, int col) 
{ 
    if(row<0 || col < 0) return NULL;
    if(!M.buffer) return NULL; 
    if(row < S.top_margin)
        {
            // top band
            if(col < S.left_margin)
                {
                    return &(M.tl[row*S.left_margin+col]);
                }
            else if (col < S.left_margin + S.width)
                {
                    int icol = col - S.left_margin;
                    return &(M.top[row*S.width+icol]);
                }
            else if (col < S.raw_width)
                {
                    int icol = col - S.left_margin - S.width;
                    return &(M.tr[row*S.right_margin+icol]);
                }
            else
                return NULL; // out of bounds
        }
    else if (row < S.top_margin + S.height)
        {
            //normal image height
            int irow = row - S.top_margin;
            if(col < S.left_margin)
                {
                    return &M.left[irow*S.left_margin + col];
                }
            else if (col < S.left_margin + S.width)
                {
                    // central image
                    return NULL;
                }
            else if (col < S.raw_width)
                {
                    int icol = col - S.left_margin - S.width;
                    return &M.right[irow*S.right_margin+icol];
                }
            else
                return NULL; // out of bounds
        }
    else if (row < S.raw_height)
        {
            int irow = row - S.top_margin - S.height;
            // bottom band
            if(col < S.left_margin)
                {
                    return &M.bl[irow*S.left_margin+col];
                }
            else if (col < S.left_margin + S.width)
                {
                    int icol = col - S.left_margin;
                    return &M.bottom[irow*S.width + icol];
                }
            else if (col < S.raw_width)
                {
                    int icol = col - S.left_margin - S.width;
                    return &M.br[irow*S.right_margin + icol];
                }
            else
                return NULL; // out of bounds
        }
    else
        {
            // out of bounds
            return NULL;
        }
    // fallback
    return NULL;
}

void LibRaw:: init_masked_ptrs()
{
    if(!M.buffer) return;
    
    // top band
    M.tl = M.buffer;
    M.top = M.tl +(S.top_margin*S.left_margin);
    M.tr =  M.top + (S.top_margin*S.width);
    
    // left-right
    M.left = M.tr + (S.top_margin * S.right_margin);
    M.right = M.left + (S.left_margin * S.height);

    // bottom band
    M.bl = M.right + (S.right_margin * S.height);
    M.bottom = M.bl + (S.left_margin * S.bottom_margin);
    M.br = M.bottom + (S.width * S.bottom_margin);

}
int LibRaw::add_masked_borders_to_bitmap()
{
    CHECK_ORDER_HIGH(LIBRAW_PROGRESS_PRE_INTERPOLATE);
    CHECK_ORDER_LOW(LIBRAW_PROGRESS_LOAD_RAW);

    if(S.width != S.iwidth || S.height!=S.iheight)
        return LIBRAW_CANNOT_ADDMASK;

    if(!P1.filters)
        return LIBRAW_CANNOT_ADDMASK;
        
    if(!imgdata.image)
        return LIBRAW_OUT_OF_ORDER_CALL;

    if(S.raw_width < S.width || S.raw_height < S.height)
        return LIBRAW_SUCCESS; // nothing to do or already called

    if(S.width == S.raw_width && S.height == S.raw_height)
        return LIBRAW_SUCCESS; // nothing to do or already called

    ushort (*newimage)[4];

    newimage = (ushort (*)[4]) calloc (S.raw_height*S.raw_width, sizeof (*newimage));
    merror (newimage, "add_masked_borders_to_bitmap()");

    int r,c;
    // top rows
    for (r=0; r<S.top_margin;r++)
        for(c=0;c<S.raw_width;c++)
            {
                ushort *p = get_masked_pointer(r,c);
                if(p)
                    newimage[r*S.raw_width+c][COLOR(r,c)] = *p;
            }
    // middle rows
    for (r=S.top_margin; r<S.top_margin+S.height;r++)
        {
            int row = r-S.top_margin;
            for(c=0;c<S.left_margin;c++)
                {
                    ushort *p = get_masked_pointer(r,c);
                    if(p)
                        newimage[r*S.raw_width+c][COLOR(r,c)] =  *p;
                }
            for(c=S.left_margin; c<S.left_margin+S.iwidth;c++)
                {
                    int col = c - S.left_margin;
                    newimage[r*S.raw_width+c][COLOR(r,c)] = imgdata.image[row*S.iwidth+col][COLOR(r,c)];
//                    for(int cc=0;cc<4;cc++)
//                        newimage[r*S.raw_width+c][cc] = imgdata.image[row*S.iwidth+col][cc];
                }
            for(c=S.left_margin+S.iwidth;c<S.raw_width;c++)
                {
                    ushort *p = get_masked_pointer(r,c);
                    if(p)
                        newimage[r*S.raw_width+c][COLOR(r,c)] =  *p;
                }
        }
    // bottom rows
    for (r=S.top_margin+S.height; r<S.raw_height;r++)
        for(c=0;c<S.raw_width;c++)
            {
                ushort *p = get_masked_pointer(r,c);
                if(p)
                    newimage[r*S.raw_width+c][COLOR(r,c)] = *p;
            }
    free(imgdata.image);
    imgdata.image=newimage;
    S.iwidth = S.width = S.raw_width;
    S.iheight = S.height = S.raw_height;
    return LIBRAW_SUCCESS;
}

int LibRaw::open_file(const char *fname)
{
    // this stream will close on recycle()
    LibRaw_file_datastream *stream = new LibRaw_file_datastream(fname);
    if(!stream->valid())
        {
            delete stream;
            return LIBRAW_IO_ERROR;
        }
    ID.input_internal = 0; // preserve from deletion on error
    int ret = open_datastream(stream);
    if (ret == LIBRAW_SUCCESS)
        {
            ID.input_internal =1 ; // flag to delete datastream on recycle
        }
    else
        {
            delete stream;
            ID.input_internal = 0;
        }
    return ret;
}

int LibRaw::open_buffer(void *buffer, size_t size)
{
    // this stream will close on recycle()
    if(!buffer  || buffer==(void*)-1)
        return LIBRAW_IO_ERROR;

    LibRaw_buffer_datastream *stream = new LibRaw_buffer_datastream(buffer,size);
    if(!stream->valid())
        {
            delete stream;
            return LIBRAW_IO_ERROR;
        }
    ID.input_internal = 0; // preserve from deletion on error
    int ret = open_datastream(stream);
    if (ret == LIBRAW_SUCCESS)
        {
            ID.input_internal =1 ; // flag to delete datastream on recycle
        }
    else
        {
            delete stream;
            ID.input_internal = 0;
        }
    return ret;
}


int LibRaw::open_datastream(LibRaw_abstract_datastream *stream)
{

    if(!stream)
        return ENOENT;
    if(!stream->valid())
        return LIBRAW_IO_ERROR;
    recycle();

    try {
        ID.input = stream;
        SET_PROC_FLAG(LIBRAW_PROGRESS_OPEN);

        if (O.use_camera_matrix < 0)
            O.use_camera_matrix = O.use_camera_wb;

        identify();

        if(IO.fuji_width)
            {
                IO.fwidth = S.width;
                IO.fheight = S.height;
                S.iwidth = S.width = IO.fuji_width << !libraw_internal_data.unpacker_data.fuji_layout;
                S.iheight = S.height = S.raw_height;
                S.raw_height += 2*S.top_margin;
            }

        int saved_raw_width = S.raw_width;
        int saved_width = S.width;
        // from packed_12_load_raw
        if ((load_raw == &LibRaw:: packed_load_raw) && (S.raw_width * 8U >= S.width * libraw_internal_data.unpacker_data.tiff_bps))
            {	
                // raw_width is in bytes!
                S.raw_width = S.raw_width * 8 / libraw_internal_data.unpacker_data.tiff_bps;	
            }
        else if (S.pixel_aspect < 0.95 || S.pixel_aspect > 1.05)
            {
                S.width*=S.pixel_aspect;
            }

        if(S.raw_width>S.width + S.left_margin)
            S.right_margin = S.raw_width - S.width - S.left_margin;

        if(S.raw_height > S.height + S.top_margin)
            S.bottom_margin = S.raw_height - S.height - S.top_margin;

        S.raw_width = saved_raw_width;
        S.width = saved_width;

        if(C.profile_length)
            {
                if(C.profile) free(C.profile);
                C.profile = malloc(C.profile_length);
                merror(C.profile,"LibRaw::open_file()");
                ID.input->seek(ID.profile_offset,SEEK_SET);
                ID.input->read(C.profile,C.profile_length,1);
            }
        
        SET_PROC_FLAG(LIBRAW_PROGRESS_IDENTIFY);
    }
    catch ( LibRaw_exceptions err) {
        EXCEPTION_HANDLER(err);
    }

    if(P1.raw_count < 1) 
        return LIBRAW_FILE_UNSUPPORTED;

    if (O.user_flip >= 0)
        S.flip = O.user_flip;
    
    switch ((S.flip+3600) % 360) 
        {
        case 270:  S.flip = 5;  break;
        case 180:  S.flip = 3;  break;
        case  90:  S.flip = 6;  break;
        }
    
    write_fun = &LibRaw::write_ppm_tiff;
    
    if (load_raw == &LibRaw::kodak_ycbcr_load_raw) 
        {
            S.height += S.height & 1;
            S.width  += S.width  & 1;
        }

    IO.shrink = P1.filters && (O.half_size || O.threshold || O.aber[0] != 1 || O.aber[2] != 1);
    S.iheight = (S.height + IO.shrink) >> IO.shrink;
    S.iwidth  = (S.width  + IO.shrink) >> IO.shrink;
    
    SET_PROC_FLAG(LIBRAW_PROGRESS_SIZE_ADJUST);


    return LIBRAW_SUCCESS;
}

int LibRaw::unpack(void)
{
    CHECK_ORDER_HIGH(LIBRAW_PROGRESS_LOAD_RAW);
    CHECK_ORDER_LOW(LIBRAW_PROGRESS_IDENTIFY);
    try {

        RUN_CALLBACK(LIBRAW_PROGRESS_LOAD_RAW,0,2);
        if (O.shot_select >= P1.raw_count)
            return LIBRAW_REQUEST_FOR_NONEXISTENT_IMAGE;
        
        if(!load_raw)
            return LIBRAW_UNSPECIFIED_ERROR;
        
        if (O.use_camera_matrix && C.cmatrix[0][0] > 0.25) 
            {
                memcpy (C.rgb_cam, C.cmatrix, sizeof (C.cmatrix));
                IO.raw_color = 0;
            }
        // already allocated ?
        if(imgdata.image) free(imgdata.image);
        
        imgdata.image = (ushort (*)[4]) calloc (S.iheight*S.iwidth, sizeof (*imgdata.image));
        merror (imgdata.image, "unpack()");


        if(S.top_margin || S.left_margin || S.right_margin || S.bottom_margin)
            {
                unsigned sz = S.raw_height*(S.left_margin+S.right_margin) 
                    + S.width*(S.top_margin+S.bottom_margin);
                imgdata.masked_pixels.buffer = (ushort*) calloc(sz, sizeof(ushort)); 
                merror (imgdata.masked_pixels.buffer, "unpack()");
                init_masked_ptrs();
            }
        if (libraw_internal_data.unpacker_data.meta_length) 
            {
                libraw_internal_data.internal_data.meta_data = 
                    (char *) malloc (libraw_internal_data.unpacker_data.meta_length);
                merror (libraw_internal_data.internal_data.meta_data, "LibRaw::unpack()");
            }
        ID.input->seek(libraw_internal_data.unpacker_data.data_offset, SEEK_SET);
        int save_document_mode = O.document_mode;
        O.document_mode = 0;

        if(!own_filtering_supported() && (O.filtering_mode & LIBRAW_FILTERING_AUTOMATIC_BIT))
            O.filtering_mode = LIBRAW_FILTERING_AUTOMATIC_BIT; // turn on black and zeroes filtering
        
        (this->*load_raw)();
        
        O.document_mode = save_document_mode;

        if (O.filtering_mode & LIBRAW_FILTERING_AUTOMATIC_BIT)
            O.filtering_mode = LIBRAW_FILTERING_AUTOMATIC; // restore automated mode
        
        SET_PROC_FLAG(LIBRAW_PROGRESS_LOAD_RAW);
        RUN_CALLBACK(LIBRAW_PROGRESS_LOAD_RAW,1,2);
        
        return 0;
    }
    catch ( LibRaw_exceptions err) {
        EXCEPTION_HANDLER(err);
    }
}

int LibRaw::dcraw_document_mode_processing(void)
{
    CHECK_ORDER_HIGH(LIBRAW_PROGRESS_PRE_INTERPOLATE);
    CHECK_ORDER_LOW(LIBRAW_PROGRESS_LOAD_RAW);

    try {

        if(IO.fwidth) 
            rotate_fuji_raw();

        if(!own_filtering_supported() && (O.filtering_mode & LIBRAW_FILTERING_AUTOMATIC_BIT))
            O.filtering_mode = LIBRAW_FILTERING_AUTOMATIC_BIT; // turn on black and zeroes filtering

        O.document_mode = 2;

        O.use_fuji_rotate = 0;
        if (!(O.filtering_mode & LIBRAW_FILTERING_NOZEROES) && IO.zero_is_bad)
            {
                remove_zeroes();
                SET_PROC_FLAG(LIBRAW_PROGRESS_REMOVE_ZEROES);
            }
        if(O.bad_pixels) 
            {
                bad_pixels(O.bad_pixels);
                SET_PROC_FLAG(LIBRAW_PROGRESS_BAD_PIXELS);
            }
        if (O.dark_frame)
            {
                subtract (O.dark_frame);
                SET_PROC_FLAG(LIBRAW_PROGRESS_DARK_FRAME);
            }
        if(O.filtering_mode & LIBRAW_FILTERING_NOBLACKS)
            C.black=0;

        if (O.user_black >= 0) 
            C.black = O.user_black;

        if (O.user_sat > 0) 
            C.maximum = O.user_sat;

        pre_interpolate();
        SET_PROC_FLAG(LIBRAW_PROGRESS_PRE_INTERPOLATE);

        if (libraw_internal_data.internal_output_params.mix_green)
            {
                int i;
                for (P1.colors=3, i=0; i < S.height*S.width; i++)
                    imgdata.image[i][1] = (imgdata.image[i][1] + imgdata.image[i][3]) >> 1;
            }
        SET_PROC_FLAG(LIBRAW_PROGRESS_MIX_GREEN);

        if ( P1.colors == 3) 
            median_filter();
        SET_PROC_FLAG(LIBRAW_PROGRESS_MEDIAN_FILTER);

        if ( O.highlight == 2) 
            blend_highlights();

        if ( O.highlight > 2) 
            recover_highlights();
        SET_PROC_FLAG(LIBRAW_PROGRESS_HIGHLIGHTS);

        if (O.use_fuji_rotate) 
            fuji_rotate();
        SET_PROC_FLAG(LIBRAW_PROGRESS_FUJI_ROTATE);
#ifndef NO_LCMS
	if(O.camera_profile)
            {
                apply_profile(O.camera_profile,O.output_profile);
                SET_PROC_FLAG(LIBRAW_PROGRESS_APPLY_PROFILE);
            }
#endif
        if(!libraw_internal_data.output_data.histogram)
            {
                libraw_internal_data.output_data.histogram = (int (*)[LIBRAW_HISTOGRAM_SIZE]) malloc(sizeof(*libraw_internal_data.output_data.histogram)*4);
                merror(libraw_internal_data.output_data.histogram,"LibRaw::dcraw_document_mode_processing()");
            }
        convert_to_rgb();
        SET_PROC_FLAG(LIBRAW_PROGRESS_CONVERT_RGB);

        if (O.use_fuji_rotate)
            stretch();
        SET_PROC_FLAG(LIBRAW_PROGRESS_STRETCH);

        if (O.filtering_mode & LIBRAW_FILTERING_AUTOMATIC_BIT)
            O.filtering_mode = LIBRAW_FILTERING_AUTOMATIC; // restore automated mode

        return 0;
    }
    catch ( LibRaw_exceptions err) {
        EXCEPTION_HANDLER(err);
    }

}

#if 1
#define FORC(cnt) for (c=0; c < cnt; c++)
#define FORCC FORC(ret->colors)
#define SWAP(a,b) { a ^= b; a ^= (b ^= a); }

libraw_processed_image_t * LibRaw::dcraw_make_mem_thumb(int *errcode)
{
    if(!T.thumb)
        {
            if ( !ID.toffset) 
                {
                    if(errcode) *errcode= LIBRAW_NO_THUMBNAIL;
                }
            else
                {
                    if(errcode) *errcode= LIBRAW_OUT_OF_ORDER_CALL;
                }
            return NULL;
        }

    if (T.tformat == LIBRAW_THUMBNAIL_BITMAP)
        {
            libraw_processed_image_t * ret = 
                (libraw_processed_image_t *)::malloc(sizeof(libraw_processed_image_t)+T.tlength);

            if(!ret)
                {
                    if(errcode) *errcode= ENOMEM;
                    return NULL;
                }

            memset(ret,0,sizeof(libraw_processed_image_t));
            ret->type   = LIBRAW_IMAGE_BITMAP;
            ret->height = T.theight;
            ret->width  = T.twidth;
            ret->colors = 3; 
            ret->bits   = 8;
            ret->data_size = T.tlength;
            memmove(ret->data,T.thumb,T.tlength);
            if(errcode) *errcode= 0;
            return ret;
        }
    else if (T.tformat == LIBRAW_THUMBNAIL_JPEG)
        {
            ushort exif[5];
            int mk_exif = 0;
            if(strcmp(T.thumb+6,"Exif")) mk_exif = 1;
            
            int dsize = T.tlength + mk_exif * (sizeof(exif)+sizeof(tiff_hdr));

            libraw_processed_image_t * ret = 
                (libraw_processed_image_t *)::malloc(sizeof(libraw_processed_image_t)+dsize);

            if(!ret)
                {
                    if(errcode) *errcode= ENOMEM;
                    return NULL;
                }

            memset(ret,0,sizeof(libraw_processed_image_t));

            ret->type = LIBRAW_IMAGE_JPEG;
            ret->data_size = dsize;
            
            ret->data[0] = 0xff;
            ret->data[1] = 0xd8;
            if(mk_exif)
                {
                    struct tiff_hdr th;
                    memcpy (exif, "\xff\xe1  Exif\0\0", 10);
                    exif[1] = htons (8 + sizeof th);
                    memmove(ret->data+2,exif,sizeof(exif));
                    tiff_head (&th, 0);
                    memmove(ret->data+(2+sizeof(exif)),&th,sizeof(th));
                    memmove(ret->data+(2+sizeof(exif)+sizeof(th)),T.thumb+2,T.tlength-2);
                }
            else
                {
                    memmove(ret->data+2,T.thumb+2,T.tlength-2);
                }
            if(errcode) *errcode= 0;
            return ret;
            
        }
    else
        {
            if(errcode) *errcode= LIBRAW_UNSUPPORTED_THUMBNAIL;
            return NULL;

        }
}



libraw_processed_image_t *LibRaw::dcraw_make_mem_image(int *errcode)
{
    if((imgdata.progress_flags & LIBRAW_PROGRESS_THUMB_MASK) < LIBRAW_PROGRESS_PRE_INTERPOLATE)
            {
                if(errcode) *errcode= LIBRAW_OUT_OF_ORDER_CALL;
                return NULL;
            }

    if(libraw_internal_data.output_data.histogram)
        {
            int perc, val, total, t_white=0x2000,c;

            perc = S.width * S.height * 0.01;		/* 99th percentile white level */
            if (IO.fuji_width) perc /= 2;
            if (!((O.highlight & ~2) || O.no_auto_bright))
                for (t_white=c=0; c < P1.colors; c++) {
                    for (val=0x2000, total=0; --val > 32; )
                        if ((total += libraw_internal_data.output_data.histogram[c][val]) > perc) break;
                    if (t_white < val) t_white = val;
                }
            gamma_curve (O.gamm[0], O.gamm[1], 2, (t_white << 3)/O.bright);
        }

    unsigned ds = S.height * S.width * (O.output_bps/8) * P1.colors;
    libraw_processed_image_t *ret = (libraw_processed_image_t*)::malloc(sizeof(libraw_processed_image_t)+ds);
    if(!ret)
        {
                if(errcode) *errcode= ENOMEM;
                return NULL;
        }
    memset(ret,0,sizeof(libraw_processed_image_t));
    // metadata init

    int s_iheight = S.iheight;
    int s_iwidth = S.iwidth;
    int s_width = S.width;
    int s_hwight = S.height;

    S.iheight = S.height;
    S.iwidth  = S.width;


    if (S.flip & 4) SWAP(S.height,S.width);


    ret->type   = LIBRAW_IMAGE_BITMAP;
    ret->height = S.height;
    ret->width  = S.width;
    ret->colors = P1.colors;
    ret->bits   = O.output_bps;

    ret->data_size = ds;

    // Cut'n'paste from write_tiff_ppm, should be generalized later
    uchar *bufp = ret->data;
    uchar *ppm;
    ushort *ppm2;
    int c, row, col, soff, rstep, cstep;


    soff  = flip_index (0, 0);
    cstep = flip_index (0, 1) - soff;
    rstep = flip_index (1, 0) - flip_index (0, S.width);


    for (row=0; row < ret->height; row++, soff += rstep) 
        {
            ppm2 = (ushort*) (ppm = bufp);
            for (col=0; col < ret->width; col++, soff += cstep)
                if (ret->bits == 8)
                    FORCC ppm [col*ret->colors+c] = imgdata.color.curve[imgdata.image[soff][c]]>>8;
                else
                    FORCC ppm2[col*ret->colors+c] =     imgdata.color.curve[imgdata.image[soff][c]];
            bufp+=ret->colors*(ret->bits/8)*ret->width;
        }
    if(errcode) *errcode= 0;

    S.iheight = s_iheight;
    S.iwidth = s_iwidth;
    S.width = s_width;
    S.height = s_hwight;

    return ret;
}

#undef FORC
#undef FORCC
#undef SWAP
#endif


int LibRaw::dcraw_ppm_tiff_writer(const char *filename)
{
    CHECK_ORDER_LOW(LIBRAW_PROGRESS_LOAD_RAW);

    if(!imgdata.image) 
        return LIBRAW_OUT_OF_ORDER_CALL;

    if(!filename) 
        return ENOENT;
    FILE *f = fopen(filename,"wb");

    if(!f) 
        return errno;
        
    try {
        if(!libraw_internal_data.output_data.histogram)
            {
                libraw_internal_data.output_data.histogram = 
                    (int (*)[LIBRAW_HISTOGRAM_SIZE]) malloc(sizeof(*libraw_internal_data.output_data.histogram)*4);
                merror(libraw_internal_data.output_data.histogram,"LibRaw::dcraw_ppm_tiff_writer()");
            }
        libraw_internal_data.internal_data.output = f;
        write_ppm_tiff();
        SET_PROC_FLAG(LIBRAW_PROGRESS_FLIP);
        libraw_internal_data.internal_data.output = NULL;
        fclose(f);
        return 0;
    }
    catch ( LibRaw_exceptions err) {
        fclose(f);
        EXCEPTION_HANDLER(err);
    }
}

void LibRaw::kodak_thumb_loader()
{
    // some kodak cameras
    ushort s_height = S.height, s_width = S.width,s_iwidth = S.iwidth,s_iheight=S.iheight;
    int s_colors = P1.colors;
    unsigned s_filters = P1.filters;
    ushort (*s_image)[4] = imgdata.image;

    
    S.height = T.theight;
    S.width  = T.twidth;
    P1.filters = 0;

    if (thumb_load_raw == &CLASS kodak_ycbcr_load_raw) 
        {
            S.height += S.height & 1;
            S.width  += S.width  & 1;
        }
    
    imgdata.image = (ushort (*)[4]) calloc (S.iheight*S.iwidth, sizeof (*imgdata.image));
    merror (imgdata.image, "LibRaw::kodak_thumb_loader()");

    ID.input->seek(ID.toffset, SEEK_SET);
    // read kodak thumbnail into T.image[]
    (this->*thumb_load_raw)();

    // copy-n-paste from image pipe
#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define LIM(x,min,max) MAX(min,MIN(x,max))
#define CLIP(x) LIM(x,0,65535)
#define SWAP(a,b) { a ^= b; a ^= (b ^= a); }

    // from scale_colors
    {
        double   dmax;
        float scale_mul[4];
        int c,val;
        for (dmax=DBL_MAX, c=0; c < 3; c++) 
                if (dmax > C.pre_mul[c])
                    dmax = C.pre_mul[c];

        for( c=0; c< 3; c++)
                scale_mul[c] = (C.pre_mul[c] / dmax) * 65535.0 / C.maximum;
        scale_mul[3] = scale_mul[1];

        size_t size = S.height * S.width;
        for (unsigned i=0; i < size*4 ; i++) 
            {
                val = imgdata.image[0][i];
                if(!val) continue;
                val *= scale_mul[i & 3];
                imgdata.image[0][i] = CLIP(val);
            }
    }

    // from convert_to_rgb
    ushort *img;
    int row,col;
    
    int  (*t_hist)[LIBRAW_HISTOGRAM_SIZE] =  (int (*)[LIBRAW_HISTOGRAM_SIZE]) calloc(sizeof(*t_hist),4);
    merror (t_hist, "LibRaw::kodak_thumb_loader()");
    
    float out[3], 
        out_cam[3][4] = 
        {
            {2.81761312, -1.98369181, 0.166078627, 0}, 
            {-0.111855984, 1.73688626, -0.625030339, 0}, 
            {-0.0379119813, -0.891268849, 1.92918086, 0}
        };

    for (img=imgdata.image[0], row=0; row < S.height; row++)
        for (col=0; col < S.width; col++, img+=4)
            {
                out[0] = out[1] = out[2] = 0;
                int c;
                for(c=0;c<3;c++) 
                    {
                        out[0] += out_cam[0][c] * img[c];
                        out[1] += out_cam[1][c] * img[c];
                        out[2] += out_cam[2][c] * img[c];
                    }
                for(c=0; c<3; c++)
                    img[c] = CLIP((int) out[c]);
                for(c=0; c<P1.colors;c++)
                    t_hist[c][img[c] >> 3]++;
                    
            }

    // from gamma_lut
    int  (*save_hist)[LIBRAW_HISTOGRAM_SIZE] = libraw_internal_data.output_data.histogram;
    libraw_internal_data.output_data.histogram = t_hist;

    // make curve output curve!
    ushort (*t_curve) = (ushort*) calloc(sizeof(C.curve),1);
    merror (t_curve, "LibRaw::kodak_thumb_loader()");
    memmove(t_curve,C.curve,sizeof(C.curve));
    memset(C.curve,0,sizeof(C.curve));
        {
            int perc, val, total, t_white=0x2000,c;

            perc = S.width * S.height * 0.01;		/* 99th percentile white level */
            if (IO.fuji_width) perc /= 2;
            if (!((O.highlight & ~2) || O.no_auto_bright))
                for (t_white=c=0; c < P1.colors; c++) {
                    for (val=0x2000, total=0; --val > 32; )
                        if ((total += libraw_internal_data.output_data.histogram[c][val]) > perc) break;
                    if (t_white < val) t_white = val;
                }
            gamma_curve (O.gamm[0], O.gamm[1], 2, (t_white << 3)/O.bright);
        }
    
    libraw_internal_data.output_data.histogram = save_hist;
    free(t_hist);
    
    // from write_ppm_tiff - copy pixels into bitmap
    
    S.iheight = S.height;
    S.iwidth  = S.width;
    if (S.flip & 4) SWAP(S.height,S.width);

    if(T.thumb) free(T.thumb);
    T.thumb = (char*) calloc (S.width * S.height, P1.colors);
    merror (T.thumb, "LibRaw::kodak_thumb_loader()");
    T.tlength = S.width * S.height * P1.colors;

    // from write_tiff_ppm
    {
        int soff  = flip_index (0, 0);
        int cstep = flip_index (0, 1) - soff;
        int rstep = flip_index (1, 0) - flip_index (0, S.width);
        
        for (int row=0; row < S.height; row++, soff += rstep) 
            {
                char *ppm = T.thumb + row*S.width*P1.colors;
                for (int col=0; col < S.width; col++, soff += cstep)
                    for(int c = 0; c < P1.colors; c++)
                        ppm [col*P1.colors+c] = imgdata.color.curve[imgdata.image[soff][c]]>>8;
            }
    }

    memmove(C.curve,t_curve,sizeof(C.curve));
    free(t_curve);

    // restore variables
    free(imgdata.image);
    imgdata.image  = s_image;
    
    T.twidth = S.width;
    S.width = s_width;

    S.iwidth = s_iwidth;
    S.iheight = s_iheight;

    T.theight = S.height;
    S.height = s_height;

    T.tcolors = P1.colors;
    P1.colors = s_colors;

    P1.filters = s_filters;
}
#undef MIN
#undef MAX
#undef LIM
#undef CLIP
#undef SWAP




// Достает thumbnail из файла, ставит thumb_format в соответствии с форматом
int LibRaw::unpack_thumb(void)
{
    CHECK_ORDER_LOW(LIBRAW_PROGRESS_IDENTIFY);
    CHECK_ORDER_BIT(LIBRAW_PROGRESS_THUMB_LOAD);

    try {
        if ( !ID.toffset) 
            {
                return LIBRAW_NO_THUMBNAIL;
            } 
        else if (thumb_load_raw) 
            {
                kodak_thumb_loader();
                T.tformat = LIBRAW_THUMBNAIL_BITMAP;
                SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
                return 0;
            } 
        else 
            {
                ID.input->seek(ID.toffset, SEEK_SET);
                if ( write_thumb == &LibRaw::jpeg_thumb)
                    {
                        if(T.thumb) free(T.thumb);
                        T.thumb = (char *) malloc (T.tlength);
                        merror (T.thumb, "jpeg_thumb()");
                        ID.input->read (T.thumb, 1, T.tlength);
                        T.tcolors = 3;
                        T.tformat = LIBRAW_THUMBNAIL_JPEG;
                        SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
                        return 0;
                    }
                else if (write_thumb == &LibRaw::ppm_thumb)
                    {
                        T.tlength = T.twidth * T.theight*3;
                        if(T.thumb) free(T.thumb);

                        T.thumb = (char *) malloc (T.tlength);
                        merror (T.thumb, "ppm_thumb()");

                        ID.input->read(T.thumb, 1, T.tlength);

                        T.tformat = LIBRAW_THUMBNAIL_BITMAP;
                        SET_PROC_FLAG(LIBRAW_PROGRESS_THUMB_LOAD);
                        return 0;

                    }
                // else if -- all other write_thumb cases!
                else
                    {
                        return LIBRAW_UNSUPPORTED_THUMBNAIL;
                    }
            }
        // last resort
        return LIBRAW_UNSUPPORTED_THUMBNAIL;
    }
    catch ( LibRaw_exceptions err) {
        EXCEPTION_HANDLER(err);
    }

}

int LibRaw::dcraw_thumb_writer(const char *fname)
{
//    CHECK_ORDER_LOW(LIBRAW_PROGRESS_THUMB_LOAD);

    if(!fname) 
        return ENOENT;
        
    FILE *tfp = fopen(fname,"wb");
    
    if(!tfp) 
        return errno;

    if(!T.thumb)
	{
		fclose(tfp);
        	return LIBRAW_OUT_OF_ORDER_CALL;
	}

    try {
        switch (T.tformat)
            {
            case LIBRAW_THUMBNAIL_JPEG:
                jpeg_thumb_writer (tfp,T.thumb,T.tlength);
                break;
            case LIBRAW_THUMBNAIL_BITMAP:
                fprintf (tfp, "P6\n%d %d\n255\n", T.twidth, T.theight);
                fwrite (T.thumb, 1, T.tlength, tfp);
                break;
            default:
                fclose(tfp);
                return LIBRAW_UNSUPPORTED_THUMBNAIL;
           }
        fclose(tfp);
        return 0;
    }
    catch ( LibRaw_exceptions err) {
        fclose(tfp);
        EXCEPTION_HANDLER(err);
    }
}

int LibRaw::adjust_sizes_info_only(void)
{
    CHECK_ORDER_LOW(LIBRAW_PROGRESS_IDENTIFY);
    CHECK_ORDER_HIGH(LIBRAW_PROGRESS_FUJI_ROTATE);
    if (O.use_fuji_rotate)
        {
            if (IO.fuji_width) 
                {
                    // restore saved values
                    if(IO.fheight)
                        {
                            S.height = IO.fheight;
                            S.width = IO.fwidth;
                            S.iheight = (S.height + IO.shrink) >> IO.shrink;
                            S.iwidth  = (S.width  + IO.shrink) >> IO.shrink;
                            S.raw_height -= 2*S.top_margin;
                            IO.fheight = IO.fwidth = 0; // prevent repeated calls
                        }
                    // dcraw code
                    IO.fuji_width = (IO.fuji_width - 1 + IO.shrink) >> IO.shrink;
                    S.iwidth = (ushort)(IO.fuji_width / sqrt(0.5));
                    S.iheight = (ushort)( (S.iheight - IO.fuji_width) / sqrt(0.5));
                } 
            else 
                {
                    if (S.pixel_aspect < 1) S.iheight = (ushort)( S.iheight / S.pixel_aspect + 0.5);
                    if (S.pixel_aspect > 1) S.iwidth  = (ushort) (S.iwidth  * S.pixel_aspect + 0.5);
                }
        }
    SET_PROC_FLAG(LIBRAW_PROGRESS_FUJI_ROTATE);
    if (S.flip & 4)
        {
            unsigned short t = S.iheight;
            S.iheight=S.iwidth;
            S.iwidth = t;
            SET_PROC_FLAG(LIBRAW_PROGRESS_FLIP);
        }
    return 0;
}

int LibRaw::rotate_fuji_raw(void)
{
    CHECK_ORDER_LOW(LIBRAW_PROGRESS_LOAD_RAW);
    CHECK_ORDER_HIGH(LIBRAW_PROGRESS_PRE_INTERPOLATE);


    if(!IO.fwidth) return LIBRAW_SUCCESS;
    int row,col,r,c;
    ushort (*newimage)[4];
    ushort fiwidth,fiheight;

    fiheight = (IO.fheight + IO.shrink) >> IO.shrink;
    fiwidth = (IO.fwidth + IO.shrink) >> IO.shrink;
    
    newimage = (ushort (*)[4]) calloc (fiheight*fiwidth, sizeof (*newimage));
    merror(newimage,"rotate_fuji_raw()");
    for(row=0;row<S.height;row++)
        {
            for(col=0;col<S.width;col++)
                {
                    if (libraw_internal_data.unpacker_data.fuji_layout) {
                        r = IO.fuji_width - 1 - col + (row >> 1);
                        c = col + ((row+1) >> 1);
                    } else {
                        r = IO.fuji_width - 1 + row - (col >> 1);
                        c = row + ((col+1) >> 1);
                    }
                    newimage[((r) >> IO.shrink)*fiwidth + ((c) >> IO.shrink)][FCF(row,col)] = 
                        imgdata.image[((row) >> IO.shrink)*S.iwidth + ((col) >> IO.shrink)][FCF(row,col)];
                }
        }
    // restore fuji sizes!
    S.height = IO.fheight;
    S.width = IO.fwidth;
    S.iheight = (S.height + IO.shrink) >> IO.shrink;
    S.iwidth  = (S.width  + IO.shrink) >> IO.shrink;
    S.raw_height -= 2*S.top_margin;
    IO.fheight = IO.fwidth = 0; // prevent repeated calls

    free(imgdata.image);
    imgdata.image = newimage;
    return LIBRAW_SUCCESS;
    
}


int LibRaw::dcraw_process(void)
{
    int quality,i;


    CHECK_ORDER_LOW(LIBRAW_PROGRESS_LOAD_RAW);
    CHECK_ORDER_HIGH(LIBRAW_PROGRESS_PRE_INTERPOLATE);

    try {

        adjust_maximum();
        if(IO.fwidth) 
            rotate_fuji_raw();


        if(!own_filtering_supported() && (O.filtering_mode & LIBRAW_FILTERING_AUTOMATIC_BIT))
            O.filtering_mode = LIBRAW_FILTERING_AUTOMATIC_BIT; // turn on black and zeroes filtering

        if(O.half_size) 
            O.four_color_rgb = 1;

        if (!(O.filtering_mode & LIBRAW_FILTERING_NOZEROES) && IO.zero_is_bad) 
            {
                remove_zeroes();
                SET_PROC_FLAG(LIBRAW_PROGRESS_REMOVE_ZEROES);
            }
        if(O.bad_pixels) 
            {
                bad_pixels(O.bad_pixels);
                SET_PROC_FLAG(LIBRAW_PROGRESS_BAD_PIXELS);
            }
        if (O.dark_frame)
            {
                subtract (O.dark_frame);
                SET_PROC_FLAG(LIBRAW_PROGRESS_DARK_FRAME);
            }

        quality = 2 + !IO.fuji_width;

        if(O.filtering_mode & LIBRAW_FILTERING_NOBLACKS)
            C.black=0;

        if (O.user_qual >= 0) quality = O.user_qual;
        if (O.user_black >= 0) C.black = O.user_black;
        if (O.user_sat > 0) C.maximum = O.user_sat;


        if ( O.document_mode < 2)
            {
                scale_colors();
                SET_PROC_FLAG(LIBRAW_PROGRESS_SCALE_COLORS);
            }

        pre_interpolate();
        SET_PROC_FLAG(LIBRAW_PROGRESS_PRE_INTERPOLATE);

        if (P1.filters && !O.document_mode) 
            {
                if (quality == 0)
                    lin_interpolate();
                else if (quality == 1 || P1.colors > 3)
                    vng_interpolate();
                else if (quality == 2)
                    ppg_interpolate();
                else 
                    ahd_interpolate();
                SET_PROC_FLAG(LIBRAW_PROGRESS_INTERPOLATE);
            }
        if (IO.mix_green)
            {
                for (P1.colors=3, i=0; i < S.height * S.width; i++)
                    imgdata.image[i][1] = (imgdata.image[i][1] + imgdata.image[i][3]) >> 1;
                SET_PROC_FLAG(LIBRAW_PROGRESS_MIX_GREEN);
            }

        if (P1.colors == 3) 
            {
                median_filter();
                SET_PROC_FLAG(LIBRAW_PROGRESS_MEDIAN_FILTER);
            }
        
        if (O.highlight == 2) 
            {
                blend_highlights();
                SET_PROC_FLAG(LIBRAW_PROGRESS_HIGHLIGHTS);
            }
        
        if (O.highlight > 2) 
            {
                recover_highlights();
                SET_PROC_FLAG(LIBRAW_PROGRESS_HIGHLIGHTS);
            }
        
        if (O.use_fuji_rotate) 
            {
                fuji_rotate();
                SET_PROC_FLAG(LIBRAW_PROGRESS_FUJI_ROTATE);
            }
    
        if(!libraw_internal_data.output_data.histogram)
            {
                libraw_internal_data.output_data.histogram = (int (*)[LIBRAW_HISTOGRAM_SIZE]) malloc(sizeof(*libraw_internal_data.output_data.histogram)*4);
                merror(libraw_internal_data.output_data.histogram,"LibRaw::dcraw_process()");
            }
#ifndef NO_LCMS
	if(O.camera_profile)
            {
                apply_profile(O.camera_profile,O.output_profile);
                SET_PROC_FLAG(LIBRAW_PROGRESS_APPLY_PROFILE);
            }
#endif

        convert_to_rgb();
        SET_PROC_FLAG(LIBRAW_PROGRESS_CONVERT_RGB);

        if (O.use_fuji_rotate) 
            {
                stretch();
                SET_PROC_FLAG(LIBRAW_PROGRESS_STRETCH);
            }
        if (O.filtering_mode & LIBRAW_FILTERING_AUTOMATIC_BIT)
            O.filtering_mode = LIBRAW_FILTERING_AUTOMATIC; // restore automated mode
        return 0;
    }
    catch ( LibRaw_exceptions err) {
        EXCEPTION_HANDLER(err);
    }
}

// Supported cameras:
static const char  *static_camera_list[] = 
{
"Adobe Digital Negative (DNG)",
"AgfaPhoto DC-833m",
"Apple QuickTake 100",
"Apple QuickTake 150",
"Apple QuickTake 200",
"AVT F-080C",
"AVT F-145C",
"AVT F-201C",
"AVT F-510C",
"AVT F-810C",
"Canon PowerShot 600",
"Canon PowerShot A5",
"Canon PowerShot A5 Zoom",
"Canon PowerShot A50",
"Canon PowerShot A460 (CHDK hack)",
"Canon PowerShot A470 (CHDK hack)",
"Canon PowerShot A530 (CHDK hack)",
"Canon PowerShot A570 (CHDK hack)",
"Canon PowerShot A590 (CHDK hack)",
"Canon PowerShot A610 (CHDK hack)",
"Canon PowerShot A620 (CHDK hack)",
"Canon PowerShot A630 (CHDK hack)",
"Canon PowerShot A640 (CHDK hack)",
"Canon PowerShot A650 (CHDK hack)",
"Canon PowerShot A710 IS (CHDK hack)",
"Canon PowerShot A720 IS (CHDK hack)",
"Canon PowerShot Pro70",
"Canon PowerShot Pro90 IS",
"Canon PowerShot Pro1",
"Canon PowerShot G1",
"Canon PowerShot G2",
"Canon PowerShot G3",
"Canon PowerShot G5",
"Canon PowerShot G6",
"Canon PowerShot G7 (CHDK hack)",
"Canon PowerShot G9",
"Canon PowerShot G10",
"Canon PowerShot G11",
"Canon PowerShot S2 IS (CHDK hack)",
"Canon PowerShot S3 IS (CHDK hack)",
"Canon PowerShot S5 IS (CHDK hack)",
"Canon PowerShot SD300 (CHDK hack)",
"Canon PowerShot S30",
"Canon PowerShot S40",
"Canon PowerShot S45",
"Canon PowerShot S50",
"Canon PowerShot S60",
"Canon PowerShot S70",
"Canon PowerShot S90",
"Canon PowerShot SX1 IS",
"Canon PowerShot SX110 IS (CHDK hack)",
"Canon EOS D30",
"Canon EOS D60",
"Canon EOS 5D",
"Canon EOS 5D Mark II",
"Canon EOS 7D",
"Canon EOS 10D",
"Canon EOS 20D",
"Canon EOS 30D",
"Canon EOS 40D",
"Canon EOS 50D",
"Canon EOS 300D / Digital Rebel / Kiss Digital",
"Canon EOS 350D / Digital Rebel XT / Kiss Digital N",
"Canon EOS 400D / Digital Rebel XTi / Kiss Digital X",
"Canon EOS 450D / Digital Rebel XSi / Kiss Digital X2",
"Canon EOS 500D / Digital Rebel T1i / Kiss Digital X3",
"Canon EOS 1000D / Digital Rebel XS / Kiss Digital F",
"Canon EOS D2000C",
"Canon EOS-1D",
"Canon EOS-1DS",
"Canon EOS-1D Mark II",
"Canon EOS-1D Mark II N",
"Canon EOS-1D Mark III",
"Canon EOS-1D Mark IV",
"Canon EOS-1Ds Mark II",
"Canon EOS-1Ds Mark III",
"Casio QV-2000UX",
"Casio QV-3000EX",
"Casio QV-3500EX",
"Casio QV-4000",
"Casio QV-5700",
"Casio QV-R41",
"Casio QV-R51",
"Casio QV-R61",
"Casio EX-S20",
"Casio EX-S100",
"Casio EX-Z4",
"Casio EX-Z50",
"Casio EX-Z55",
"Casio EX-Z60",
"Casio EX-Z75",
"Casio EX-Z750",
"Casio EX-Z850",
"Casio Exlim Pro 505",
"Casio Exlim Pro 600",
"Casio Exlim Pro 700",
"Contax N Digital",
"Creative PC-CAM 600",
"Epson R-D1",
"Foculus 531C",
"Fuji FinePix E550",
"Fuji FinePix E900",
"Fuji FinePix F700",
"Fuji FinePix F710",
"Fuji FinePix F800",
"Fuji FinePix F810",
"Fuji FinePix S2Pro",
"Fuji FinePix S3Pro",
"Fuji FinePix S5Pro",
"Fuji FinePix S20Pro",
"Fuji FinePix S100FS",
"Fuji FinePix S5000",
"Fuji FinePix S5100/S5500",
"Fuji FinePix S5200/S5600",
"Fuji FinePix S6000fd",
"Fuji FinePix S7000",
"Fuji FinePix S9000/S9500",
"Fuji FinePix S9100/S9600",
"Fuji FinePix S200EXR",
"Fuji IS-1",
"Hasselblad CFV",
"Hasselblad H3D",
"Hasselblad V96C",
"Imacon Ixpress 16-megapixel",
"Imacon Ixpress 22-megapixel",
"Imacon Ixpress 39-megapixel",
"ISG 2020x1520",
"Kodak DC20 (see Oliver Hartman's page)",
"Kodak DC25 (see Jun-ichiro Itoh's page)",
"Kodak DC40",
"Kodak DC50",
"Kodak DC120 (also try kdc2tiff)",
"Kodak DCS200",
"Kodak DCS315C",
"Kodak DCS330C",
"Kodak DCS420",
"Kodak DCS460",
"Kodak DCS460A",
"Kodak DCS520C",
"Kodak DCS560C",
"Kodak DCS620C",
"Kodak DCS620X",
"Kodak DCS660C",
"Kodak DCS660M",
"Kodak DCS720X",
"Kodak DCS760C",
"Kodak DCS760M",
"Kodak EOSDCS1",
"Kodak EOSDCS3B",
"Kodak NC2000F",
"Kodak ProBack",
"Kodak PB645C",
"Kodak PB645H",
"Kodak PB645M",
"Kodak DCS Pro 14n",
"Kodak DCS Pro 14nx",
"Kodak DCS Pro SLR/c",
"Kodak DCS Pro SLR/n",
"Kodak C330",
"Kodak C603",
"Kodak P850",
"Kodak P880",
"Kodak Z980",
"Kodak Z1015",
"Kodak KAI-0340",
"Konica KD-400Z",
"Konica KD-510Z",
"Leaf AFi 7",
"Leaf Aptus 17",
"Leaf Aptus 22",
"Leaf Aptus 54S",
"Leaf Aptus 65",
"Leaf Aptus 75",
"Leaf Aptus 75S",
"Leaf Cantare",
"Leaf CatchLight",
"Leaf CMost",
"Leaf DCB2",
"Leaf Valeo 6",
"Leaf Valeo 11",
"Leaf Valeo 17",
"Leaf Valeo 22",
"Leaf Volare",
"Leica Digilux 2",
"Leica Digilux 3",
"Leica D-LUX2",
"Leica D-LUX3",
"Leica D-LUX4",
"Leica V-LUX1",
"Logitech Fotoman Pixtura",
"Mamiya ZD",
"Micron 2010",
"Minolta RD175",
"Minolta DiMAGE 5",
"Minolta DiMAGE 7",
"Minolta DiMAGE 7i",
"Minolta DiMAGE 7Hi",
"Minolta DiMAGE A1",
"Minolta DiMAGE A2",
"Minolta DiMAGE A200",
"Minolta DiMAGE G400",
"Minolta DiMAGE G500",
"Minolta DiMAGE G530",
"Minolta DiMAGE G600",
"Minolta DiMAGE Z2",
"Minolta Alpha/Dynax/Maxxum 5D",
"Minolta Alpha/Dynax/Maxxum 7D",
"Motorola PIXL",
"Nikon D1",
"Nikon D1H",
"Nikon D1X",
"Nikon D2H",
"Nikon D2Hs",
"Nikon D2X",
"Nikon D2Xs",
"Nikon D3",
"Nikon D3X",
"Nikon D40",
"Nikon D40X",
"Nikon D50",
"Nikon D60",
"Nikon D70",
"Nikon D70s",
"Nikon D80",
"Nikon D90",
"Nikon D100",
"Nikon D200",
"Nikon D300",
"Nikon D300s",
"Nikon D700",
"Nikon D3000",
"Nikon D5000",
"Nikon E700 (\"DIAG RAW\" hack)",
"Nikon E800 (\"DIAG RAW\" hack)",
"Nikon E880 (\"DIAG RAW\" hack)",
"Nikon E900 (\"DIAG RAW\" hack)",
"Nikon E950 (\"DIAG RAW\" hack)",
"Nikon E990 (\"DIAG RAW\" hack)",
"Nikon E995 (\"DIAG RAW\" hack)",
"Nikon E2100 (\"DIAG RAW\" hack)",
"Nikon E2500 (\"DIAG RAW\" hack)",
"Nikon E3200 (\"DIAG RAW\" hack)",
"Nikon E3700 (\"DIAG RAW\" hack)",
"Nikon E4300 (\"DIAG RAW\" hack)",
"Nikon E4500 (\"DIAG RAW\" hack)",
"Nikon E5000",
"Nikon E5400",
"Nikon E5700",
"Nikon E8400",
"Nikon E8700",
"Nikon E8800",
"Nikon Coolpix P6000",
"Nikon Coolpix S6 (\"DIAG RAW\" hack)",
"Nokia N95",
"Olympus C3030Z",
"Olympus C5050Z",
"Olympus C5060WZ",
"Olympus C7070WZ",
"Olympus C70Z,C7000Z",
"Olympus C740UZ",
"Olympus C770UZ",
"Olympus C8080WZ",
"Olympus X200,D560Z,C350Z",
"Olympus E-1",
"Olympus E-3",
"Olympus E-10",
"Olympus E-20",
"Olympus E-30",
"Olympus E-300",
"Olympus E-330",
"Olympus E-400",
"Olympus E-410",
"Olympus E-420",
"Olympus E-500",
"Olympus E-510",
"Olympus E-520",
"Olympus E-620",
"Olympus E-P1",
"Olympus SP310",
"Olympus SP320",
"Olympus SP350",
"Olympus SP500UZ",
"Olympus SP510UZ",
"Olympus SP550UZ",
"Olympus SP560UZ",
"Olympus SP570UZ",
"Panasonic DMC-FZ8",
"Panasonic DMC-FZ18",
"Panasonic DMC-FZ28",
"Panasonic DMC-FZ30",
"Panasonic DMC-FZ35/FZ38",
"Panasonic DMC-FZ50",
"Panasonic DMC-FX150",
"Panasonic DMC-G1",
"Panasonic DMC-GH1",
"Panasonic DMC-L1",
"Panasonic DMC-L10",
"Panasonic DMC-LC1",
"Panasonic DMC-LX1",
"Panasonic DMC-LX2",
"Panasonic DMC-LX3",
"Pentax *ist D",
"Pentax *ist DL",
"Pentax *ist DL2",
"Pentax *ist DS",
"Pentax *ist DS2",
"Pentax K10D",
"Pentax K20D",
"Pentax K100D",
"Pentax K100D Super",
"Pentax K200D",
"Pentax K2000/K-m",
"Pentax K-x",
"Pentax K-7",
"Pentax Optio S",
"Pentax Optio S4",
"Pentax Optio 33WR",
"Pentax Optio 750Z",
"Phase One LightPhase",
"Phase One H 10",
"Phase One H 20",
"Phase One H 25",
"Phase One P 20",
"Phase One P 25",
"Phase One P 30",
"Phase One P 45",
"Phase One P 45+",
"Pixelink A782",
"Rollei d530flex",
"RoverShot 3320af",
"Samsung GX-1S",
"Samsung GX-10",
"Samsung S85 (hacked)",
"Samsung S850 (hacked)",
"Sarnoff 4096x5440",
"Sinar 3072x2048",
"Sinar 4080x4080",
"Sinar 4080x5440",
"Sinar STI format",
"SMaL Ultra-Pocket 3",
"SMaL Ultra-Pocket 4",
"SMaL Ultra-Pocket 5",
"Sony DSC-F828",
"Sony DSC-R1",
"Sony DSC-V3",
"Sony DSLR-A100",
"Sony DSLR-A200",
"Sony DSLR-A300",
"Sony DSLR-A330",
"Sony DSLR-A350",
"Sony DSLR-A380",
"Sony DSLR-A500",
"Sony DSLR-A550",
"Sony DSLR-A700",
"Sony DSLR-A850",
"Sony DSLR-A900",
"Sony XCD-SX910CR",
"STV680 VGA",

   NULL
};

const char** LibRaw::cameraList() { return static_camera_list;}
int LibRaw::cameraCount() { return (sizeof(static_camera_list)/sizeof(static_camera_list[0]))-1; }


const char * LibRaw::strprogress(enum LibRaw_progress p)
{
    switch(p)
        {
        case LIBRAW_PROGRESS_START:
            return "Starting";
        case LIBRAW_PROGRESS_OPEN :
            return "Opening file";
        case LIBRAW_PROGRESS_IDENTIFY :
            return "Reading metadata";
        case LIBRAW_PROGRESS_SIZE_ADJUST:
            return "Adjusting size";
        case LIBRAW_PROGRESS_LOAD_RAW:
            return "Reading RAW data";
        case LIBRAW_PROGRESS_REMOVE_ZEROES:
            return "Clearing zero values";
        case LIBRAW_PROGRESS_BAD_PIXELS :
            return "Removing dead pixels";
        case LIBRAW_PROGRESS_DARK_FRAME:
            return "Subtracting dark frame data";
        case LIBRAW_PROGRESS_SCALE_COLORS:
            return "Scaling colors";
        case LIBRAW_PROGRESS_PRE_INTERPOLATE:
            return "Pre-interpolating";
        case LIBRAW_PROGRESS_INTERPOLATE:
            return "Interpolating";
        case LIBRAW_PROGRESS_MIX_GREEN :
            return "Mixing green channels";
        case LIBRAW_PROGRESS_MEDIAN_FILTER   :
            return "Median filter";
        case LIBRAW_PROGRESS_HIGHLIGHTS:
            return "Highlight recovery";
        case LIBRAW_PROGRESS_FUJI_ROTATE :
            return "Rotating Fuji diagonal data";
        case LIBRAW_PROGRESS_FLIP :
            return "Flipping image";
        case LIBRAW_PROGRESS_APPLY_PROFILE:
            return "ICC conversion";
        case LIBRAW_PROGRESS_CONVERT_RGB:
            return "Converting to RGB";
        case LIBRAW_PROGRESS_STRETCH:
            return "Stretching image";
        case LIBRAW_PROGRESS_THUMB_LOAD:
            return "Loading thumbnail";
        default:
            return "Some strange things";
        }
}
