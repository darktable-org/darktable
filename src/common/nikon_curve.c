#ifndef DT_NIKON_CURVE_C
#define DT_NIKON_CURVE_C
/***************************************************
 nikon_curve.c - read Nikon NTC/NCV files

 Copyright 2004-2008 by Shawn Freeman, Udi Fuchs

 This program reads in a Nikon NTC/NCV file,
 interperates it's tone curve, and writes out a
 simple ASCII file containing a table of interpolation
 values. See the header file for more information.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

****************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <stdarg.h> /* For variable argument lists */
#include <glib.h>
#include "nikon_curve.h"

#if 0
#ifdef __WITH_UFRAW__
    #include "uf_glib.h"
    #include "ufraw.h"
#else
    #define MAX(a,b) ((a) > (b) ? (a) : (b))
    #define MIN(a,b) ((a) < (b) ? (a) : (b))
    #define g_fopen fopen
#endif
#endif
#define g_fopen fopen

/*************************************************
 * Internal static data
 *************************************************/

//file offsets for the different data in different file types
static const int FileOffsets[2][4] = {
    {NTC_PATCH_OFFSET,NTC_BOX_DATA, NTC_NUM_ANCHOR_POINTS, NTC_ANCHOR_DATA_START},
    {NCV_PATCH_OFFSET,NCV_BOX_DATA, NCV_NUM_ANCHOR_POINTS, NCV_ANCHOR_DATA_START},
};

//file header indicating ntc file
static const unsigned char NTCFileHeader[] = {0x9d,0xdc,0x7d,0x00,0x65,0xd4,
			0x11,0xd1,0x91,0x94,0x44,0x45,0x53,0x54,0x00,0x00};

//file header indicating an ncv file
static const unsigned char NCVFileHeader[] = {0x40,0xa9,0x86,0x7a,0x1b,0xe9,
			0xd2,0x11,0xa9,0x0a,0x00,0xaa,0x00,0xb1,0xc1,0xb7};

//This is an additional header chunk at the beginning of the file
//There are some similarities between the headers, but not enough to fully crack.
//This does not appear to change.
static const unsigned char NCVSecondFileHeader[] = {0x01,0x32,0xa4,0x76,0xa2,
			0x17,0xd4,0x11,0xa9,0x0a,0x00,0xaa,0x00,0xb1,0xc1,
			0xb7,0x01,0x00,0x05,0x00,0x00,0x00,0x01};

//This is the terminator of an NCV file. Again there are some similarites
//to other sections, but not enough for to crack what it means. However,
//it does not appear to change.
static const unsigned char NCVFileTerminator[] = {0x45,0xd3,0x0d,0x77,0xa3,0x6e,
			0x1e,0x4e,0xa4,0xbe,0xcf,0xc1,0x8e,0xb5,0xb7,0x47,
			0x01,0x00,0x05,0x00,0x00,0x00,0x01 };

//File section header. Only a one byte difference between this and an NTC file header
static const unsigned char FileSectionHeader[] = {0x9d,0xdc,0x7d,0x03,0x65,0xd4,
			0x11,0xd1,0x91,0x94,0x44,0x45,0x53,0x54,0x00,0x00};
//file type header array
static const unsigned char *FileTypeHeaders[NUM_FILE_TYPES] = {
    NTCFileHeader,
    NCVFileHeader,
};

/**STANDALONE**/
#ifdef _STAND_ALONE_

//filenames
char exportFilename[1024];
char nikonFilename[1024];

unsigned int standalone_samplingRes = 65536;
unsigned int standalone_outputRes = 256;
unsigned int program_mode = CURVE_MODE;

/*******************************************
ProcessArgs:
    Convenient function for processing the args
    for the test runner.
********************************************/
int ProcessArgs(int num_args, char *args[])
{
    exportFilename[0] = '\0';
    nikonFilename[0] = '\0';

    int i;
    for(i = 0; i < num_args; i++)
    {
	if (strcmp(args[i],"-h") == 0 || strcmp(args[i],"-H") == 0 || num_args <= 1)
	{
	    printf("NikonCurveGenerator %s %s\n",NC_VERSION, NC_DATE);
	    printf("Written by Shawn Freeman\n");
	    printf("Thanks go out to Udi Fuchs, UFRaw, and GIMP :)\n\n");
	    printf("Usage:\n");
	    printf("-o     Specify output file.\n");
	    printf("-sr    Specify sampling resolution. Default is 65536.\n");
	    printf("-or    Specify output resolution. Default is 256.\n\n");
	    printf("-nef   Specify an NEF file to get tone curve data from.\n\n");
	    printf("       The -or and -sr options are ignored for NEF files\n\n");
	    printf("NOTE: If a resolution is not specified, a default one will be used.\n");
	    printf("      If the -o option is not specified, default files will be used.\n\n");
	    printf("Example:\n");
	    printf("NikonCurveGenerator -sr 65536 -or 256 curveFile -o exportFile\n");

	    //signal that processing cannot occur
	    return NC_ERROR;
	}
	else if (strcmp(args[i],"-o") == 0 || strcmp(args[i],"-O") == 0)
	{
	    i++;
	    strncpy(exportFilename, args[i], 1023);
	    exportFilename[1023] = '\0';
	}
	else if (strcmp(args[i],"-sr") == 0)
	{
	    i++;
	    standalone_samplingRes = atoi(args[i]);

	    if (standalone_samplingRes < 1)
	    {
	        nc_message(NC_WARNING, "WARNING: Sampling resolution must be"
				">= 1! Using default of 65535.\n");
	    }
	}
	else if (strcmp(args[i],"-or") == 0)
	{
	    i++;
	    standalone_outputRes = atoi(args[i]);

	    if (standalone_outputRes < 1)
	    {
	        nc_message(NC_WARNING, "WARNING: Output resolution must be"
				">= 1! Using default of 256.\n");
	    }
	}
	else if (strcmp(args[i],"-nef") == 0)
	{
	    i++;
	    program_mode = NEF_MODE;
	    strncpy(nikonFilename, args[i], 1023);
	    nikonFilename[1023] = '\0';
	}
	//don't load argument 0
	else if (i != 0)
	{
	    //consider this the file name to load
	    strncpy(nikonFilename, args[i], 1023);
	    nikonFilename[1023] = '\0';
	}
    }

    if (strlen(exportFilename) == 0)
    {
	//set it to have a default output file name
	strncpy(exportFilename, nikonFilename, 1023);
	strncat(exportFilename, "_CURVE_OUTPUT.txt", 1023);
	exportFilename[1023] = '\0';
    }

    return NC_SUCCESS;
}
#endif //End STAND_ALONE

/************************************************************
nc_message_handler:
    The Nikon Curve message handler. Udi Fuchs created this
to make the error handling consistent acros the code.

  code - Message code
  message - The message
**************************************************************/
void nc_message(int code, char *format, ...)
{
    char message[256];
    va_list ap;
    va_start(ap, format);

    vsnprintf(message, 255, format, ap);
    message[255] = '\0';
    va_end(ap);

#ifdef _STAND_ALONE_    //if we're running standalone mode

    code = code;
    fprintf(stderr, "%s", message);
    fflush(stderr);

#else

#ifdef __WITH_UFRAW__    //and if compiling with UFRAW

    if (code==NC_SET_ERROR)
    {
	ufraw_message(UFRAW_SET_ERROR, message);
    }
    else
    {
	ufraw_message(code, message);
    }

#else    //else, just print out the errors normally

    code = code;
    g_printerr("%s", message);

#endif //End WITH_UFRAW

#endif //End STAND_ALONE
}

void DEBUG_PRINT(char *format, ...)
{
#if 0//def _DEBUG
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    fflush(stderr);
    va_end(ap);
#else
    format = format;
#endif
}

/* nc_merror(): Handle memory allocaltion errors */
void nc_merror(void *ptr, char *where)
{
    if (ptr) return;
#ifdef __WITH_UFRAW__
    g_error("Out of memory in %s\n", where);
#else
    fprintf(stderr, "Out of memory in %s\n", where);
    exit(1);
#endif
}

size_t nc_fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    size_t num = fread(ptr, size, nmemb, stream);
    if ( num!=nmemb )
	nc_message(NC_WARNING, "WARNING: fread %d != %d\n", num, nmemb);
    return num;
}

size_t nc_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    size_t num = nc_fwrite(ptr, size, nmemb, stream);
    if ( num!=nmemb )
	nc_message(NC_WARNING, "WARNING: nc_fwrite %d != %d\n", num, nmemb);
    return num;
}

// Assert something at compile time (must use this inside a function);
// works because compilers won't let us declare negative-length arrays.
#define STATIC_ASSERT(cond) \
    { (void)((int (*)(char failed_static_assertion[(cond)?1:-1]))0); }

/***********************************************************************
isBigEndian:
	Determines if the machine we are running on is big endian or not.
************************************************************************/
int isBigEndian()
{
    STATIC_ASSERT(sizeof(short)==2);
    short x;
    unsigned char EndianTest[2] = { 1, 0 };

    x = *(short *) EndianTest;

    return (x!=1);
}

/***********************************************************************
ShortVal:
	Convert short int (16 bit) from little endian to machine endianess.
************************************************************************/
short ShortVal(short s)
{
    STATIC_ASSERT(sizeof(short)==2);
    if (isBigEndian()) {
	unsigned char b1, b2;

	b1 = s & 255;
	b2 = (s >> 8) & 255;

	return (b1 << 8) + b2;
    } else
	return s;
}

/***********************************************************************
LongVal:
	Convert long int (32 bit) from little endian to machine endianess.
************************************************************************/
int LongVal(int i)
{
    STATIC_ASSERT(sizeof(int)==4);
    if (isBigEndian()) {
	unsigned char b1, b2, b3, b4;

	b1 = i & 255;
	b2 = ( i >> 8 ) & 255;
	b3 = ( i>>16 ) & 255;
	b4 = ( i>>24 ) & 255;

	return ((int)b1 << 24) + ((int)b2 << 16) + ((int)b3 << 8) + b4;
    } else
	return i;
}

/***********************************************************************
FloatVal:
	Convert float from little endian to machine endianess.
************************************************************************/
float FloatVal(float f)
{
    STATIC_ASSERT(sizeof(float)==4);
    if (isBigEndian()) {
	union {
	    float f;
	    unsigned char b[4];
	} dat1, dat2;

	dat1.f = f;
	dat2.b[0] = dat1.b[3];
	dat2.b[1] = dat1.b[2];
	dat2.b[2] = dat1.b[1];
	dat2.b[3] = dat1.b[0];
	return dat2.f;
    } else
	return f;
}

/***********************************************************************
DoubleVal:
	Convert double from little endian to machine endianess.
************************************************************************/
double DoubleVal(double d)
{
    STATIC_ASSERT(sizeof(double)==8);
    if (isBigEndian()) {
	union {
	    double d;
	    unsigned char b[8];
	} dat1, dat2;

	dat1.d = d;
	dat2.b[0] = dat1.b[7];
	dat2.b[1] = dat1.b[6];
	dat2.b[2] = dat1.b[5];
	dat2.b[3] = dat1.b[4];
	dat2.b[4] = dat1.b[3];
	dat2.b[5] = dat1.b[2];
	dat2.b[6] = dat1.b[1];
	dat2.b[7] = dat1.b[0];
	return dat2.d;
    } else
	return d;
}

//**********************************************************************
//
//  Purpose:
//
//    D3_NP_FS factors and solves a D3 system.
//
//  Discussion:
//
//    The D3 storage format is used for a tridiagonal matrix.
//    The superdiagonal is stored in entries (1,2:N), the diagonal in
//    entries (2,1:N), and the subdiagonal in (3,1:N-1).  Thus, the
//    original matrix is "collapsed" vertically into the array.
//
//    This algorithm requires that each diagonal entry be nonzero.
//    It does not use pivoting, and so can fail on systems that
//    are actually nonsingular.
//
//  Example:
//
//    Here is how a D3 matrix of order 5 would be stored:
//
//       *  A12 A23 A34 A45
//      A11 A22 A33 A44 A55
//      A21 A32 A43 A54  *
//
//  Modified:
//
//      07 January 2005    Shawn Freeman (pure C modifications)
//    15 November 2003    John Burkardt
//
//  Author:
//
//    John Burkardt
//
//  Parameters:
//
//    Input, int N, the order of the linear system.
//
//    Input/output, double A[3*N].
//    On input, the nonzero diagonals of the linear system.
//    On output, the data in these vectors has been overwritten
//    by factorization information.
//
//    Input, double B[N], the right hand side.
//
//    Output, double D3_NP_FS[N], the solution of the linear system.
//    This is NULL if there was an error because one of the diagonal
//    entries was zero.
//
double *d3_np_fs ( int n, double a[], double b[] )

{
  int i;
  double *x;
  double xmult;
//
//  Check.
//
  for ( i = 0; i < n; i++ )
  {
    if ( a[1+i*3] == 0.0E+00 )
    {
      return NULL;
    }
  }
  x = (double *)calloc(n,sizeof(double));
  nc_merror(x, "d3_np_fs");

  for ( i = 0; i < n; i++ )
  {
    x[i] = b[i];
  }

  for ( i = 1; i < n; i++ )
  {
    xmult = a[2+(i-1)*3] / a[1+(i-1)*3];
    a[1+i*3] = a[1+i*3] - xmult * a[0+i*3];
    x[i] = x[i] - xmult * x[i-1];
  }

  x[n-1] = x[n-1] / a[1+(n-1)*3];
  for ( i = n-2; 0 <= i; i-- )
  {
    x[i] = ( x[i] - a[0+(i+1)*3] * x[i+1] ) / a[1+i*3];
  }

  return x;
}

//**********************************************************************
//
//  Purpose:
//
//    SPLINE_CUBIC_SET computes the second derivatives of a piecewise cubic spline.
//
//  Discussion:
//
//    For data interpolation, the user must call SPLINE_SET to determine
//    the second derivative data, passing in the data to be interpolated,
//    and the desired boundary conditions.
//
//    The data to be interpolated, plus the SPLINE_SET output, defines
//    the spline.  The user may then call SPLINE_VAL to evaluate the
//    spline at any point.
//
//    The cubic spline is a piecewise cubic polynomial.  The intervals
//    are determined by the "knots" or abscissas of the data to be
//    interpolated.  The cubic spline has continous first and second
//    derivatives over the entire interval of interpolation.
//
//    For any point T in the interval T(IVAL), T(IVAL+1), the form of
//    the spline is
//
//      SPL(T) = A(IVAL)
//             + B(IVAL) * ( T - T(IVAL) )
//             + C(IVAL) * ( T - T(IVAL) )**2
//             + D(IVAL) * ( T - T(IVAL) )**3
//
//    If we assume that we know the values Y(*) and YPP(*), which represent
//    the values and second derivatives of the spline at each knot, then
//    the coefficients can be computed as:
//
//      A(IVAL) = Y(IVAL)
//      B(IVAL) = ( Y(IVAL+1) - Y(IVAL) ) / ( T(IVAL+1) - T(IVAL) )
//        - ( YPP(IVAL+1) + 2 * YPP(IVAL) ) * ( T(IVAL+1) - T(IVAL) ) / 6
//      C(IVAL) = YPP(IVAL) / 2
//      D(IVAL) = ( YPP(IVAL+1) - YPP(IVAL) ) / ( 6 * ( T(IVAL+1) - T(IVAL) ) )
//
//    Since the first derivative of the spline is
//
//      SPL'(T) =     B(IVAL)
//              + 2 * C(IVAL) * ( T - T(IVAL) )
//              + 3 * D(IVAL) * ( T - T(IVAL) )**2,
//
//    the requirement that the first derivative be continuous at interior
//    knot I results in a total of N-2 equations, of the form:
//
//      B(IVAL-1) + 2 C(IVAL-1) * (T(IVAL)-T(IVAL-1))
//      + 3 * D(IVAL-1) * (T(IVAL) - T(IVAL-1))**2 = B(IVAL)
//
//    or, setting H(IVAL) = T(IVAL+1) - T(IVAL)
//
//      ( Y(IVAL) - Y(IVAL-1) ) / H(IVAL-1)
//      - ( YPP(IVAL) + 2 * YPP(IVAL-1) ) * H(IVAL-1) / 6
//      + YPP(IVAL-1) * H(IVAL-1)
//      + ( YPP(IVAL) - YPP(IVAL-1) ) * H(IVAL-1) / 2
//      =
//      ( Y(IVAL+1) - Y(IVAL) ) / H(IVAL)
//      - ( YPP(IVAL+1) + 2 * YPP(IVAL) ) * H(IVAL) / 6
//
//    or
//
//      YPP(IVAL-1) * H(IVAL-1) + 2 * YPP(IVAL) * ( H(IVAL-1) + H(IVAL) )
//      + YPP(IVAL) * H(IVAL)
//      =
//      6 * ( Y(IVAL+1) - Y(IVAL) ) / H(IVAL)
//      - 6 * ( Y(IVAL) - Y(IVAL-1) ) / H(IVAL-1)
//
//    Boundary conditions must be applied at the first and last knots.
//    The resulting tridiagonal system can be solved for the YPP values.
//
//  Modified:
//
//      07 January 2005    Shawn Freeman (pure C modifications)
//    06 February 2004    John Burkardt
//
//
//  Author:
//
//    John Burkardt
//
//  Parameters:
//
//    Input, int N, the number of data points.  N must be at least 2.
//    In the special case where N = 2 and IBCBEG = IBCEND = 0, the
//    spline will actually be linear.
//
//    Input, double T[N], the knot values, that is, the points were data is
//    specified.  The knot values should be distinct, and increasing.
//
//    Input, double Y[N], the data values to be interpolated.
//
//    Input, int IBCBEG, left boundary condition flag:
//      0: the cubic spline should be a quadratic over the first interval;
//      1: the first derivative at the left endpoint should be YBCBEG;
//      2: the second derivative at the left endpoint should be YBCBEG.
//
//    Input, double YBCBEG, the values to be used in the boundary
//    conditions if IBCBEG is equal to 1 or 2.
//
//    Input, int IBCEND, right boundary condition flag:
//      0: the cubic spline should be a quadratic over the last interval;
//      1: the first derivative at the right endpoint should be YBCEND;
//      2: the second derivative at the right endpoint should be YBCEND.
//
//    Input, double YBCEND, the values to be used in the boundary
//    conditions if IBCEND is equal to 1 or 2.
//
//    Output, double SPLINE_CUBIC_SET[N], the second derivatives of the cubic spline.
//
double *spline_cubic_set ( int n, double t[], double y[], int ibcbeg,
    double ybcbeg, int ibcend, double ybcend )
{
  double *a;
  double *b;
  int i;
  double *ypp;
//
//  Check.
//
  if ( n <= 1 )
  {
    nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
	    "The number of data points must be at least 2.\n");
    return NULL;
  }

  for ( i = 0; i < n - 1; i++ )
  {
    if ( t[i+1] <= t[i] )
    {
      nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
	      "The knots must be strictly increasing, but "
	      "T(%u) = %e, T(%u) = %e\n",i,t[i],i+1,t[i+1]);
      return NULL;
    }
  }
  a = (double *)calloc(3*n,sizeof(double));
  nc_merror(a, "spline_cubic_set");
  b = (double *)calloc(n,sizeof(double));
  nc_merror(b, "spline_cubic_set");
//
//  Set up the first equation.
//
  if ( ibcbeg == 0 )
  {
    b[0] = 0.0E+00;
    a[1+0*3] = 1.0E+00;
    a[0+1*3] = -1.0E+00;
  }
  else if ( ibcbeg == 1 )
  {
    b[0] = ( y[1] - y[0] ) / ( t[1] - t[0] ) - ybcbeg;
    a[1+0*3] = ( t[1] - t[0] ) / 3.0E+00;
    a[0+1*3] = ( t[1] - t[0] ) / 6.0E+00;
  }
  else if ( ibcbeg == 2 )
  {
    b[0] = ybcbeg;
    a[1+0*3] = 1.0E+00;
    a[0+1*3] = 0.0E+00;
  }
  else
  {
    nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
	    "IBCBEG must be 0, 1 or 2. The input value is %u.\n", ibcbeg);
    free(a);
    free(b);
    return NULL;
  }
//
//  Set up the intermediate equations.
//
  for ( i = 1; i < n-1; i++ )
  {
    b[i] = ( y[i+1] - y[i] ) / ( t[i+1] - t[i] )
      - ( y[i] - y[i-1] ) / ( t[i] - t[i-1] );
    a[2+(i-1)*3] = ( t[i] - t[i-1] ) / 6.0E+00;
    a[1+ i   *3] = ( t[i+1] - t[i-1] ) / 3.0E+00;
    a[0+(i+1)*3] = ( t[i+1] - t[i] ) / 6.0E+00;
  }
//
//  Set up the last equation.
//
  if ( ibcend == 0 )
  {
    b[n-1] = 0.0E+00;
    a[2+(n-2)*3] = -1.0E+00;
    a[1+(n-1)*3] = 1.0E+00;
  }
  else if ( ibcend == 1 )
  {
    b[n-1] = ybcend - ( y[n-1] - y[n-2] ) / ( t[n-1] - t[n-2] );
    a[2+(n-2)*3] = ( t[n-1] - t[n-2] ) / 6.0E+00;
    a[1+(n-1)*3] = ( t[n-1] - t[n-2] ) / 3.0E+00;
  }
  else if ( ibcend == 2 )
  {
    b[n-1] = ybcend;
    a[2+(n-2)*3] = 0.0E+00;
    a[1+(n-1)*3] = 1.0E+00;
  }
  else
  {
    nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
	    "IBCEND must be 0, 1 or 2. The input value is %u", ibcend);
    free(a);
    free(b);
    return NULL;
  }
//
//  Solve the linear system.
//
  if ( n == 2 && ibcbeg == 0 && ibcend == 0 )
  {
    ypp = (double *)calloc(2,sizeof(double));
    nc_merror(ypp, "spline_cubic_set");

    ypp[0] = 0.0E+00;
    ypp[1] = 0.0E+00;
  }
  else
  {
    ypp = d3_np_fs ( n, a, b );

    if ( !ypp )
    {
      nc_message(NC_SET_ERROR, "spline_cubic_set() error: "
	      "The linear system could not be solved.\n");
      free(a);
      free(b);
      return NULL;
    }

  }

  free(a);
  free(b);
  return ypp;
}

//**********************************************************************
//
//  Purpose:
//
//    SPLINE_CUBIC_VAL evaluates a piecewise cubic spline at a point.
//
//  Discussion:
//
//    SPLINE_CUBIC_SET must have already been called to define the values of YPP.
//
//    For any point T in the interval T(IVAL), T(IVAL+1), the form of
//    the spline is
//
//      SPL(T) = A
//             + B * ( T - T(IVAL) )
//             + C * ( T - T(IVAL) )**2
//             + D * ( T - T(IVAL) )**3
//
//    Here:
//      A = Y(IVAL)
//      B = ( Y(IVAL+1) - Y(IVAL) ) / ( T(IVAL+1) - T(IVAL) )
//        - ( YPP(IVAL+1) + 2 * YPP(IVAL) ) * ( T(IVAL+1) - T(IVAL) ) / 6
//      C = YPP(IVAL) / 2
//      D = ( YPP(IVAL+1) - YPP(IVAL) ) / ( 6 * ( T(IVAL+1) - T(IVAL) ) )
//
//  Modified:
//
//      07 January 2005    Shawn Freeman (pure C modifications)
//    04 February 1999    John Burkardt
//
//  Author:
//
//    John Burkardt
//
//  Parameters:
//
//    Input, int n, the number of knots.
//
//    Input, double Y[N], the data values at the knots.
//
//    Input, double T[N], the knot values.
//
//    Input, double TVAL, a point, typically between T[0] and T[N-1], at
//    which the spline is to be evalulated.  If TVAL lies outside
//    this range, extrapolation is used.
//
//    Input, double Y[N], the data values at the knots.
//
//    Input, double YPP[N], the second derivatives of the spline at
//    the knots.
//
//    Output, double *YPVAL, the derivative of the spline at TVAL.
//
//    Output, double *YPPVAL, the second derivative of the spline at TVAL.
//
//    Output, double SPLINE_VAL, the value of the spline at TVAL.
//
double spline_cubic_val ( int n, double t[], double tval, double y[],
	double ypp[], double *ypval, double *yppval )
{
  double dt;
  double h;
  int i;
  int ival;
  double yval;
//
//  Determine the interval [ T(I), T(I+1) ] that contains TVAL.
//  Values below T[0] or above T[N-1] use extrapolation.
//
  ival = n - 2;

  for ( i = 0; i < n-1; i++ )
  {
    if ( tval < t[i+1] )
    {
      ival = i;
      break;
    }
  }
//
//  In the interval I, the polynomial is in terms of a normalized
//  coordinate between 0 and 1.
//
  dt = tval - t[ival];
  h = t[ival+1] - t[ival];

  yval = y[ival]
    + dt * ( ( y[ival+1] - y[ival] ) / h
	   - ( ypp[ival+1] / 6.0E+00 + ypp[ival] / 3.0E+00 ) * h
    + dt * ( 0.5E+00 * ypp[ival]
    + dt * ( ( ypp[ival+1] - ypp[ival] ) / ( 6.0E+00 * h ) ) ) );

  *ypval = ( y[ival+1] - y[ival] ) / h
    - ( ypp[ival+1] / 6.0E+00 + ypp[ival] / 3.0E+00 ) * h
    + dt * ( ypp[ival]
    + dt * ( 0.5E+00 * ( ypp[ival+1] - ypp[ival] ) / h ) );

  *yppval = ypp[ival] + dt * ( ypp[ival+1] - ypp[ival] ) / h;

  return yval;
}

/*********************************************
GetNikonFileType:
    Gets the nikon file type by comparing file
    headers.

  file - The file to check.
**********************************************/
int GetNikonFileType(FILE *file)
{
    unsigned char buff[HEADER_SIZE];
    int i = 0, j = 0;
    int found = 1;

    nc_fread(buff,HEADER_SIZE,1,file);

    for(i = 0; i < NUM_FILE_TYPES; i++)
    {
	found = 1;
	for(j = 0; j < HEADER_SIZE; j++)
	{
	    if (buff[j] != FileTypeHeaders[i][j])
	    {
	        found = 0;
	        break;
	    }
	}

	if (found)
	{
	    //return the file type
	    return i;
	}
    }
	nc_message(NC_SET_ERROR, "Error, no compatible file types found!\n");
    return -1;
}

/*********************************************
LoadNikonCurve:
    Loads all curves from a Nikon ntc or ncv file.

    fileName    - The filename.
    curve        - Pointer to curve struct to hold the data.
    resolution    - How many data points to sample from the curve.
**********************************************/
int LoadNikonData(char *fileName, NikonData *data)
{
    FILE *input = NULL;
    int offset = 0;
    CurveData *curve = NULL;

    if (fileName == NULL || strlen(fileName) == 0)
    {
	nc_message(NC_SET_ERROR,
	            "Error, input filename cannot be NULL or empty!\n");
	return NC_ERROR;
    }

    DEBUG_PRINT("DEBUG: OPENING FILE: %s\n",fileName);

    //open file for reading only!
    input = g_fopen(fileName,"rb");

    //make sure we have a valid file
    if (input == NULL)
    {
	nc_message(NC_SET_ERROR, "Error opening '%s': %s\n",
	        fileName, strerror(errno));
	return NC_ERROR;
    }

    //init the curve;
    memset(data,0,sizeof(NikonData));

    //get the file type
    data->m_fileType = GetNikonFileType(input);
    // set file seek positions for curve tones depending of file type
    // todo: is it possible to find one common rule?
    long curveFilePos[4][4] = {
	{FileOffsets[data->m_fileType][BOX_DATA], SEEK_SET, FileOffsets[data->m_fileType][ANCHOR_DATA], SEEK_SET},
	{NEXT_SECTION_BOX_DATA_OFFSET, SEEK_CUR, NUM_POINTS_TO_ANCHOR_OFFSET, SEEK_CUR},
	{NEXT_SECTION_BOX_DATA_OFFSET, SEEK_CUR, NUM_POINTS_TO_ANCHOR_OFFSET, SEEK_CUR},
	{NEXT_SECTION_BOX_DATA_OFFSET, SEEK_CUR, NUM_POINTS_TO_ANCHOR_OFFSET, SEEK_CUR}
    };

    //make sure we have a good file type
    if (data->m_fileType == -1)
	return NC_ERROR;

    //advance file pointer to necessary data section
    fseek(input,offset,SEEK_SET);

    //Conevenience and opt if compiler doesn't already do it
    curve = &data->curves[0];

    //set curve type
    curve->m_curveType = TONE_CURVE;

    //read patch version
    fseek(input,FileOffsets[data->m_fileType][PATCH_DATA],SEEK_SET);
    nc_fread(&data->m_patch_version,sizeof(unsigned short),1,input);
    data->m_patch_version = ShortVal(data->m_patch_version);

    // read all tone curves data follow from here
    int i;
    for(i = 0; i < NUM_CURVE_TYPES; i++)
    {
	curve = &data->curves[i];

	//set curve type
	curve->m_curveType = i;

	//get box data
	fseek(input, curveFilePos[i][0], curveFilePos[i][1]);

	nc_fread(&curve->m_min_x,sizeof(double),1,input);
	curve->m_min_x = DoubleVal(curve->m_min_x);

	nc_fread(&curve->m_max_x,sizeof(double),1,input);
	curve->m_max_x = DoubleVal(curve->m_max_x);

	nc_fread(&curve->m_gamma,sizeof(double),1,input);
	curve->m_gamma = DoubleVal(curve->m_gamma);

	nc_fread(&curve->m_min_y,sizeof(double),1,input);
	curve->m_min_y = DoubleVal(curve->m_min_y);

	nc_fread(&curve->m_max_y,sizeof(double),1,input);
	curve->m_max_y = DoubleVal(curve->m_max_y);

	//get number of anchors (always located after box data)
	nc_fread(&curve->m_numAnchors,1,1,input);

	// It seems that if there is no curve then the 62 bytes in the buffer
	// are either all 0x00 (D70) or 0xFF (D2H).
	// We therefore switch these values with the default values.
	if (curve->m_min_x==1.0) {
	    curve->m_min_x = 0.0;
	    DEBUG_PRINT("DEBUG: NEF X MIN -> %e (changed)\n",curve->m_min_x);
	}
	if (curve->m_max_x==0.0) {
	    curve->m_max_x = 1.0;
	    DEBUG_PRINT("DEBUG: NEF X MAX -> %e (changed)\n",curve->m_max_x);
	}
	if (curve->m_min_y==1.0) {
	    curve->m_min_y = 0.0;
	    DEBUG_PRINT("DEBUG: NEF Y MIN -> %e (changed)\n",curve->m_min_y);
	}
	if (curve->m_max_y==0.0) {
	    curve->m_max_y = 1.0;
	    DEBUG_PRINT("DEBUG: NEF Y MAX -> %e (changed)\n",curve->m_max_y);
	}
	if (curve->m_gamma==0.0 || curve->m_gamma==255.0+255.0/256.0) {
	    curve->m_gamma = 1.0;
	    DEBUG_PRINT("DEBUG: NEF GAMMA -> %e (changed)\n",curve->m_gamma);
	}
	if (curve->m_numAnchors==255) {
	    curve->m_numAnchors = 0;
	    DEBUG_PRINT("DEBUG: NEF NUMBER OF ANCHORS -> %u (changed)\n",
		    curve->m_numAnchors);
	}
	if (curve->m_numAnchors > NIKON_MAX_ANCHORS) {
	    curve->m_numAnchors = NIKON_MAX_ANCHORS;
	    DEBUG_PRINT("DEBUG: NEF NUMBER OF ANCHORS -> %u (changed)\n",
		    curve->m_numAnchors);
	}
	//Move to start of the anchor data
	fseek(input, curveFilePos[i][2], curveFilePos[i][3]);

	//read in the anchor points
	int rs = nc_fread(curve->m_anchors, sizeof(CurveAnchorPoint),
		curve->m_numAnchors, input);
	if (curve->m_numAnchors != rs) {
	    nc_message(NC_SET_ERROR, "Error reading all anchor points\n");
	    return NC_ERROR;
	}

	int j;
	for (j = 0; j < curve->m_numAnchors; j++)
	{
	    curve->m_anchors[j].x = DoubleVal(curve->m_anchors[j].x);
	    curve->m_anchors[j].y = DoubleVal(curve->m_anchors[j].y);
	}

	DEBUG_PRINT("DEBUG: Loading Data:\n");
	DEBUG_PRINT("DEBUG: CURVE_TYPE: %u \n",curve->m_curveType);
	DEBUG_PRINT("DEBUG: BOX->MIN_X: %f\n",curve->m_min_x);
	DEBUG_PRINT("DEBUG: BOX->MAX_X: %f\n",curve->m_max_x);
	DEBUG_PRINT("DEBUG: BOX->GAMMA: %f\n",curve->m_gamma);
	DEBUG_PRINT("DEBUG: BOX->MIN_Y: %f\n",curve->m_min_y);
	DEBUG_PRINT("DEBUG: BOX->MAX_Y: %f\n",curve->m_max_x);

#ifdef _DEBUG
	int i_dbg;
	for(i_dbg = 0; i_dbg < curve->m_numAnchors; i_dbg++)
	{
	    DEBUG_PRINT("DEBUG: ANCHOR X,Y: %f,%f\n",
		    curve->m_anchors[i_dbg].x,curve->m_anchors[i_dbg].y);
	}
	DEBUG_PRINT("\n");
#endif
    }
    fclose(input);
    return NC_SUCCESS;
}

/*********************************************
CurveDataSample:
    Samples from a spline curve constructed from
    the Nikon data.

    curve   - Pointer to curve struct to hold the data.
    sample  - Pointer to sample struct to hold the data.
**********************************************/
int CurveDataSample(CurveData *curve, CurveSample *sample)
{
    int i = 0, n;

    double x[20];
    double y[20];

    //The box points (except the gamma) are what the anchor points are relative
    //to so...

    double box_width = curve->m_max_x - curve->m_min_x;
    double box_height = curve->m_max_y - curve->m_min_y;
    double gamma = 1.0/curve->m_gamma;

    //build arrays for processing
    if (curve->m_numAnchors == 0)
    {
	//just a straight line using box coordinates
	x[0] = curve->m_min_x;
	y[0] = curve->m_min_y;
	x[1] = curve->m_max_x;
	y[1] = curve->m_max_y;
	n = 2;
    }
    else
    {
	for(i = 0; i < curve->m_numAnchors; i++)
	{
	    x[i] = curve->m_anchors[i].x*box_width + curve->m_min_x;
	    y[i] = curve->m_anchors[i].y*box_height + curve->m_min_y;
	}
	n = curve->m_numAnchors;
    }
    //returns an array of second derivatives used to calculate the spline curve.
    //this is a malloc'd array that needs to be freed when done.
    //The setings currently calculate the natural spline, which closely matches
    //camera curve output in raw files.
    double *ypp = spline_cubic_set(n, x, y, 2, 0.0, 2, 0.0);
    if (ypp==NULL) return NC_ERROR;

    //first derivative at a point
    double ypval = 0;

    //second derivate at a point
    double yppval = 0;

    //Now build a table
    int val;
    double res = 1.0/(double)(sample->m_samplingRes-1);

    //allocate enough space for the samples
    DEBUG_PRINT("DEBUG: SAMPLING ALLOCATION: %u bytes\n",
	        sample->m_samplingRes*sizeof(int));
    DEBUG_PRINT("DEBUG: SAMPLING OUTPUT RANGE: 0 -> %u\n", sample->m_outputRes);

    // sample->m_Samples = (unsigned short int *)realloc(sample->m_Samples,
	    // sample->m_samplingRes * sizeof(short int));
    // nc_merror(sample->m_Samples, "CurveDataSample");

    int firstPointX = x[0] * (sample->m_samplingRes-1);
    int firstPointY = pow(y[0], gamma) * (sample->m_outputRes-1);
    int lastPointX = x[n-1] * (sample->m_samplingRes-1);
    int lastPointY = pow(y[n-1], gamma) * (sample->m_outputRes-1);
    int maxY = curve->m_max_y * (sample->m_outputRes-1);
    int minY = curve->m_min_y * (sample->m_outputRes-1);

    for(i = 0; i < (int)sample->m_samplingRes; i++)
    {
	//get the value of the curve at a point
	//take into account that curves may not necessarily begin at x = 0.0
	//nor end at x = 1.0

	//Before the first point and after the last point, take a strait line
	if (i < firstPointX) {
	    sample->m_Samples[i] = firstPointY;
	} else if (i > lastPointX) {
	    sample->m_Samples[i] = lastPointY;
	} else {
	    //within range, we can sample the curve
	    if (gamma==1.0)
		val = spline_cubic_val( n, x, i*res, y,
			ypp, &ypval, &yppval ) * (sample->m_outputRes-1) + 0.5;
	    else
		val = pow(spline_cubic_val( n, x, i*res, y,
			ypp, &ypval, &yppval ), gamma) *
			(sample->m_outputRes-1) + 0.5;

	    sample->m_Samples[i] = MIN(MAX(val,minY),maxY);
	}
    }

    free(ypp);
    return NC_SUCCESS;
}

/*********************************************
CurveDataReset:
    Reset curve to straight line but don't touch the curve name.
**********************************************/
void CurveDataReset(CurveData *curve)
{
    curve->m_min_x = 0;
    curve->m_max_x = 1;
    curve->m_min_y = 0;
    curve->m_max_y = 1;
    curve->m_gamma = 1;
    curve->m_numAnchors = 2;
    curve->m_anchors[0].x = 0;
    curve->m_anchors[0].y = 0;
    curve->m_anchors[1].x = 1;
    curve->m_anchors[1].y = 1;
}

/*********************************************
CurveDataIsTrivial:
    Check if the curve is a trivial linear curve.
**********************************************/
int CurveDataIsTrivial(CurveData *curve)
{
    if ( curve->m_min_x != 0 ) return FALSE;
    if ( curve->m_max_x != 1 ) return FALSE;
    if ( curve->m_min_y != 0 ) return FALSE;
    if ( curve->m_max_y != 1 ) return FALSE;
    if ( curve->m_numAnchors < 2 ) return TRUE;
    if ( curve->m_numAnchors != 2 ) return FALSE;
    if ( curve->m_anchors[0].x != 0 ) return FALSE;
    if ( curve->m_anchors[0].y != 0 ) return FALSE;
    if ( curve->m_anchors[1].x != 1 ) return FALSE;
    if ( curve->m_anchors[1].y != 1 ) return FALSE;
    return TRUE;
}

/*********************************************
CurveDataSetPoint:
   Change the position of point to the new (x,y) coordinate.
   The end-points get a special treatment. When these are moved all the
   other points are moved together, keeping their relative position constant.
**********************************************/
void CurveDataSetPoint(CurveData *curve, int point, double x, double y)
{
    int i;
    double left = curve->m_anchors[0].x;
    double right = curve->m_anchors[curve->m_numAnchors-1].x;
    if (point==0) {
	for (i=0; i<curve->m_numAnchors; i++)
	    curve->m_anchors[i].x = x + (curve->m_anchors[i].x-left) *
		    (right-x) / (right-left);
    } else if (point==curve->m_numAnchors-1) {
	for (i=0; i<curve->m_numAnchors; i++)
	    curve->m_anchors[i].x = left + (curve->m_anchors[i].x-left) *
		(x-left) / (right-left);
    } else {
	curve->m_anchors[point].x = x;
    }
    curve->m_anchors[point].y = y;
}

/****************************************************
SampleToCameraCurve:

    EXPERIMENTAL!!!!!

    Transforms the curve generated by sampling the
    spline interpolator into the curve that is used by
    the camera.

    This is a special function. While the function places
    no special restrictions on sampling resolution or
    output resolution, it should be noted that Nikon D70
    camera curve is 4096 entries of 0-255.

    If you intend on using this function as such, you should
    set the sampling resolution and output resolution
    accordingly.

    curve - The Nikon curve to sample and transform.
*****************************************************/
#define CAMERA_LINEAR_CURVE_SLOPE 0.26086956521739130434782608695652
#define CAMERA_LINEAR_LIMIT ((276.0/4096.0)*65536.0)

int SampleToCameraCurve(CurveData *curve, CurveSample *sample)
{
    unsigned int i = 0;

    if (curve->m_numAnchors < 2)
    {
	nc_message(NC_SET_ERROR, "Not enough anchor points(need at least two)!\n");
	return NC_ERROR;
    }

    double x[20];
    double y[20];

    //The box points (except the gamma) are what the anchor points are relative
    //to so...

    double box_width = curve->m_max_x - curve->m_min_x;
    double box_height = curve->m_max_y - curve->m_min_y;
    double gamma = 1.0/curve->m_gamma;

    //build arrays for processing
    if (curve->m_numAnchors == 0)
    {
	//just a straight line using box coordinates
	x[0] = curve->m_min_x;
	y[0] = curve->m_min_y;
	x[1] = curve->m_max_x;
	y[1] = curve->m_max_y;
    }
    else
    {
	for(i = 0; i < (unsigned int)curve->m_numAnchors; i++)
	{
	    x[i] = curve->m_anchors[i].x*box_width + curve->m_min_x;
	    y[i] = curve->m_anchors[i].y*box_height + curve->m_min_y;
	}
    }

    //returns an array of second derivatives used to calculate the spline curve.
    //this is a malloc'd array that needs to be freed when done.
    //The setings currently calculate the natural spline, which closely matches
    //camera curve output in raw files.
    double *ypp = spline_cubic_set(curve->m_numAnchors,x,y,2, 0.0, 2, 0.0);
    if (ypp==NULL) return NC_ERROR;

    //first derivative at a point
    double ypval = 0;

    //second derivate at a point
    double yppval = 0;

    //Now build a table
    double val = 0;
    double res = 1.0/(double)sample->m_samplingRes;

    DEBUG_PRINT("DEBUG: SAMPLING RESOLUTION: %u bytes\n",
	        sample->m_samplingRes*sizeof(int));
    DEBUG_PRINT("DEBUG: SAMPLING OUTPUT RANGE: 0 -> %u\n", sample->m_outputRes);

    double outres = sample->m_outputRes;

    for(i = 0; i < sample->m_samplingRes; i++)
    {
	//get the value of the curve at a point
	//take into account that curves may not necessarily begin at x = 0.0
	//nor end at x = 1.0
	if (i*res < curve->m_min_x || i*res > curve->m_max_x)
	{
	    val = 0.0;
	}
	else
	{
	    //within range, okay to sample the curve
	    val = spline_cubic_val ( curve->m_numAnchors, x, i*res,
			    y, ypp, &ypval, &yppval );

	    //Compensate for gamma.
	    val = pow(val,gamma);

	    //cap at the high end of the range
	    if (val > curve->m_max_y)
	    {
	        val = curve->m_max_y;
	    }
	    //cap at the low end of the range
	    else if (val < curve->m_min_y)
	    {
	        val = curve->m_min_y;
	    }

	    //transform "linear curve" to the camera curve
	    //outres = 4096;
	    //val *= outres;

	    //this equation is used inside Nikon's program to transform
	    //the curves into the camera curves.
	    //FIX LINEAR SECTION
	    /*if (val < CAMERA_LINEAR_CURVE_SLOPE)
	    {
	        //do linear
	        val = val*4096*CAMERA_LINEAR_CURVE_SLOPE;

	    }
	    else*/
	    {
	        //do real curve??
	        val = ( log(7*val+1.0) / log(4*val+2.0) )*142.0+104.0*(val);
	    }

	    //cap at the high end of the range
	    if (val > outres*curve->m_max_y)
	    {
	        val = outres;
	    }
	    //cap at the low end of the range
	    else if (val < curve->m_min_y*outres)
	    {
	        val = curve->m_min_y*outres;
	    }

	}

	//save the sample
	sample->m_Samples[i] = (unsigned int)floor(val);
    }

    free(ypp);
    return NC_SUCCESS;
}

/************************************************************
SaveNikonDataFile:
    Savess a curve to a Nikon ntc or ncv file.

    data        - A NikonData structure containing info of all the curves.
    fileName    - The filename.
    filetype    - Indicator for an NCV or NTC file.
    version        - The version of the Nikon file to write
**************************************************************/
int SaveNikonDataFile(NikonData *data, char *outfile, int filetype, int version)
{
    FILE *output = NULL;
    int i = 0,r = 0,g = 0,b = 0;
    unsigned short_tmp = 0;
    unsigned int long_tmp = 0;
    double double_tmp = 0;
    CurveData *curve = NULL;
    version = version;

    //used for file padding
    unsigned char pad[32];
    memset(pad,0,32);

    output = g_fopen(outfile,"wb+");
    if (!output)
    {
	nc_message(NC_SET_ERROR, "Error creating curve file '%s': %s\n",
	        outfile, strerror(errno));
	return NC_ERROR;
    }

    //write out file header
    nc_fwrite(FileTypeHeaders[filetype],HEADER_SIZE,1,output);

    if (filetype == NCV_FILE)
    {
	//write out unknown header bytes
	short_tmp = ShortVal(NCV_UNKNOWN_HEADER_DATA);
	nc_fwrite(&short_tmp, 2, 1, output);

	//write out file size - header
	//Placeholder.The real filesize is written at the end.
	//NCV files have two size location, one here and one in the
	//NTC section of the file
	long_tmp = 0;
	nc_fwrite(&long_tmp, 4, 1, output);

	//write second header chunk
	nc_fwrite(NCVSecondFileHeader,1,NCV_SECOND_HEADER_LENGTH,output);

	//From here until almost the end, the file is an NTC file
	nc_fwrite(NTCFileHeader,NTC_FILE_HEADER_LENGTH,1,output);
    }

    //patch version? (still unsure about this one)
    if (data->m_patch_version < NIKON_PATCH_4)
    {
	    data->m_patch_version = NIKON_PATCH_5;
    }
    short_tmp = ShortVal(data->m_patch_version);
    nc_fwrite(&short_tmp, 2, 1, output);

    //write out file size - header
    //Placeholder.The real filesize is written at the end.
    long_tmp = 0;
    nc_fwrite(&long_tmp, 4, 1, output);

    //write out version
    unsigned int forced_ver = ShortVal(NIKON_VERSION_4_1);
    nc_fwrite(&forced_ver, 4, 1, output);

    //write out pad (this is a 7 byte pad)
    nc_fwrite(&pad,1,7,output);

    //now wash and repeat for the four sections of data
    for(i = 0; i < 4; i++)
    {
	//write out section header (same as NTC file header)
	nc_fwrite(FileSectionHeader,1,NTC_FILE_HEADER_LENGTH,output);

	//write out section type
	long_tmp = LongVal(i);
	nc_fwrite(&long_tmp,4,1,output);

	//write out unknown data
	short_tmp = ShortVal(NTC_UNKNOWN_DATA);
	nc_fwrite(&short_tmp,2,1,output);

	//write out pad byte
	nc_fwrite(pad,1,1,output);

	//write out components
	switch (i)
	{
	    case 0:
	        r = g = b = 0;
	    break;

	    case 1:
	        r = 255;
	        g = b = 0;
	    break;

	    case 2:
	        g = 255;
	        r = b = 0;
	    break;

	    case 3:
	        b = 255;
	        g = r = 0;
	    break;
	}

	long_tmp = LongVal(r);
	nc_fwrite(&long_tmp,4,1,output);

	long_tmp = LongVal(g);
	nc_fwrite(&long_tmp,4,1,output);

	long_tmp = LongVal(b);
	nc_fwrite(&long_tmp,4,1,output);

	//write out pad (12 byte pad)
	nc_fwrite(pad,12,1,output);

	//write out rgb weights
	switch (i)
	{
	    case 0:
	        r = g = b = 255;
	    break;

	    case 1:
	        r = 255;
	        g = b = 0;
	    break;

	    case 2:
	        g = 255;
	        r = b = 0;
	    break;

	    case 3:
	        b = 255;
	        g = r = 0;
	    break;
	}

	long_tmp = LongVal(r);
	nc_fwrite(&long_tmp,4,1,output);

	long_tmp = LongVal(g);
	nc_fwrite(&long_tmp,4,1,output);

	long_tmp = LongVal(b);
	nc_fwrite(&long_tmp,4,1,output);

	curve = &data->curves[i];
	//write out curve data
	if (curve->m_numAnchors >= 2)
	{
	    //we have a legit curve, use the data as is
	    double_tmp = DoubleVal(curve->m_min_x);
	    nc_fwrite(&double_tmp,sizeof(double),1,output);

	    double_tmp = DoubleVal(curve->m_max_x);
	    nc_fwrite(&double_tmp,sizeof(double),1,output);

	    double_tmp = DoubleVal(curve->m_gamma);
	    nc_fwrite(&double_tmp,sizeof(double),1,output);

	    double_tmp = DoubleVal(curve->m_min_y);
	    nc_fwrite(&double_tmp,sizeof(double),1,output);

	    double_tmp = DoubleVal(curve->m_max_y);
	    nc_fwrite(&double_tmp,sizeof(double),1,output);

	    //write out number of anchor points (minimum is two)
	    nc_fwrite(&curve->m_numAnchors,1,1,output);

	    //write out pad
	    nc_fwrite(pad,NUM_POINTS_TO_ANCHOR_OFFSET,1,output);

	    //write out anchor point data
	    if (curve->m_anchors)
	    {
		int i;
		for (i = 0; i < curve->m_numAnchors; i++)
		{
		    double_tmp = DoubleVal(curve->m_anchors[i].x);
		    nc_fwrite(&double_tmp,sizeof(double),1,output);
		    double_tmp = DoubleVal(curve->m_anchors[i].y);
		    nc_fwrite(&double_tmp,sizeof(double),1,output);
		}
	    }
	    else
	    {
	        nc_message(NC_SET_ERROR,"Curve anchor data is NULL! Aborting file write!\n");
	        return NC_ERROR;
	    }
	}
	else
	{
	    DEBUG_PRINT("NOTE: There are < 2 anchor points for curve %u! Forcing curve defaults.\n",i);
	    DEBUG_PRINT("This should not be a concern unless it is happening for curve 0\n");
	    //This curve either has not been correctly initialized or is empty.
	    //Force defaults.
	    double default_val = 0;
	    nc_fwrite(&default_val,sizeof(double),1,output); //min x
	    default_val = DoubleVal(1.0);
	    nc_fwrite(&default_val,sizeof(double),1,output); //max_x
	    //gamma has a default of 1
	    default_val = DoubleVal(1.0);
	    nc_fwrite(&default_val,sizeof(double),1,output); //gamma
	    default_val = 0;
	    nc_fwrite(&default_val,sizeof(double),1,output); //min y
	    default_val = DoubleVal(1.0);
	    nc_fwrite(&default_val,sizeof(double),1,output); //max y

	    //force the number of anchors to be 2
	    unsigned char num = 2;
	    nc_fwrite(&num,1,1,output);

	    //write out pad
	    nc_fwrite(pad,NUM_POINTS_TO_ANCHOR_OFFSET,1,output);

	    //if the number of anchors was < 2, force default values.
	    default_val = 0;
	    nc_fwrite(&default_val,sizeof(double),1,output); //min x
	    nc_fwrite(&default_val,sizeof(double),1,output); //min y
	    default_val = DoubleVal(1.0);
	    nc_fwrite(&default_val,sizeof(double),1,output); //max x
	    nc_fwrite(&default_val,sizeof(double),1,output); //max y

	}

	//write out pad
	nc_fwrite(pad,END_ANCHOR_DATA_PAD_LENGTH,1,output);
    }

    if (filetype == NCV_FILE)
    {
	//write out the file terminator if this is an NCV file
	nc_fwrite(NCVFileTerminator,NCV_FILE_TERMINATOR_LENGTH,1,output);
    }

    //calculate the file size
    //size = filesize - size of header - 2 bytes (unknown data after the end of the header)
    long size = ftell(output)-HEADER_SIZE-2;

    //set the file write position to the size location
    fseek(output, FILE_SIZE_OFFSET, SEEK_SET);

    //write out the file size
    size = LongVal(size);
    nc_fwrite(&size,4,1,output);

    if (filetype == NCV_FILE)
    {
	    //another size needs to placed in the NTC header inside the file
	    fseek(output, NCV_SECOND_FILE_SIZE_OFFSET, SEEK_SET);

	    //The - 6 is interesting. The last 6 bytes of the terminator must have some special meaning because
	    //the calculated size in files from the Nikon progs always calculate size bytes short.
	    //I'm assuming it is more than coincedence that those bytes match the last 6 bytes
	    //of the NCV second file header. I've yet to determine their significance.
	    size = LongVal(size - NCV_HEADER_SIZE - 6);
	    nc_fwrite(&size,4,1,output);

    }
    fclose(output);

    return NC_SUCCESS;
}

/************************************************************
SaveNikonCurveFile:
    Saves out curves to a Nikon ntc or ncv file. This function
    takes a single curve and uses defaults for the other curves.
    Typically, the curve used is the tone curve.

    curve        - A CurveData structure. This is usually the tone curve
    curve_type    - The curve type (TONE_CURVE, RED_CURVE, etc.)
    fileName    - The filename.
    filetype    - Indicator for an NCV or NTC file.
    version        - The version of the Nikon file to write

NOTE:    The only version tested is Nikon 4.1 anything
	other than this may result in unpredictable behavior.
	For now, the version passed in is ignored and is forced
	to 4.1.

	This function is just a helper function that allows the user
	to just carry around a single curve.
**************************************************************/
int SaveNikonCurveFile(CurveData *curve, int curve_type, char *outfile,
	int filetype, int version)
{
    NikonData data;
    //clear the structure
    memset(&data,0,sizeof(data));
    //assume that it's the tone curve
    data.curves[curve_type] = *curve;
    //call the work horse
    return SaveNikonDataFile(&data, outfile, filetype, version);
}

/*********************************************
SaveSampledNikonCurve:
    Saves a sampling from a curve to text file to
    be processed by UFRaw.

    sample      - Pointer to sampled curve struct to hold the data.
    fileName    - The filename.
**********************************************/
int SaveSampledNikonCurve(CurveSample *sample, char *outfile)
{
    unsigned int i = 0;
    FILE *output = NULL;

    if (outfile == NULL || strlen(outfile) == 0)
    {
	nc_message(NC_SET_ERROR,
	    "Output filename cannot be null or empty!\n");
    }

    output = g_fopen(outfile,"wb+");

    if (!output)
    {
	nc_message(NC_SET_ERROR, "Error creating curve file '%s': %s\n",
	        outfile, strerror(errno));
	return NC_ERROR;
    }

    if (!sample->m_Samples)
    {
	nc_message(NC_SET_ERROR,
	        "Sample array has not been allocated or is corrupt!\n");
	return NC_ERROR;
    }

    DEBUG_PRINT("DEBUG: OUTPUT FILENAME: %s\n",outfile);
    fprintf(output,"%u %u\n",0,sample->m_Samples[0]);
    for(i = 1; i < sample->m_samplingRes; i++)
    {
	// Print sample point only if different than previous one
	if (sample->m_Samples[i]!=sample->m_Samples[i-1])
	{
	    fprintf(output,"%u %u\n",i,sample->m_Samples[i]);
	}
    }
    // Make sure the last point is also printed
    if (sample->m_Samples[i-1]==sample->m_Samples[i-2])
    {
	fprintf(output,"%u %u\n",i-1,sample->m_Samples[i-1]);
    }

    fclose(output);
    return NC_SUCCESS;
}

/*******************************************************
CurveSampleInit:
    Init and allocate curve sample.
********************************************************/
CurveSample *CurveSampleInit(unsigned int samplingRes, unsigned int outputRes)
{
    CurveSample *sample = (CurveSample*)calloc(1, sizeof(CurveSample));
    nc_merror(sample, "CurveSampleInit");
    sample->m_samplingRes = samplingRes;
    sample->m_outputRes = outputRes;
    if (samplingRes>0) {
	sample->m_Samples = (unsigned short int*)calloc(samplingRes, sizeof(short int));
	nc_merror(sample->m_Samples, "CurveSampleInit");
    } else {
	sample->m_Samples = NULL;
    }
    return sample;
}

/*******************************************************
CurveSampleFree:
    Frees memory allocated for this curve sample.
********************************************************/
int CurveSampleFree(CurveSample *sample)
{
    //if these are null, they've already been deallocated
    if (sample==NULL) return NC_SUCCESS;

    if (sample->m_Samples!=NULL)
    {
	free(sample->m_Samples);
	sample->m_Samples = NULL;
    }

    free(sample);

    return NC_SUCCESS;
}

/****************************************
ConvertNikonCurveData:
    The main driver. Takes a filename and
    processes the curve, if possible.

    fileName    -    The file to process.
*****************************************/
int ConvertNikonCurveData(char *inFileName, char *outFileName,
    unsigned int samplingRes, unsigned int outputRes)
{
    //Load the curve data from the ncv/ntc file
    NikonData data;
    char tmpstr[1024];

    if ( samplingRes <= 1 || outputRes <= 1 || samplingRes > MAX_RESOLUTION
	    || outputRes > MAX_RESOLUTION )
    {
	nc_message(NC_SET_ERROR, "Error, sampling and output resolution"
	            "must be 1 <= res <= %u\n", MAX_RESOLUTION);
	return NC_ERROR;
    }

    //loads all the curve data. Does not allocate sample arrays.
    if (LoadNikonData(inFileName, &data) != NC_SUCCESS)
    {
	return NC_ERROR;
    }

    CurveSample *sample = CurveSampleInit(samplingRes, outputRes);

    //Cycle through all curves
    int i;
    for (i = 0; i < NUM_CURVE_TYPES; i++)
    {
	//Populates the samples array for the given curve
	if (SampleToCameraCurve( &data.curves[i], sample) != NC_SUCCESS)
	{
	    CurveSampleFree(sample);
	    return NC_ERROR;
	}

	//rename output files
	strncpy(tmpstr, outFileName, 1023);
	tmpstr[1023] = '\0';
	//if the name has an extension, attempt to remove it
	if (tmpstr[strlen(tmpstr)-4] == '.')
	{
	    tmpstr[strlen(tmpstr)-4] = '\0';
	}

	switch(i)
	{
	    case TONE_CURVE:
	        strncat(tmpstr, "_TONE.txt", 1023);
	    break;

	    case RED_CURVE:
	        strncat(tmpstr, "_RED.txt", 1023);
	    break;

	    case GREEN_CURVE:
	        strncat(tmpstr, "_GREEN.txt", 1023);
	    break;

	    case BLUE_CURVE:
	        strncat(tmpstr, "_BLUE.txt", 1023);
	    break;

	    default:
	        //should never get here
	    break;
	}

	//print out curve data
	if (SaveSampledNikonCurve(sample, tmpstr) != NC_SUCCESS)
	{
	    CurveSampleFree(sample);
	    return NC_ERROR;
	}
    }

    //must be called when finished with a CurveSample structure
    CurveSampleFree(sample);

    return NC_SUCCESS;
}

/*****************************************************
FindTIFFOffset:
    Moves the file pointer to the location
    indicated by the TAG-TYPE pairing. This is meant just
    as a helper function for this code. Uses elsewhere
    may be limited.

    file    - Nikon File ptr
    num_entries - Number of entries to search through
    tiff_tag - The tiff tag to match.
    tiff_type - The tiff type to match.
*******************************************************/
int FindTIFFOffset(FILE *file, unsigned short num_entries, unsigned short tiff_tag, unsigned short tiff_type)
{
    unsigned short tag = 0;
    unsigned short type = 0;
    unsigned int offset = 0;

    int i;
    for(i = 0; i < num_entries; i++)
    {
	//get tag 2 bytes
	tag = (fgetc(file)<<8)|fgetc(file);
	if (tag == tiff_tag)
	{
	    //get type 2 bytes
	    type = (fgetc(file)<<8)|fgetc(file);
	    if (type == tiff_type)    //Type for length of field
	    {
	        //get length (4 bytes)
	        offset = (fgetc(file)<<24)|(fgetc(file)<<16)|(fgetc(file)<<8)|fgetc(file);
	        //get value\offset 4 bytes
	        offset = (fgetc(file)<<24)|(fgetc(file)<<16)|(fgetc(file)<<8)|fgetc(file);
	        fseek(file,offset,SEEK_SET);
	        return 1; //true;
	    }
	}
	else
	{
	    //advance to next entry
	    fseek(file,10,SEEK_CUR);
	}
    }
    return 0; //false;
}

/*******************************************************
RipNikonNEFData:
    Gets Nikon NEF data. For now, this is just the tone
    curve data.

    infile -	The input file
    curve  -	data structure to hold data in.
    sample_p -	pointer to the curve sample reference.
		can be NULL if curve sample is not needed.
********************************************************/
int RipNikonNEFData(char *infile, CurveData *data, CurveSample **sample_p)
{
    unsigned short byte_order = 0;
    unsigned short num_entries = 0;
    unsigned short version = 0;
    unsigned int offset = 0;

    //open the file
    FILE *file = g_fopen(infile,"rb");

    //make sure we have a valid file
    if (file == NULL)
    {
	nc_message(NC_SET_ERROR, "Error opening '%s': %s\n",
	        infile, strerror(errno));
	return NC_ERROR;
    }

    //gets the byte order
    nc_fread(&byte_order,2,1,file);
    byte_order = ShortVal(byte_order);
    if (byte_order != 0x4d4d)
    {
	//Must be in motorola format if it came from a NIKON
	nc_message(NC_SET_ERROR,
	    "NEF file data format is Intel. Data format should be Motorola.\n");
	return NC_ERROR;
    }

    //get the version
    //nc_fread(&version,2,1,file);
    version = (fgetc(file)<<8)|fgetc(file);
    if (version != 0x002a)
    {
	//must be 42 or not a valid TIFF
	nc_message(NC_SET_ERROR,
	    "NEF file version is %u. Version should be 42.\n",version);
	return NC_ERROR;
    }

    //get offset to first IFD
    offset = (fgetc(file)<<24)|(fgetc(file)<<16)|(fgetc(file)<<8)|fgetc(file);
    //go to the IFD
    fseek(file,offset,SEEK_SET);
    //get number of entries
    num_entries = (fgetc(file)<<8)|fgetc(file);

    //move file pointer to exif offset
    if (!FindTIFFOffset(file,num_entries,TIFF_TAG_EXIF_OFFSET,TIFF_TYPE_LONG))
    {
	nc_message(NC_SET_ERROR,
	    "NEF data entry could not be found with tag %u and type %u.\n",
	    TIFF_TAG_EXIF_OFFSET, TIFF_TYPE_LONG);
	return NC_ERROR;
    }

    //get number of entries
    num_entries = (fgetc(file)<<8)|fgetc(file);

    //move file pointer to maker note offset
    if (!FindTIFFOffset(file,num_entries,TIFF_TAG_MAKER_NOTE_OFFSET,TIFF_TYPE_UNDEFINED))
    {
	nc_message(NC_SET_ERROR,
	    "NEF data entry could not be found with tag %u and type %u.\n",
	    TIFF_TAG_MAKER_NOTE_OFFSET, TIFF_TYPE_UNDEFINED);
	return NC_ERROR;
    }

    //////////////////////////////////////////////////////////////////////////
    //NOTE: At this point, this section of the file acts almost like another
    //      file header. Skip the first bytes, (which just say nikon with a
    //      few bytes at the end. Offsets from here on in are from the start
    //      of this section, not the start of the file.
    //////////////////////////////////////////////////////////////////////////

    //Check the name. If it isn't Nikon then we can't do anything with this file.
    char name[6];
    nc_fread(name,6,1,file);
    if (strcmp(name,"Nikon") != 0)
    {
	nc_message(NC_SET_ERROR,
	    "NEF string identifier is %s. Should be: Nikon.\n",name);
	return NC_ERROR;
    }
    fseek(file,4,SEEK_CUR);

    //save the current file location, as all other offsets for this section run off this.
    unsigned long pos = ftell(file);

    //get byte order (use a regular fread)
    nc_fread(&byte_order,2,1,file);
    byte_order = ShortVal(byte_order);
    if (byte_order != 0x4d4d)
    {
	//Must be in motorola format or not from a Nikon
	nc_message(NC_SET_ERROR,
	    "NEF secondary file data format is Intel. "
	    "Data format should be Motorola.\n");
	return NC_ERROR;
    }

    //get version
    version = (fgetc(file)<<8)|fgetc(file);
    if (version != 0x002a)
    {
	nc_message(NC_SET_ERROR,
	    "NEF secondary file version is %u. Version should be 42.\n",
	    version);
	return NC_ERROR;
    }

    //get offset to first IFD
    offset = (fgetc(file)<<24)| (fgetc(file)<<16)|(fgetc(file)<<8)|fgetc(file);
    //go to the IFD (these offsets are NOT from the start of the file,
    //just the start of the section).
    fseek(file,pos+offset,SEEK_SET);
    //get number of entries
    num_entries = (fgetc(file)<<8)|fgetc(file);

    //move file position to tone curve data
    if (!FindTIFFOffset(file,num_entries,TIFF_TAG_CURVE_OFFSET,TIFF_TYPE_UNDEFINED))
    {
	nc_message(NC_SET_ERROR,
	    "NEF data entry could not be found with tag %u and type %u.\n",
	    TIFF_TAG_CURVE_OFFSET, TIFF_TYPE_UNDEFINED);
	return NC_ERROR;
    }

    offset = ftell(file);
    return RipNikonNEFCurve(file, offset+pos, data, sample_p);
}


/*******************************************************
RipNikonNEFCurve:
    The actual retriever for the curve data from the NEF
    file.

    file   - The input file.
    infile - Offset to retrieve the data
    curve  - data structure to hold curve in.
    sample_p -	pointer to the curve sample reference.
		can be NULL if curve sample is not needed.
********************************************************/
int RipNikonNEFCurve(FILE *file, int offset, CurveData *data,
	CurveSample **sample_p)
{
    int i;

    //seek to the offset of the data. Skip first two bytes (section isn't needed).
    fseek(file, offset+2, SEEK_SET);

    memset(data,0,sizeof(CurveData));
    /////////////////////////////////////////////////
    //GET CURVE DATA
    /////////////////////////////////////////////////
    //get box data and gamma
    data->m_min_x = (double)fgetc(file)/255.0;
    data->m_max_x = (double)fgetc(file)/255.0;
    data->m_min_y = (double)fgetc(file)/255.0;
    data->m_max_y = (double)fgetc(file)/255.0;
    //16-bit fixed point.
    data->m_gamma = (double)fgetc(file) + ((double)fgetc(file)/256.0);

    //DEBUG_PRINT("DEBUG: NEF SECTION SIZE -> %u\n",data->section_size);
    DEBUG_PRINT("DEBUG: NEF X MIN -> %e\n",data->m_min_x);
    DEBUG_PRINT("DEBUG: NEF X MAX -> %e\n",data->m_max_x);
    DEBUG_PRINT("DEBUG: NEF Y MIN -> %e\n",data->m_min_y);
    DEBUG_PRINT("DEBUG: NEF Y MAX -> %e\n",data->m_max_y);
    //DEBUG_PRINT("DEBUG: NEF GAMMA (16-bit fixed point) -> %e\n",(data->m_gamma>>8)+(data->m_gamma&0x00ff)/256.0);
    DEBUG_PRINT("DEBUG: NEF GAMMA -> %e\n",data->m_gamma);
    // It seems that if there is no curve then the 62 bytes in the buffer
    // are either all 0x00 (D70) or 0xFF (D2H).
    // We therefore switch these values with the default values.
    if (data->m_min_x==1.0) {
	data->m_min_x = 0.0;
	DEBUG_PRINT("DEBUG: NEF X MIN -> %e (changed)\n",data->m_min_x);
    }
    if (data->m_max_x==0.0) {
	data->m_max_x = 1.0;
	DEBUG_PRINT("DEBUG: NEF X MAX -> %e (changed)\n",data->m_max_x);
    }
    if (data->m_min_y==1.0) {
	data->m_min_y = 0.0;
	DEBUG_PRINT("DEBUG: NEF Y MIN -> %e (changed)\n",data->m_min_y);
    }
    if (data->m_max_y==0.0) {
	data->m_max_y = 1.0;
	DEBUG_PRINT("DEBUG: NEF Y MAX -> %e (changed)\n",data->m_max_y);
    }
    if (data->m_gamma==0.0 || data->m_gamma==255.0+255.0/256.0) {
	data->m_gamma = 1.0;
	DEBUG_PRINT("DEBUG: NEF GAMMA -> %e (changed)\n",data->m_gamma);
    }

    //get number of anchor points (there should be at least 2
    nc_fread(&data->m_numAnchors,1,1,file);
    DEBUG_PRINT("DEBUG: NEF NUMBER OF ANCHORS -> %u\n",data->m_numAnchors);
    if (data->m_numAnchors==255) {
	data->m_numAnchors = 0;
	DEBUG_PRINT("DEBUG: NEF NUMBER OF ANCHORS -> %u (changed)\n",
		data->m_numAnchors);
    }
    if (data->m_numAnchors > NIKON_MAX_ANCHORS) {
	data->m_numAnchors = NIKON_MAX_ANCHORS;
	DEBUG_PRINT("DEBUG: NEF NUMBER OF ANCHORS -> %u (changed)\n",
		data->m_numAnchors);
    }

    //convert data to doubles
    for(i = 0; i < data->m_numAnchors; i++)
    {
	//get anchor points
	data->m_anchors[i].x = (double)fgetc(file)/255.0;
	data->m_anchors[i].y = (double)fgetc(file)/255.0;
    }

    //The total number of points possible is 25 (50 bytes).
    //At this point we subtract the number of bytes read for points from the max (50+1)
    fseek(file,(51 - data->m_numAnchors*2),SEEK_CUR);

    //get data (always 4096 entries, 1 byte apiece)
    DEBUG_PRINT("DEBUG: NEF data OFFSET -> %ld\n",ftell(file));

    if (sample_p!=NULL)
    {
	// Sampling res is always 4096, and output res is alway 256
	*sample_p = CurveSampleInit(4096, 256);

	//get the samples
	for(i = 0; i < 4096; i++)
	{
	    (*sample_p)->m_Samples[i] = (unsigned int)fgetc(file);
	}
    }

    return NC_SUCCESS;
}

/*******************************
main:
    Uh....no comment. :)
********************************/
#ifdef _STAND_ALONE_

int main(int argc, char* argv[])
{
    //make sure we can continue processing
    if (ProcessArgs(argc,argv) == NC_SUCCESS)
    {

	//if we are in NEF mode, rip the curve out of the RAW file
	if (program_mode == NEF_MODE)
	{
	    NikonData data;

	    //intiialze the structure to zero
	    memset(&data,0,sizeof(NikonData));

	    if (RipNikonNEFData(nikonFilename, &data.curves[TONE_CURVE], NULL)
	            != NC_SUCCESS)
	    {
	        return NC_ERROR;
	    }

	    CurveSample *sample = CurveSampleInit(65536, 256);

	    if (CurveDataSample(&data.curves[TONE_CURVE], sample)
	            != NC_SUCCESS)
	    {
		CurveSampleFree(sample);
	        return NC_ERROR;
	    }

	    if (SaveSampledNikonCurve(sample, exportFilename) != NC_SUCCESS)
	    {
		CurveSampleFree(sample);
	        return NC_ERROR;
	    }

	    if (SaveNikonCurveFile(&data.curves[TONE_CURVE], TONE_CURVE,
				"outcurve.ncv",NCV_FILE, NIKON_VERSION_4_1))
	    {
		CurveSampleFree(sample);
	        return NC_ERROR;
	    }

	    //This can also be used
	    if (SaveNikonDataFile(&data,"outcurve2.ncv",NCV_FILE, NIKON_VERSION_4_1))
	    {
		CurveSampleFree(sample);
	        return NC_ERROR;
	    }

	    CurveSampleFree(sample);
	}
	//else, process a nikon curve file
	else
	{
	    //do the deed
	    ConvertNikonCurveData(nikonFilename,exportFilename,
	        standalone_samplingRes, standalone_outputRes);
	}
    }
    return NC_SUCCESS;
}
#endif
#undef g_fopen
#endif
