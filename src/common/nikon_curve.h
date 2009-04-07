/***************************************************
 nikon_curve.h - read Nikon NTC/NCV files

 Copyright 2004-2008 by Shawn Freeman, Udi Fuchs

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

****************************************************/

/***************************************************

  This program reads in a Nikon NTC/NCV file, 
  interperates it's tone curve, and writes out a
  simple ascii file containing a table of interpolation
  values.

  You'll note that this has been written in way that can be used in a standalone program or
  incorporated into another program. You can use the functions seperately or
  you can just call ConvertNikonCurveData with an input and output file.

  I've tried to document the code as clearly as possible. Let me know if you 
  have any problems!

  Thanks goes out to Udi Fuchs for wanting to incorporate nikon curve loading into his program.
  This will make GIMP just that much better. :)

  NOTES:
  08/06/2005 (Shawn Freeman)
    The last endian fix would not have worked correctly. The new fix should correct the
    endianess problem.

    The fix uses a new function DetermineEndianess to assign function pointers the correct
    swapping function based on the endianess of the machine.

    For programs that use this code on big endian machines, it is important that this function
    be called. So for now, each function that reads or writes data to a file will call this
    function.


  07/27/2005 (Shawn Freeman)
	Someone discovered the dreaded endianess bug. NTC and NCV files are written in little-endian
	format, so when a Mac (or other big endian machine) tries to read in the file it blows up.

	There doesn't appear to be a way to determine the endianess of a machine a runtime (at
	least not in an ANSI compliant way). The solution? Custom read and write functions that
	can handle the endianess.

	The ENDIANESS define can be set to either LITTLE_ENDIAN (compiles code for little endian
	machines), BIG_ENDIAN (compiles code for big endian machines), or UNKNOWN_ENDIAN (compiles
	code that dynamically determines endianess and runs appropriate code).

	Performance isn't much of an issues since files are small, but it is more efficient
	to compile with the endianess set than to leave it at unknown.

	Unfortunately, I don't have a way to test this code. But it should do the trick.

  06/20/2005 (Udi Fuchs)
	Added the function CurveDataReset()

  06/14/2005 (Udi Fuchs)
	Added the function CurveDataSetPoint()

  06/13/2005 (Shawn Freeman)
    Added a couple of explicit casts to keep some compilers from complaining.
    At this point, I'd say this code is decent enough to classified as 1.0. Right now
    there are no outstanding bugs left that I am aware of. The code has been fairly well
    documented and has a significant amount of error checking built in.

    There are still some experimental code in here that could be removed, along with options
    for compiling on differnt compilers.

    I don't have as much time as I used to have, due to a new baby and everything.But
    I should still have time to do some tweaks, features, and fixes if necessary.

  Enjoy!

  06/10/2005 (Udi Fuchs)
	Removed the name "Nikon" from general structures/functions
	NikonAnchorPoint -> CurveAnchorPoint
	NikonCurve -> CurveData
	SampleNikonCurve() -> CurveDataSample()
	FreeNikonCurve() -> CurveSampleFree()
	Moved the sampling data to the CurveSample structure, now nikon_curve
	and UFRaw can use the sampel CurveData structure.
	Removed unused functions RemoveRedundantData(), ResampleNEFCurve()
	Curve sampling changed - the curve before first point and after
	the last point is flattened.
	
  04/16/2005 Removed malloc calls and replaced them with fixed arrays. The Nikon camera
	    does not support any more than 20 anchors for their curve. This speeds up the
	    code a little bit.

	    This version also supports the latest version of the NTC/NCV file format.

  03/18/2005 The last remaining incompatibility between MSVC and gcc: 
	        
	          #define vsnprintf _vsnprintf

	    Other Fixes:
	    1. Fixed a bug that was incorrectly writing out minimum y-axis values for color curves.

  03/12/2005 I found some additional bugs when writing out ntc/ncv files that would cause the Nikon
	    software to crash. The problem was two-fold.

	    First, all four curves in the file must contain some information. The SaveNikonCurveFile now
	    checks to make sure that a curve has good data. If it doesn't, then defaults are written in their
	    place.

	    Why would a curve contain incorrect information? If you're only working with a tone curve in a NikonData
	    structure, the color curves would not contain any information. The save function automatically corrects
	    this.

	    Second, the version number. Right now, the only version this code supports is Nikon 4.1. So while the
	    save function takes the version as a paramter, internally it is ignored and forced to 4.1.
	    
	    A second version of the SaveNikonCurveFile function now takes a single curve as an argument and uses
	    defaults for the rest of the curves in the file. The function allows you to specify which curve your
	    saving (TONE_CURVE, RED_CURVE, GREEN_CURVE, or BLUE_CURVE). Typically, the curve would be tone.


  03/11/2005    Udi added some debugging code for use with UFRaw. Unfortunately, the debugging code uses
	    macros with variable arguments, which are not supported by all compilers (namely MSVC).
	    I added the following:
	    
	      __MSVC__        : Define this if you are compiling with MSVC. This enables code that allows
				the program to still print out debug information, though not quite as robust
				as with using gcc.

	    __WITH_UFRAW__    : Define this if this code is compiling with UFRaw, This allows for connecting
				out to the UFRaw error handler.

	    In regards to these flags, the code has been changed around a little bit. I also moved some code
	    to more "appropriate" locations, in keeping with the layout of these files.

	    I also renamed the SaveNikonCurve function to SaveNikonCurveFile for clarity. I also fixed a bug
	    in the function to handle NikonData structures with only one used curve. This allows extracting
	    the curve from an NEF file and saving it to an NTC/NCV file. This would also allow a curve editor
	    to write out files successfully, even if they only use tone curves.

  03/05/2005 Added a fix to allow SampleNikonCurve to be called more than once so that a programmer
	    can resample the curve. This would be used when extracting a curve from an NEF file
	    and rebuilding the NTC/NCV file curve from the data.

	    Also added a fix that allows for curves with no anchors, just box coordinates.

	    The ResampleCurve function has been renamed to ResampleNEFCurve. This is to avoid confusion.
	    THIS FUNCTION SHOULD NOT BE USED FOR STANDARD NTC/NCV/NEF CURVE GENERATION. This function is
	    only useful for building a smooth curve from the 4096 byte tone curve table in an NEF file.
	    This function may end up going away if it's not found useful.

  03/04/2005 Added a new function called ResampleCurve that allows the programmer to resample curves
	    that are contained in a NikonCurve structure. A cubic spline interpolator is used in the
	    resampling for the smoothest possible curve. This functions was added at the request of
	    Udi Fuchs to allow UFRaw to read in and resample curves stored in NEF files.

	    A word of caution when using this function. While the function stores all of it's results
	    in the passed in curve structure, those results may not be compatible with the camera and/or
	    the NTC/NCV files. Specifically, this function will most likely contain anchor counts much
	    greater than 20, which is the maximum. It is NOT RECOMMENDED that you try and save out to 
	    NTC/NCV format after running this function unless you're absolutely sure you know what you
	    are doing.

	    With that out of the way, this function is most useful to create a smoother curve than what 
	    comes from the camera and the NTC/NCV curve files.

  02/09/2005 Found a fixed a subtle, yet glaring bug in the curve sampling functions. The sampling
	    functions were not obeying the limits set in the ncv/ntc files. Loading one of these
	    files that contained any start and end points other than 0 and 1 would result in
	    the code sampling from the extrapolated parts of the spline curve instead of the
	    interpolated curve. This actually resulted in a crash in UFRaw, since it was returning
	    huge numbers as a result of signed to unsigned conversion.

  02/06/2005    Finally figured out the mathematical relationship (or a close approximation) between
	    the Nikon curves and curves imbedded in the NEF files. General form is:

	        x = a*x + b*ln(cx+1)/ln(dx+2)

	    I've found this gives a very good match for the curves. The new function to use to get
	    this capability is SampleToCameraCurve. This does a standard curve sample then runs the
	    value through the above transform. Quite useful if you want to force-embed a particular
	    curve into a batch of NEF files.


  02/01/2005 Fixed some issues to better operate with UFRaw. Removed macros and replaced with
	    with byte reading and shifting (to avoid including windows crud).

	    The function RipNEFCurve now uses the standard Nikon Curve structure. The NEFData
	    structure is now obsolete, along with the save and free functions (Removed).
	    
  01/29/2005 Added support for getting the tone curve data out of an NEF file. This will generate
	    a file similar to the other curve data files generated by this progrom in standalone
	    mode.

	    A new command line option has been added to specify NEF processing (-nef filename). The
	    NEF capabilities have only been tested with Nikon D70 NEF's.

  01/14/2005    Fixed some bugs. The static data declarations in the header were not declared
	    static, resulting in errors when trying to compile into another program.

	    Added a function to remove redundant calculated curve data and placing it in a 
	    an allocated array. This should help facilitate a no diska access interface to UFRaw.

	    Tweaked a few lines that were either not compiling or had warnings when compiled with
	    gcc.

	    Changed the unsigned shorts to unsigned ints just for the sake of it. May be useful if
	    anyone wants to do bor than 16 bits of sampling. Also means that the program uses a 
	    bit more memory when processing.

  01/11/2005 Merged changes from Udi. Added the ability to set both sampling resolution and
	    output resolution in LoadNikonData. For example, you can set the sampling resolution
	    to 65536 and output resolution to 256.

	    I also added a #define (_STAND_ALONE_) for compiling as a standalone program.Undef
	    if you just want to use the functions.

	    Additional command line options are avalable:
	    -sr    Specifies sampling resolution
	    -or Specifies output resolution
	    
	    Some "how to use" info comes up if there are no arguments on the command line.
	    If these aren't specified, defaults are used. This only applies to standalone.

  01/08/2005 Removed all code relating to based on Numerical Recipes since it requires
	    a soul-sucking liscense. The spline calculation code has been replaced by
	    the freely available code from John Bukardt.I only included the functions from
	    the library that were necessary to avoid bloat. The code has also been converted
	    to using standard C. The original code is available from:

	    http://www.csit.fsu.edu/~burkardt/cpp_src/spline/spline.html

	    New Features:
	    1. Heavy error checking, with verbose output.
	    2. Supports writing out to NTC and NCV files.
	    2. In debug mode, lots of information is displayed to the console.
	    3. Supports exporting Tone and RGB curves from NCV and NTC files.
	    4. Removed non-ANSI functions, data declarations etc.
	    4. Alot of tweaks.
	    5. Alot more testing ;)

	    The standalone prorgam outputs 4 files, each ending in the name of the curve data
	    it contains. If you use this like a library, you can make it do whatever you want.

  01/06/2005 This is version .01. The gamma calculation might not match
	    what Nikon's Capture does. It only supports NTC files right now.
	    NCV format will be supported shortly, but will not impact
	    the structure of this "library". Other additional functionality
	    will be to support the color curve sections of NTC/NCV files.

  @author: Shawn Freeman 1/06/2005
  @liscense: GNU GPL
****************************************************/
#ifndef _NIKON_CURVE_H
#define _NIKON_CURVE_H

#include <stdio.h>

#define NC_VERSION "1.2"
#define NC_DATE "2005-08-06"

//////////////////////////////////////////
//COMPILER CONTROLS
//////////////////////////////////////////
//Undef this if you don't want to use the stand-alone program
//This is mainly for debugging purposes
//#define _STAND_ALONE_

//Define this if you are using Microsoft Visual C++. This enables code to
//deal with the fact the MSVC does not support variable argument macros.
//#define __MSVC__

//the only remaining incompatibility between MSVC and gcc
#ifdef __MSVC__
    #define vsnprintf _vsnprintf
#endif

//Define this if using with UFRaw
//#define __WITH_UFRAW__

//Flags used to determine what file we're trying to process.
//Should only be used in standalone mode.
#ifdef _STAND_ALONE_

#define CURVE_MODE  0
#define NEF_MODE    1

#endif


/*******************************************************************************
Information regarding the file format.

Section Headers: 

Order of Box Data: Left x, Right x, Midpoint x (gamma), Bottom y, Top y

Order of Anchor Data: Start x, Start y, Anchor 1 x, Anchor 1 y, ... , End x, End y

Anchor Point Data: This is aligned on 8 byte boundries. However, the section must
	            end on a 16 byte boundary, which means an 8 byte pad is added.
********************************************************************************/

//DEFINES FOR WRITING OUT DATA (for ntc/ncv files)
#define NCV_HEADER_SIZE		    0x3E    //Combined header length for an NCV file
#define NCV_SECOND_FILE_SIZE_OFFSET 0x3F    //4 bytes (int). File size - NCV_header
#define NCV_UNKNOWN_HEADER_DATA	    0x002    //2 bytes. (?)
#define NCV_SECOND_HEADER_LENGTH    23
#define NCV_FILE_TERMINATOR_LENGTH  23

#define NTC_FILE_HEADER_LENGTH	    0x10    //16 bytes. Doesn't change
//This seemed to change when Nikon released an updated capture program
//from 4.1 to 4.2. This may be an int but not sure.
#define NCV_PATCH_OFFSET            0x3D    //2 bytes(?)
#define NTC_PATCH_OFFSET            0x10    //2 bytes(?)
#define FILE_SIZE_OFFSET            0x12    //4 bytes (int). File size - header.
#define NTC_VERSION_OFFSET          0x16    //4 bytes (int).(?)
					    //9 byte pad(?)
					    //16 bytes. Another section header goes here. 

//From here down repeats for every section
#define NTC_SECTION_TYPE_OFFSET	    0x00    //4 bytes (int) (0,1,2,3)

#define NTC_UNKNOWN		    0x05    //2 bytes. Doesn't change but not sure what they do (0x03ff)
#define NTC_UNKNOWN_DATA            0x3FF    //

#define NTC_RED_COMPONENT_OFFSET    0x08    //4 bytes (int) (0-255)
#define NTC_GREEN_COMPONENT_OFFSET  0x0C    //4 bytes (int) (0-255)
#define NTC_BLUE_COMPONENT_OFFSET   0x0F    //4 bytes (int) (0-255)
					    //12 byte pad all zeros(align to 16?)

#define NTC_RED_WEIGHT_OFFSET	    0x1F    //4 bytes (int) (0-255)
#define NTC_GREEN_WEIGHT_OFFSET	    0x23    //4 bytes (int)    (0-255)
#define NTC_BLUE_WEIGHT_OFFSET	    0x27    //4 bytes (int)    (0-255)

#define END_ANCHOR_DATA_PAD_LENGTH  0x08    //Always all zeros    
#define NTC_SECTION_HEADER_LENGTH   0x10    //Doesn't change        


//DEFINES FOR READING IN DATA
#define HEADER_SIZE		    0x10    //First characters may be unicode Japanese?

#define NTC_BOX_DATA		    0x5C    //Start of box data
#define NTC_NUM_ANCHOR_POINTS	    0x84    //Number of anchor points plus 2 for start and end points
#define NTC_ANCHOR_DATA_START	    0x88    //Beginning of anchor point data

#define NCV_BOX_DATA		    0x89    //Start of box data
#define NCV_NUM_ANCHOR_POINTS	    0xB2    //Number of anchor points plus 2 for start and end points
#define NCV_ANCHOR_DATA_START	    0xB5    //Beginning of anchor point data

//array indices to retrive data
#define PATCH_DATA	    0
#define BOX_DATA	    1
#define NUM_ANCHOR_POINTS   2
#define ANCHOR_DATA	    3

//Some data sections sizes for calculating offsets
#define NEXT_SECTION_BOX_DATA_OFFSET	0x43    //after the anchor data, this is the offset to 
						//the beginning of the next section's box data

#define NUM_POINTS_TO_ANCHOR_OFFSET	0x03    //number of bytes from the number of anchor points
						//byte to the start of anchor data.
//Nikon version defines
#define NIKON_VERSION_4_1   0x00000401
#define NIKON_PATCH_4       0x04ff
#define NIKON_PATCH_5       0x05ff
#define NIKON_MAX_ANCHORS   20

//file types
#define NTC_FILE        0
#define NCV_FILE        1
#define NUM_FILE_TYPES  2

//Curve Types
#define TONE_CURVE      0
#define RED_CURVE       1
#define GREEN_CURVE     2
#define BLUE_CURVE      3
#define NUM_CURVE_TYPES 4

//Maximum resoltuion allowed due to space considerations.
#define MAX_RESOLUTION    65536

//////////////////////////////
//NEF/TIFF MACROS AND DEFINES
//////////////////////////////
#define TIFF_TAG_EXIF_OFFSET        34665
#define TIFF_TAG_MAKER_NOTE_OFFSET  37500
#define TIFF_TAG_CURVE_OFFSET       140

#define TIFF_TYPE_UNDEFINED         7
#define TIFF_TYPE_LONG		    4


////////////////////////
////////////////////////
//ERROR HANDLING
////////////////////////
////////////////////////
#define NC_SUCCESS	0
#define NC_ERROR	100
#define NC_WARNING	104
#define NC_SET_ERROR	200


//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
//DATA STRUCTURES
//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

/**********************************************************
CurveData:
    Structure for the curve data inside a NTC/NCV file.
***********************************************************/
typedef struct
{
    double x;
    double y;
} CurveAnchorPoint;

typedef struct 
{
    char name[80];

    //Type for this curve
    unsigned int m_curveType;
    
    //Box data
    double m_min_x;
    double m_max_x;
    double m_min_y;
    double m_max_y;
    double m_gamma;

    //Number of anchor points
    unsigned char m_numAnchors;
    
    //contains a list of anchors, 2 doubles per each point, x-y format
    //max is 20 points
    CurveAnchorPoint m_anchors[NIKON_MAX_ANCHORS];
    
} CurveData;

typedef struct 
{
    //Number of samples to use for the curve.
    unsigned int m_samplingRes;
    unsigned int m_outputRes;

    //Sampling array
    unsigned short int *m_Samples; // jo: changed to short int to save memory

} CurveSample;


/********************************************
NikonPoint:
    Simple point structure. Used for storing 
reduced data from a sampled curve.
*********************************************/
typedef struct
{
    unsigned int x;
    unsigned int y;
} NikonPoint;

/*********************************************
NikonData:
    Overall data structure for Nikon file data
**********************************************/
typedef struct
{
    //Number of output points
    int m_fileType;
    unsigned short m_patch_version;
    CurveData curves[4];
} NikonData;

//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////
//FUNCTIONS
//////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////

/************************************************************
nc_message:
    The Nikon Curve message handler. Udi Fuchs created this
to make the error handling consistent across the code. 

  code - Message code
  format - The message to format
  ... - variable arguments
**************************************************************/

void nc_message(int code, char *format, ...);
void DEBUG_PRINT(char *format, ...);

/*******************************************************************
 d3_np_fs:
   Helper function for calculating and storing tridiagnol matrices.
   Cubic spline calculations involve these types of matrices.
*********************************************************************/
double *d3_np_fs ( int n, double a[], double b[] );

/*******************************************************************
 spline_cubic_set:
   spline_cubic_set gets the second derivatives for the curve to be used in
   spline construction

    n = number of control points
    t[] = x array
    y[] = y array
    ibcbeg = initial point condition (see function notes).
    ybcbeg = beginning value depending on above flag
    ibcend = end point condition (see function notes).
    ybcend = end value depending on above flag
  
    returns the y value at the given tval point
*********************************************************************/
double *spline_cubic_set ( int n, double t[], double y[], int ibcbeg,
    double ybcbeg, int ibcend, double ybcend );
/*******************************************************************
 spline_cubic_val:
   spline_cubic_val gets a value from spline curve.

    n = number of control points
    t[] = x array
    tval = x value you're requesting the data for, can be anywhere on the interval.
    y[] = y array
    ypp[] = second derivative array calculated by spline_cubic_set
    ypval = first derivative value of requested point
    yppval = second derivative value of requested point
  
    returns the y value at the given tval point
*********************************************************************/
double spline_cubic_val ( int n, double t[], double tval, double y[],
    double ypp[], double *ypval, double *yppval );


/*********************************************
LoadNikonData:
    Loads a curve from a Nikon ntc or ncv file.
    
    fileName    - The filename.
    curve        - Pointer to curve struct to hold the data.
    resolution    - How many data points to sample from the curve
**********************************************/
int LoadNikonData(char *fileName, NikonData *data);

/*********************************************
CurveDataSample:
    Samples from a spline curve constructed from
    the curve data.
    
    curve   - Pointer to curve struct to hold the data.
    sample  - Pointer to sample struct to hold the data.
**********************************************/
int CurveDataSample(CurveData *curve, CurveSample *sample);

/*********************************************
 * CurveDataReset:
 *     Reset curve to straight line but don't touch the curve name.
 **********************************************/
void CurveDataReset(CurveData *curve);

/*********************************************
 * CurveDataIsTrivial:
 *     Check if the curve is a trivial linear curve.
 ***********************************************/
int CurveDataIsTrivial(CurveData *curve);

/*********************************************
 CurveDataSetPoint:
    Change the position of point to the new (x,y) coordinate.
    The end-points get a special treatment. When these are moved all the
    other points are moved together, keeping their relative position constant.
**********************************************/
void CurveDataSetPoint(CurveData *curve, int point, double x, double y);

/****************************************************
SampleToCameraCurve:
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
int SampleToCameraCurve(CurveData *curve, CurveSample *sample);

/************************************************************
SaveNikonDataFile:
    Savess a curve to a Nikon ntc or ncv file.
    
    data        - A NikonData structure containing info of all the curves.
    fileName    - The filename.
    filetype    - Indicator for an NCV or NTC file.
    version        - The version of the Nikon file to write

NOTE:    The only version tested is Nikon 4.1 anything
	other than this may result in unpredictable behavior.
	For now, the version passed in is ignored and is forced
	to 4.1.
**************************************************************/
int SaveNikonDataFile(NikonData *data, char *outfile, int filetype, int version);

/************************************************************
SaveNikonCurveFile:
    Saves out curves to a Nikon ntc or ncv file. This function
    takes a single curve and uses defaults for the other curves.
    Typically, the curve used is the tone curve.
    
    curve        - A NikonCurve structure. This is usually the tone curve
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
	int filetype, int version);

/*********************************************
SaveSampledNikonCurve:
    Saves a sampling from a curve to text file to
    be processed by UFRaw.

    sample        - Pointer to the sampled curve.
    fileName    - The filename.
**********************************************/
int SaveSampledNikonCurve(CurveSample *sample, char *outfile);

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
int FindTIFFOffset(FILE *file, unsigned short num_entries, unsigned short tiff_tag,
	            unsigned short tiff_type);

/*******************************************************
RipNikonNEFData:
    Gets Nikon NEF data. For now, this is just the tone
    curve data. This is more of a helper function for running
    in stand-alone. This function basically finds the correct
    file offset, and then calls RipNikonNEFCurve.

    infile - The input file
    curve  - data structure to hold data in.
    sample_p -  pointer to the curve sample reference.
		can be NULL if curve sample is not needed.
********************************************************/
int RipNikonNEFData(char *infile, CurveData *data, CurveSample **sample_p);

/*******************************************************
RipNikonNEFCurve:
    The actual retriever for the curve data from the NEF
    file.
    
    file   -	The input file.
    infile -	Offset to retrieve the data
    curve  -	data structure to hold curve in.
    sample_p -  pointer to the curve sample reference.
		can be NULL if curve sample is not needed.
********************************************************/
int RipNikonNEFCurve(FILE *file, int offset, CurveData *data,
	CurveSample **sample_p);

/*******************************************************
CurveSampleInit:
    Init and allocate curve sample.
********************************************************/
CurveSample *CurveSampleInit(unsigned int samplingRes, unsigned int outputRes);

/*******************************************************
CurveSampleFree:
    Frees memory allocated for this curve sample.
********************************************************/
int CurveSampleFree(CurveSample *sample);

/****************************************
ConvertNikonCurveData:
    The main driver. Takes a filename and
    processes the curve, if possible.

    fileName    -    The file to process.
*****************************************/
int ConvertNikonCurveData(char *inFileName, char *outFileName, unsigned int samplingRes, unsigned int outputRes);

#endif


