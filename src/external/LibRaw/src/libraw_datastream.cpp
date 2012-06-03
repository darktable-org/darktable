#ifdef WIN32
#ifdef __MINGW32__
    #define _WIN32_WINNT 0x0500
    #include <stdexcept>
#endif
#endif

#define LIBRAW_LIBRARY_BUILD
#include "libraw/libraw_types.h"
#include "libraw/libraw.h"
#include "libraw/libraw_datastream.h"
#include "internal/libraw_bytebuffer.h"
#ifdef USE_JASPER
#include <jasper/jasper.h>	/* Decode RED camera movies */
#else
#define NO_JASPER
#endif


LibRaw_byte_buffer::LibRaw_byte_buffer(unsigned sz) 
{ 
    buf=0; size=sz; offt=0; do_free=0; 
    if(size)
        { 
            buf = (unsigned char*)malloc(size); do_free=1;
        }
}

void LibRaw_byte_buffer::set_buffer(void *bb, unsigned int sz) 
{ 
    buf = (unsigned char*)bb; size = sz; offt=0; do_free=0;
}

LibRaw_byte_buffer::~LibRaw_byte_buffer() 
{ 
    if(do_free) free(buf);
}

LibRaw_byte_buffer *LibRaw_abstract_datastream::make_byte_buffer(unsigned int sz)
{
    LibRaw_byte_buffer *ret = new LibRaw_byte_buffer(sz);
    read(ret->get_buffer(),sz,1);
    return ret;
}

int LibRaw_abstract_datastream::tempbuffer_open(void  *buf, size_t size)
{
    if(substream) return EBUSY;
    substream = new LibRaw_buffer_datastream(buf,size);
    return substream?0:EINVAL;
}


void	LibRaw_abstract_datastream::tempbuffer_close()
{
    if(substream) delete substream;
    substream = NULL;
}

// == LibRaw_file_datastream ==

LibRaw_file_datastream::LibRaw_file_datastream(const char *fname)
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
 int LibRaw_file_datastream::valid()
{ 
    return f.get() ? 1 : 0; 
}

#define LR_STREAM_CHK() do {if(!f.get()) throw LIBRAW_EXCEPTION_IO_EOF;}while(0)

int LibRaw_file_datastream::read(void * ptr,size_t size, size_t nmemb)
{
    if(substream) return substream->read(ptr,size,nmemb);
    
/* Visual Studio 2008 marks sgetn as insecure, but VS2010 does not. */
#if defined(WIN32SECURECALLS) && (_MSC_VER < 1600)
    LR_STREAM_CHK(); return int(f->_Sgetn_s(static_cast<char*>(ptr), nmemb * size,nmemb * size) / size); 
#else
    LR_STREAM_CHK(); return int(f->sgetn(static_cast<char*>(ptr), std::streamsize(nmemb * size)) / size); 
#endif
}

int LibRaw_file_datastream::eof() 
{ 
    if(substream) return substream->eof();
    LR_STREAM_CHK(); return f->sgetc() == EOF; 
}

int LibRaw_file_datastream::seek(INT64 o, int whence) 
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

INT64 LibRaw_file_datastream::tell()     
{ 
    if(substream) return substream->tell();
    LR_STREAM_CHK(); return f->pubseekoff(0, std::ios_base::cur);  
}

char* LibRaw_file_datastream::gets(char *str, int sz) 
{ 
    if(substream) return substream->gets(str,sz);
    LR_STREAM_CHK(); 
    std::istream is(f.get());
    is.getline(str, sz);
    if (is.fail()) return 0;
    return str;
}

int LibRaw_file_datastream::scanf_one(const char *fmt, void*val) 
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

const char* LibRaw_file_datastream::fname() 
{ 
    return filename; 
}
    
/* You can't have a "subfile" and a "tempfile" at the same time. */
int LibRaw_file_datastream::subfile_open(const char *fn)
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


void LibRaw_file_datastream::subfile_close()
{ 
    if (!saved_f.get()) return; 
    f = saved_f;   
}

#undef LR_STREAM_CHK

void * LibRaw_file_datastream::make_jas_stream()
{
#ifdef NO_JASPER
    return NULL;
#else
    return jas_stream_fopen(fname(),"rb");
#endif
}

// == LibRaw_buffer_datastream
LibRaw_buffer_datastream::LibRaw_buffer_datastream(void *buffer, size_t bsize)
{    
    buf = (unsigned char*)buffer; streampos = 0; streamsize = bsize;
}

LibRaw_buffer_datastream::~LibRaw_buffer_datastream(){}

int LibRaw_buffer_datastream::read(void * ptr,size_t sz, size_t nmemb)
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

int LibRaw_buffer_datastream::seek(INT64 o, int whence)
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

INT64 LibRaw_buffer_datastream::tell()
{ 
    if(substream) return substream->tell();
    return INT64(streampos);
}

char* LibRaw_buffer_datastream::gets(char *s, int sz)
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

int LibRaw_buffer_datastream::scanf_one(const char *fmt, void* val)
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

LibRaw_byte_buffer *LibRaw_buffer_datastream::make_byte_buffer(unsigned int sz)
{
    LibRaw_byte_buffer *ret = new LibRaw_byte_buffer(0);
    if(streampos + sz > streamsize)
        sz = streamsize - streampos;
    ret->set_buffer(buf+streampos,sz);
    return ret;
}

int LibRaw_buffer_datastream::eof()
{ 
    if(substream) return substream->eof();
    return streampos >= streamsize;
}
 int LibRaw_buffer_datastream::valid() 
{ 
    return buf?1:0;
}


void * LibRaw_buffer_datastream::make_jas_stream()
{
#ifdef NO_JASPER
    return NULL;
#else
    return jas_stream_memopen((char*)buf,streamsize);
#endif
}

// == LibRaw_bigfile_datastream
LibRaw_bigfile_datastream::LibRaw_bigfile_datastream(const char *fname)
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

LibRaw_bigfile_datastream::~LibRaw_bigfile_datastream() {if(f)fclose(f); if(sav)fclose(sav);}
int         LibRaw_bigfile_datastream::valid() { return f?1:0;}

#define LR_BF_CHK() do {if(!f) throw LIBRAW_EXCEPTION_IO_EOF;}while(0)

int LibRaw_bigfile_datastream::read(void * ptr,size_t size, size_t nmemb) 
{ 
    LR_BF_CHK(); 
    return substream?substream->read(ptr,size,nmemb):int(fread(ptr,size,nmemb,f));
}

int LibRaw_bigfile_datastream::eof()
{ 
    LR_BF_CHK(); 
    return substream?substream->eof():feof(f);
}

int     LibRaw_bigfile_datastream:: seek(INT64 o, int whence)
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

INT64 LibRaw_bigfile_datastream::tell()
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

char* LibRaw_bigfile_datastream::gets(char *str, int sz)
{ 
    LR_BF_CHK(); 
    return substream?substream->gets(str,sz):fgets(str,sz,f);
}

int LibRaw_bigfile_datastream::scanf_one(const char *fmt, void*val)
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

const char *LibRaw_bigfile_datastream::fname() 
{ 
    return filename; 
}

int LibRaw_bigfile_datastream::subfile_open(const char *fn)
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

void LibRaw_bigfile_datastream::subfile_close()
{
    if(!sav) return;
    fclose(f);
    f = sav;
    sav = 0;
}


void *LibRaw_bigfile_datastream::make_jas_stream()
{
#ifdef NO_JASPER
    return NULL;
#else
    return jas_stream_freopen(fname(),"rb",f);
#endif
}

// == LibRaw_windows_datastream
#ifdef WIN32

LibRaw_windows_datastream::LibRaw_windows_datastream(const TCHAR* sFile)
    : LibRaw_buffer_datastream(NULL, 0)
    , hMap_(0)
    , pView_(NULL)
{
    HANDLE hFile = CreateFile(sFile, GENERIC_READ, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (hFile == INVALID_HANDLE_VALUE) 
        throw std::runtime_error("failed to open the file"); 
    
    try { Open(hFile); 	}	catch(...) { CloseHandle(hFile); throw; }
    
    CloseHandle(hFile);		// windows will defer the actual closing of this handle until the hMap_ is closed
    reconstruct_base();
}

	// ctor: construct with a file handle - caller is responsible for closing the file handle
LibRaw_windows_datastream::LibRaw_windows_datastream(HANDLE hFile)
    : LibRaw_buffer_datastream(NULL, 0)
    , hMap_(0)
    , pView_(NULL)
{
    Open(hFile);
    reconstruct_base();
}

// dtor: unmap and close the mapping handle
LibRaw_windows_datastream::~LibRaw_windows_datastream()
{
    if (pView_ != NULL)
        ::UnmapViewOfFile(pView_);
    
    if (hMap_ != 0)
        ::CloseHandle(hMap_);
}

void LibRaw_windows_datastream::Open(HANDLE hFile)
{
    // create a file mapping handle on the file handle
    hMap_ = ::CreateFileMapping(hFile, 0, PAGE_READONLY, 0, 0, 0);
    if (hMap_ == NULL)	throw std::runtime_error("failed to create file mapping"); 
    
    // now map the whole file base view
    if (!::GetFileSizeEx(hFile, (PLARGE_INTEGER)&cbView_))
        throw std::runtime_error("failed to get the file size"); 
    
    pView_ = ::MapViewOfFile(hMap_, FILE_MAP_READ, 0, 0, (size_t)cbView_);
    if (pView_ == NULL)	
        throw std::runtime_error("failed to map the file"); 
}


#endif


    
