//------------------------------------------------------------------------------
// File : stopwatch.cpp
//------------------------------------------------------------------------------
// GLVU : Copyright 1997 - 2002 
//        The University of North Carolina at Chapel Hill
//------------------------------------------------------------------------------
// Permission to use, copy, modify, distribute and sell this software and its 
// documentation for any purpose is hereby granted without fee, provided that 
// the above copyright notice appear in all copies and that both that copyright 
// notice and this permission notice appear in supporting documentation. 
// Binaries may be compiled with this software without any royalties or 
// restrictions. 
//
// The University of North Carolina at Chapel Hill makes no representations 
// about the suitability of this software for any purpose. It is provided 
// "as is" without express or implied warranty.

//------------------------------------------------------------------------------
// Stopwatch -- a cross platform performance timing class 
// $Id: Stopwatch.cxx,v 1.1 2007/09/24 21:03:51 cquammen Exp $
//------------------------------------------------------------------------------

#ifdef TIME

#include <stdio.h>  // for sprintf on some platforms
#include "Stopwatch.h"
#include <iostream>

#ifdef _WIN32
#  define  WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

//------------------------------------------------------------------------------
// Base Class
//------------------------------------------------------------------------------
static char baseType[] = "Stopwatch virtual base class";

/**
 * Constructor.  Stopwatches are stopped at construction time.
 * Call Start() to make it start ticking.
 * @param name   A name to set on the timer.  If no name is supplied the name
 *               "Stopwatch" will be used.
 * @see SetName
 */
StopwatchBase::StopwatchBase(const char *name)
  : elapsedTime(0.0f), numStarts(0), running(false), sw_type(baseType)
{
  if (name==0) name = "Stopwatch";
  SetName(name);
}

/**
 * Destructor.  
 */
StopwatchBase::~StopwatchBase()
{
}

#if 0
void StopwatchBase::Start()
{
  if (!running) {
    markTime();
    running = true;
  }
  numStarts++;
}

void StopwatchBase::Stop()
{
  if (running) {
    elapsedTime += diffTime();
    running = false;
  }
}

void StopwatchBase::Reset()
{
  if (running) { markTime(); numStarts=1; }
  else numStarts = 0;
  elapsedTime = 0.0f;
}

float StopwatchBase::GetTime() const
{
  if (running) return (elapsedTime + diffTime());
  else return (elapsedTime);
}
#endif 

/**
 * Return the average elapsed time over a number of starts.
 * Equals the total elapsed time over the number of starts since the last 
 * Reset() or since construction.  
 *
 * @warning Be sure to stop the stopwatch with Stop() before calling this.
 *          Otherwise the results will be meaningless.
 */
float StopwatchBase::GetAvgTime() const
{
  if (!numStarts) return 0;

  // If they call GetAvgTime while the clock is running there's 
  // no telling what they'll get since they'll be averaging in one
  // incomplete period
  if (running) return (elapsedTime+diffTime()/numStarts);
  else return  elapsedTime/numStarts;
}

/**
 * Return the number of calls to Start() since construction or 
 * since the last Reset()
 */
int StopwatchBase::GetNumStarts() const
{
  return numStarts;
}

/**
 * Sets a name on the stopwatch that can be used to identify the 
 * timer for debugging printout purposes.  Used in the output format 
 * defined by the ostream << operator.
 * @see SetName(int)
 */
void StopwatchBase::SetName(const char *s)
{
  // Copy s into our name buffer
  char *d = sw_name;
  while ((*d++ = *s++) && ((d-sw_name)<STOPWATCH_MAX_NAME));

  // Null terminate if necessary
  if (d == sw_name+STOPWATCH_MAX_NAME) sw_name[STOPWATCH_MAX_NAME-1]='\0';
}

/**
 * Sets a name on the stopwatch that can be used to identify the 
 * timer for debugging printout purposes.  Used in the output format 
 * defined by the ostream << operator.  This version converts the interger
 * argument to a string and uses that as the name.
 * @see SetName(const char*)
 */
void StopwatchBase::SetName(int id)
{
  sprintf(sw_name, "%d", id);
}

/**
 * Return the identifier set using SetName()
 */
const char* StopwatchBase::GetName() const
{
  return sw_name;
}

/**
 * Return the type of the timer.  This is a descriptive string
 * that identifies the category of timer (platform and implemnentation 
 * type).
 */
const char* StopwatchBase::GetType() const
{
  return sw_type;
}

//------------------------------------------------------------------------------
// Generic Wall Clock Implementation -- works just about everywhere
//------------------------------------------------------------------------------

static char genericType[] = "Generic wall clock (ftime())";
StopwatchGeneric::StopwatchGeneric(const char *name)
  : StopwatchBase(name)
{
  sw_type = genericType;
}

StopwatchGeneric::~StopwatchGeneric()
{
}

void StopwatchGeneric::markTime()
{
  ftime(&lastStamp);
}

float StopwatchGeneric::diffTime() const
{
  struct timeb newStamp;
  ftime(&newStamp);
  return (float)(newStamp.time - lastStamp.time)
    + ((float)(newStamp.millitm - lastStamp.millitm))/1000.0f;
}



//------------------------------------------------------------------------------
// Gettimeofday Wall Clock Implementation -- works on most unix flavors
//------------------------------------------------------------------------------

#ifndef _WIN32
static char gtodType[] = "UNIX wall clock (gettimeofday())";
StopwatchGTOD::StopwatchGTOD(const char *name)
  : StopwatchBase(name)
{
  sw_type = gtodType;
}

StopwatchGTOD::~StopwatchGTOD()
{
}

void StopwatchGTOD::markTime()
{
  gettimeofday(&lastStamp, NULL);
}

float StopwatchGTOD::diffTime() const
{
  struct timeval newStamp;
  gettimeofday(&newStamp, NULL);
  return
    (((double)newStamp.tv_sec) + ((double)newStamp.tv_usec / 1e6f)) 
    - (((double)lastStamp.tv_sec) + ((double)lastStamp.tv_usec / 1e6f));
}
#endif /* !_WIN32 */

//------------------------------------------------------------------------------
// SuperFast Hardware SGI Wall Clock Implementation -- SGI Only (duh)
//------------------------------------------------------------------------------
#ifndef _WIN32
#ifdef CLOCK_SGI_CYCLE
static char sgiType[] = "SGI hardware wall clock (clock_gettime(CLOCK_SGI_CYCLE))";
StopwatchSGI::StopwatchSGI(const char *name)
  : StopwatchBase(name)
{
  sw_type = sgiType;
}

StopwatchSGI::~StopwatchSGI()
{
}

void StopwatchSGI::markTime()
{
  clock_gettime(CLOCK_SGI_CYCLE,&lastStamp);
}

float StopwatchSGI::diffTime() const
{
  timespec_t newStamp;
  clock_gettime(CLOCK_SGI_CYCLE,&newStamp);
  return
    (((double)newStamp.tv_sec) + ((double)newStamp.tv_nsec / 1e9)) 
    - (((double)lastStamp.tv_sec) + ((double)lastStamp.tv_nsec / 1e9));
}
#endif /* CLOCK_SGI_CYCLE */
#endif /* !_WIN32 */



#ifdef _WIN32
//------------------------------------------------------------------------------
// Win32 QueryPerformanceCounter Wall Clock Implementation
//------------------------------------------------------------------------------

static char win32Type[] = "Win32 wall clock (QueryPerformanceCounter())";
StopLargeInteger StopwatchWin32::clockFreq;
bool StopwatchWin32::clockFreqSet = false;
StopwatchWin32::StopwatchWin32(const char *name)
  : StopwatchBase(name)
{
  sw_type = win32Type;
  if (!clockFreqSet) {
    QueryPerformanceFrequency((LARGE_INTEGER*)&clockFreq);
    clockFreqSet = true;
  }
}

StopwatchWin32::~StopwatchWin32()
{
}

void StopwatchWin32::markTime()
{
  QueryPerformanceCounter((LARGE_INTEGER*)&lastStamp);
}

float StopwatchWin32::diffTime() const
{
  LARGE_INTEGER newStamp;
  QueryPerformanceCounter(&newStamp);
  return (float)((newStamp.QuadPart - lastStamp.QuadPart)/(double)clockFreq.QuadPart);
}
#endif /* _WIN32 */



//------------------------------------------------------------------------------
// Generic CPU Clock Implementation
//------------------------------------------------------------------------------
static char genericCPUType[] = "Generic CPU clock (clock())";
CPUStopwatchGeneric::CPUStopwatchGeneric(const char *name)
  : StopwatchBase(name)
{
  sw_type = genericCPUType;
}

CPUStopwatchGeneric::~CPUStopwatchGeneric()
{
}

void CPUStopwatchGeneric::markTime()
{
  lastStamp = clock();
}

float CPUStopwatchGeneric::diffTime() const
{
  return (clock()-lastStamp)/float(CLOCKS_PER_SEC);
}

#endif // TIME
