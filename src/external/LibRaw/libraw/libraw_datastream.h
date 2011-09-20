/* -*- C -*-
 * File: libraw_datastream.h
 * Copyright 2008-2010 LibRaw LLC (info@libraw.org)
 * Created: Sun Jan 18 13:07:35 2009
 *
 * LibRaw Data stream interface

LibRaw is free software; you can redistribute it and/or modify
it under the terms of the one of three licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

3. LibRaw Software License 27032010
   (See file LICENSE.LibRaw.pdf provided in LibRaw distribution archive for details).

 */

#ifndef __LIBRAW_DATASTREAM_H
#define __LIBRAW_DATASTREAM_H

#include <stdio.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>

#ifndef __cplusplus


#else /* __cplusplus */

#include "libraw_const.h"
#include "libraw_types.h"
#include <fstream>
#include <memory>

#if defined (WIN32)
/* MSVS 2008 and above... */
#if _MSC_VER >= 1500
#define WIN32SECURECALLS
#endif
#endif

#define IOERROR() do { throw LIBRAW_EXCEPTION_IO_EOF; } while(0)


class LibRaw_byte_buffer
{
  public:
    LibRaw_byte_buffer(unsigned sz=0) 
        { 
            buf=0; size=sz; offt=0; do_free=0; 
            if(size)
                { 
                    buf = (unsigned char*)malloc(size); do_free=1;
                }
        }
    
    void set_buffer(void *bb, unsigned int sz) { buf = (unsigned char*)bb; size = sz; offt=0; do_free=0;}
    
    virtual ~LibRaw_byte_buffer() { if(do_free) free(buf);}

    int get_byte() { if(offt>=size) return EOF; return buf[offt++];}
    void unseek2() { if(offt>=2) offt-=2;}
    void *get_buffer() { return buf; }
    int get_ljpeg_byte() {
        if(offt>=size) return 0;
        unsigned char val = buf[offt++];
        if(val!=0xFF || offt >=size || buf[offt++]==0)
            return val;
        offt -=2;
        return 0;
    }

  private:
    unsigned char *buf;
    unsigned int  size,offt, do_free;
};

class LibRaw_bit_buffer
{
    unsigned bitbuf;
    int vbits, rst;
  public:
    LibRaw_bit_buffer() : bitbuf(0),vbits(0),rst(0) {}

        void reset() {  bitbuf=vbits=rst=0;}
#ifndef LIBRAW_LIBRARY_BUILD
        void fill_lj(LibRaw_byte_buffer* buf,int nbits);
        unsigned _getbits_lj(LibRaw_byte_buffer* buf, int nbits);
        unsigned _gethuff_lj(LibRaw_byte_buffer* buf, int nbits, unsigned short* huff);
        void fill(LibRaw_byte_buffer* buf,int nbits,int zer0_ff);
        unsigned _getbits(LibRaw_byte_buffer* buf, int nbits,int zer0_ff);
        unsigned _gethuff(LibRaw_byte_buffer* buf, int nbits, unsigned short* huff, int zer0_ff);
#else
        void fill_lj(LibRaw_byte_buffer* buf,int nbits)
        {
            unsigned c1,c2,c3;
            if(rst || nbits < vbits) return;
            int m = vbits >> 3;
            switch(m)
                {
                case 2:	
                    c1 = buf->get_ljpeg_byte();
                    bitbuf = (bitbuf <<8) | (c1);
                    vbits+=8;
                    break;
                case 1:
                    c1 = buf->get_ljpeg_byte();
                    c2 = buf->get_ljpeg_byte();
                    bitbuf = (bitbuf <<16) | (c1<<8) | c2;
                    vbits+=16;		
                    break;
                case 0:
                    c1 = buf->get_ljpeg_byte();
                    c2 = buf->get_ljpeg_byte();
                    c3 = buf->get_ljpeg_byte();
                    bitbuf = (bitbuf <<24) | (c1<<16) | (c2<<8)|c3;
                    vbits+=24;
                    break;
                }
        }

        unsigned _getbits_lj(LibRaw_byte_buffer* buf, int nbits)
        {
            unsigned c;
            if(nbits==0 || vbits < 0) return 0;
            fill_lj(buf,nbits);
            c = bitbuf << (32-vbits) >> (32-nbits);
            vbits-=nbits;
            if(vbits<0)throw LIBRAW_EXCEPTION_IO_EOF;
            return c;
        }
        unsigned _gethuff_lj(LibRaw_byte_buffer* buf, int nbits, unsigned short* huff)
        {
            unsigned c;
            if(nbits==0 || vbits < 0) return 0;
            fill_lj(buf,nbits);
            c = bitbuf << (32-vbits) >> (32-nbits);
            vbits -= huff[c] >> 8;
            c = (uchar) huff[c];
            if(vbits<0)throw LIBRAW_EXCEPTION_IO_EOF;
            return c;
        }
        void fill(LibRaw_byte_buffer* buf,int nbits,int zer0_ff)
        {
            unsigned c;
            while (!rst && vbits < nbits && (c = buf->get_byte()) != EOF &&
                   !(rst = zer0_ff && c == 0xff && buf->get_byte())) {
                bitbuf = (bitbuf << 8) + (uchar) c;
                vbits += 8;
            }
        }
        unsigned _getbits(LibRaw_byte_buffer* buf, int nbits,int zer0_ff)
        {
            unsigned c;
            if(nbits==0 || vbits < 0) return 0;
            fill(buf,nbits,zer0_ff);
            c = bitbuf << (32-vbits) >> (32-nbits);
            vbits-=nbits;
            if(vbits<0)throw LIBRAW_EXCEPTION_IO_EOF;
            return c;
        }
        unsigned _gethuff(LibRaw_byte_buffer* buf, int nbits, unsigned short* huff, int zer0_ff)
        {
            unsigned c;
            if(nbits==0 || vbits < 0) return 0;
            fill(buf,nbits,zer0_ff);
            c = bitbuf << (32-vbits) >> (32-nbits);
            vbits -= huff[c] >> 8;
            c = (uchar) huff[c];
            if(vbits<0)throw LIBRAW_EXCEPTION_IO_EOF;
            return c;
        }
#endif
};


class LibRaw_buffer_datastream;


class LibRaw_abstract_datastream
{
  public:
    LibRaw_abstract_datastream(){ substream=0;};
    virtual             ~LibRaw_abstract_datastream(void){if(substream) delete substream;}
    virtual int         valid() = 0;
    virtual int         read(void *,size_t, size_t ) = 0;
    virtual int         seek(INT64 , int ) = 0;
    virtual INT64       tell() = 0;
    virtual int         get_char() = 0;
    virtual char*       gets(char *, int) = 0;
    virtual int         scanf_one(const char *, void *) = 0;
    virtual int         eof() = 0;
    /* Make buffer from current offset */
    virtual LibRaw_byte_buffer *make_byte_buffer(unsigned int sz)
    {
        LibRaw_byte_buffer *ret = new LibRaw_byte_buffer(sz);
        read(ret->get_buffer(),sz,1);
        return ret;
    }

    /* subfile parsing not implemented in base class */
    virtual const char* fname(){ return NULL;};
    virtual int         subfile_open(const char*) { return -1;}
    virtual void        subfile_close() { }


    virtual int		tempbuffer_open(void*, size_t);
    virtual void	tempbuffer_close()
    {
        if(substream) delete substream;
        substream = NULL;
    }
  protected:
    LibRaw_abstract_datastream *substream;
};

class LibRaw_file_datastream: public LibRaw_abstract_datastream
{
  protected:
    std::auto_ptr<std::streambuf> f; /* will close() automatically through dtor */
    std::auto_ptr<std::streambuf> saved_f; /* when *f is a subfile, *saved_f is the master file */
    const char *filename;

  public:
    virtual ~LibRaw_file_datastream(){}
    LibRaw_file_datastream(const char *fname)
      :filename(fname)
    {
        if (filename) {
            std::auto_ptr<std::filebuf> buf(new std::filebuf());
            buf->open(filename, std::ios_base::in | std::ios_base::binary);
            if (buf->is_open()) {
                f = buf;
            }
        }
    }

    virtual int valid() { return f.get() ? 1 : 0; }

#define LR_STREAM_CHK() do {if(!f.get()) throw LIBRAW_EXCEPTION_IO_EOF;}while(0)

/* Visual Studio 2008 marks sgetn as insecure, but VS2010 does not. */
#if defined(WIN32SECURECALLS) && (_MSC_VER < 1600)
	virtual int read(void * ptr,size_t size, size_t nmemb){
            if(substream) return substream->read(ptr,size,nmemb);
            LR_STREAM_CHK(); return int(f->_Sgetn_s(static_cast<char*>(ptr), nmemb * size,nmemb * size) / size); 
        }
#else
    virtual int read(void * ptr,size_t size, size_t nmemb){
        if(substream) return substream->read(ptr,size,nmemb);
        LR_STREAM_CHK(); return int(f->sgetn(static_cast<char*>(ptr), std::streamsize(nmemb * size)) / size); 
    }
#endif

    virtual int eof() { 
        if(substream) return substream->eof();
        LR_STREAM_CHK(); return f->sgetc() == EOF; 
    }

    virtual int seek(INT64 o, int whence) 
    { 
        if(substream) return substream->seek(o,whence);
        LR_STREAM_CHK(); 
        std::ios_base::seekdir dir;
        switch (whence) 
            {
            case SEEK_SET: dir = std::ios_base::beg; break;
            case SEEK_CUR: dir = std::ios_base::cur; break;
            case SEEK_END: dir = std::ios_base::end; break;
            default: dir = std::ios_base::beg;
            }
        return (int)f->pubseekoff((long)o, dir);
    }

    virtual INT64 tell()     { 
        if(substream) return substream->tell();
        LR_STREAM_CHK(); return f->pubseekoff(0, std::ios_base::cur);  
    }

    virtual int get_char() { 
        if(substream) return substream->get_char();
        LR_STREAM_CHK();  return f->sbumpc();  
    }
    virtual char* gets(char *str, int sz) 
    { 
        if(substream) return substream->gets(str,sz);
        LR_STREAM_CHK(); 
        std::istream is(f.get());
        is.getline(str, sz);
        if (is.fail()) return 0;
        return str;
    }

    virtual int scanf_one(const char *fmt, void*val) 
    { 
        if(substream) return substream->scanf_one(fmt,val);
        LR_STREAM_CHK(); 

        std::istream is(f.get());

        /* HUGE ASSUMPTION: *fmt is either "%d" or "%f" */
        if (strcmp(fmt, "%d") == 0) {
          int d;
          is >> d;
          if (is.fail()) return EOF;
          *(static_cast<int*>(val)) = d;
        } else {
          float f;
          is >> f;
          if (is.fail()) return EOF;
          *(static_cast<float*>(val)) = f;
        }

        return 1;
    }
    virtual const char* fname() { 
        return filename; 
    }
    /* You can't have a "subfile" and a "tempfile" at the same time. */
    virtual int subfile_open(const char *fn)
    {
        LR_STREAM_CHK();
        if (saved_f.get()) return EBUSY;
        saved_f = f;
        std::auto_ptr<std::filebuf> buf(new std::filebuf());
        
        buf->open(fn, std::ios_base::in | std::ios_base::binary);
        if (!buf->is_open()) {
            f = saved_f;
            return ENOENT;
        } else {
            f = buf;
        }

        return 0;
    }
    
    virtual void subfile_close()    { if (!saved_f.get()) return; f = saved_f;   }
};
#undef LR_STREAM_CHK

class LibRaw_buffer_datastream : public LibRaw_abstract_datastream
{
  public:
    LibRaw_buffer_datastream(void *buffer, size_t bsize)
        {    buf = (unsigned char*)buffer; streampos = 0; streamsize = bsize;}

    virtual ~LibRaw_buffer_datastream(){}
    virtual int valid() { return buf?1:0;}

    virtual LibRaw_byte_buffer *make_byte_buffer(unsigned int sz)
    {
        LibRaw_byte_buffer *ret = new LibRaw_byte_buffer(0);
        if(streampos + sz > streamsize)
            sz = streamsize - streampos;
        ret->set_buffer(buf+streampos,sz);
        return ret;
    }

    virtual int read(void * ptr,size_t sz, size_t nmemb)
    { 
        if(substream) return substream->read(ptr,sz,nmemb);
        size_t to_read = sz*nmemb;
        if(to_read > streamsize - streampos)
            to_read = streamsize-streampos;
        if(to_read<1) 
            return 0;
        memmove(ptr,buf+streampos,to_read);
        streampos+=to_read;
        return int((to_read+sz-1)/sz);
    }


    virtual int eof()
    { 
        if(substream) return substream->eof();
        return streampos >= streamsize;
    }
    
    virtual int seek(INT64 o, int whence)
    { 
        if(substream) return substream->seek(o,whence);
        switch(whence)
            {
            case SEEK_SET:
                if(o<0)
                    streampos = 0;
                else if (size_t(o) > streamsize)
                    streampos = streamsize;
                else
                    streampos = size_t(o);
                return 0;
            case SEEK_CUR:
                if(o<0)
                    {
                        if(size_t(-o) >= streampos)
                            streampos = 0;
                        else
                            streampos += (size_t)o;
                    }
                else if (o>0)
                    {
                        if(o+streampos> streamsize)
                            streampos = streamsize;
                        else
                            streampos += (size_t)o;
                    }
                return 0;
            case SEEK_END:
            if(o>0)
                streampos = streamsize;
            else if ( size_t(-o) > streamsize)
                streampos = 0;
            else
                streampos = streamsize+(size_t)o;
            return 0;
            default:
                return 0;
            }
    }

    virtual INT64 tell()
    { 
        if(substream) return substream->tell();
        return INT64(streampos);
    }
    virtual int get_char()
    { 
        if(substream) return substream->get_char();
        if(streampos>=streamsize)
            return -1;
        return buf[streampos++];
    }
    virtual char* gets(char *s, int sz)
    { 
        if (substream) return substream->gets(s,sz);
        unsigned char *psrc,*pdest,*str;
        str = (unsigned char *)s;
        psrc = buf+streampos;
        pdest = str;
        while ( (size_t(psrc - buf) < streamsize)
                &&
                ((pdest-str)<sz)
            )
            {
                *pdest = *psrc;
                if(*psrc == '\n')
                    break;
                psrc++;
                pdest++;
            }
        if(size_t(psrc-buf) < streamsize)
            psrc++;
        if((pdest-str)<sz)
            *(++pdest)=0;
        streampos = psrc - buf;
        return s;
    }
    
    virtual int scanf_one(const char *fmt, void* val)
    { 
        if(substream) return substream->scanf_one(fmt,val);
        int scanf_res;
        if(streampos>streamsize) return 0;
#ifndef WIN32SECURECALLS
        scanf_res = sscanf((char*)(buf+streampos),fmt,val);
#else
        scanf_res = sscanf_s((char*)(buf+streampos),fmt,val);
#endif
        if(scanf_res>0)
            {
                int xcnt=0;
                while(streampos<streamsize)
                    {
                        streampos++;
                        xcnt++;
                        if(buf[streampos] == 0
                           || buf[streampos]==' '
                           || buf[streampos]=='\t'
                           || buf[streampos]=='\n'
                           || xcnt>24)
                            break;
                    }
            }
        return scanf_res;
    }

  private:
    unsigned char *buf;
    size_t   streampos,streamsize;
};


inline int LibRaw_abstract_datastream::tempbuffer_open(void  *buf, size_t size)
{
    if(substream) return EBUSY;
    substream = new LibRaw_buffer_datastream(buf,size);
    return substream?0:EINVAL;
}

#define LR_BF_CHK() do {if(!f) throw LIBRAW_EXCEPTION_IO_EOF;}while(0)


class LibRaw_bigfile_datastream : public LibRaw_abstract_datastream
{
  public:
    LibRaw_bigfile_datastream(const char *fname)
        { 
            if(fname)
                {
                    filename = fname; 
#ifndef WIN32SECURECALLS
                    f = fopen(fname,"rb");
#else
                    if(fopen_s(&f,fname,"rb"))
                        f = 0;
#endif
                }
            else 
                {filename=0;f=0;}
            sav=0;
        }

    virtual             ~LibRaw_bigfile_datastream() {if(f)fclose(f); if(sav)fclose(sav);}
    virtual int         valid() { return f?1:0;}

    virtual int         read(void * ptr,size_t size, size_t nmemb) 
    { 
        LR_BF_CHK(); 
        return substream?substream->read(ptr,size,nmemb):int(fread(ptr,size,nmemb,f));
    }
    virtual int         eof()
    { 
        LR_BF_CHK(); 
        return substream?substream->eof():feof(f);
    }
    virtual int         seek(INT64 o, int whence)
    { 
        LR_BF_CHK(); 
#if defined (WIN32) 
#ifdef WIN32SECURECALLS
        return substream?substream->seek(o,whence):_fseeki64(f,o,whence);
#else
        return substream?substream->seek(o,whence):fseek(f,(long)o,whence);
#endif
#else
        return substream?substream->seek(o,whence):fseeko(f,o,whence);
#endif
    }

    virtual INT64       tell()
    { 
        LR_BF_CHK(); 
#if defined (WIN32)
#ifdef WIN32SECURECALLS
        return substream?substream->tell():_ftelli64(f);
#else
        return substream?substream->tell():ftell(f);
#endif
#else
        return substream?substream->tell():ftello(f);
#endif
    }

    virtual int         get_char()
    { 
        LR_BF_CHK(); 
#ifndef WIN32
        return substream?substream->get_char():getc_unlocked(f);
#else
        return substream?substream->get_char():fgetc(f);
#endif
    }
        
    virtual char* gets(char *str, int sz)
    { 
        LR_BF_CHK(); 
        return substream?substream->gets(str,sz):fgets(str,sz,f);
    }

    virtual int scanf_one(const char *fmt, void*val)
    { 
        LR_BF_CHK(); 
        return substream?substream->scanf_one(fmt,val):
#ifndef WIN32SECURECALLS			
            fscanf(f,fmt,val)
#else
            fscanf_s(f,fmt,val)
#endif
            ;
    }
    virtual const char *fname() { return filename; }
    virtual int subfile_open(const char *fn)
    {
        if(sav) return EBUSY;
        sav = f;
#ifndef WIN32SECURECALLS
        f = fopen(fn,"rb");
#else
        fopen_s(&f,fn,"rb");
#endif
        if(!f)
            {
                f = sav;
                sav = NULL;
                return ENOENT;
            }
        else
            return 0;
    }
    virtual void subfile_close()
    {
        if(!sav) return;
        fclose(f);
        f = sav;
        sav = 0;
    }

  private:
    FILE *f,*sav;
    const char *filename;
};
#undef LR_BF_CHK

#endif

#endif

