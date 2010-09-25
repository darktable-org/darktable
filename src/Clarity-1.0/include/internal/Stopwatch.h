//------------------------------------------------------------------------------
// File : stopwatch.hpp
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
// $Id: Stopwatch.h,v 1.1 2008/09/11 14:56:35 cquammen Exp $
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
//  The interface is as follows:
//   The first call to Start() begins the timer counting.
//   Stop() stops the clock.
//   Reset() clears the elapsed time to zero.
//   The Stopwatch can be stopped and restarted.  Or even restarted without
//   stopping.  This is useful for timing several periods of an event to get
//   an average.  Everytime Start() is called an internal counter is 
//   incremented.  The GetAvgTime() method reports the total elapsed time
//   divided by the number of starts, while GetTime() just reports the 
//   elapsed time.
//
//   The Stopwatch class measures wall clock time.
//   The CPUStopwatch measures CPU time.  
//
//   Both will hopefully be implemented with best possible precision on
//   your platform.
//
//   Watch out for GetTime() and GetAvgTime().  They both report time in terms 
//   of the most recent Stop() or re- Start()
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// This has been tested on the following systems:
//   OS                                   (COMPILER)
//   --                                   ----------
//   HP-UX B.10.20 A 9000/735 2006086919  (gcc 2.7.2.2)
//   IRIX64 evans 6.5 11051731 IP27 (CC MipsPro 7.2.1 and gcc 2.7.2.1)
//   SunOS 5.5.1 Generic_103640-24 sun4u sparc SUNW,Ultra-2 (gcc 2.7.2)
//   Windows NT 4.0/2000 (MSVC 5.0/6.0)
//------------------------------------------------------------------------------
#ifndef GLVU_STOPWATCH_INCLUDED
#define GLVU_STOPWATCH_INCLUDED

#include <time.h>            // FOR clock and clock_gettime CALL
#include <sys/timeb.h>       // FOR ftime CALL
#ifndef _WIN32
#include <sys/time.h>        // FOR gettimeofday CALL
#endif /* !_WIN32 */

#define STOPWATCH_MAX_NAME 40

/**
 * @class StopwatchBase
 * @brief Abstract base class for wall-clock Stopwatch and CPU-timing CPUStopwatch. 
 *
 * You don't ever instantiate a StopwatchBase, rather you instantiate either
 * a #Stopwatch or a #CPUStopwatch.  Those are typedefs for the appropriate
 * platform-specific flavor of timer.
 */

/**
 * @class Stopwatch
 * @brief A multiplatform wall-clock timer class
 *
 * Stopwatch has various underlying implementations depending on the platform.
 * 
 * In contrast to the CPUStopwatch class the CPU stopwatch class
 * times the actual amount of CPU time used by the current thread.
 * 
 * This is actually a typedef set to the appropriate platform specific version
 * of the stopwatch.  The API is identical to StopwatchBase.
 * 
 */

/**
 * @class CPUStopwatch
 * @brief A multiplatform CPU-clock timer class
 *
 * In contrast to the wall-clock Stopwatch class the CPU stopwatch class
 * times the actual amount of CPU time used by the current thread.
 */


//------------------------------------------------------------------------------
// Base Interface 
//------------------------------------------------------------------------------
class StopwatchBase
{
public:
  explicit StopwatchBase(const char *name=0);
  virtual ~StopwatchBase();
  inline void Start();
  inline void Stop();
  inline void Reset();       // Like a real stopwatch, DOES NOT STOP THE CLOCK
  inline float GetTime() const;     // Instantaneous elapsed time
  float GetAvgTime() const;  // NOT ACCURATE IF NOT STOPPED!
  int GetNumStarts() const;
  void SetName(const char *n);   // sets identifier for this timer
  void SetName(int id);          // converts the int to a string name
  const char* GetName() const;
  const char* GetType() const;   // Return short description of implemntation

protected:
  float elapsedTime;     // the accumulated elapsed time
  int numStarts;         // number of calls to Start since last Reset
  char sw_name[STOPWATCH_MAX_NAME];  // a name for the stopwatch
  const char *sw_type;
  bool running;

  // THESE ARE THE METHODS SUBCLASSES NEED TO IMPLEMENT
  virtual void markTime()=0;   // jot down the current timestamp
  virtual float diffTime() const =0; // return  current_timestamp - last_mark
};


// Ostream operator to report elapsed time results 
// (defined as a template so you can call with either old- or
// new-style io headers.  I.e. you can use either 'std::cout<<' or
// 'cout<<', or any other object with an operator << for that matter)
template <class _OSTREAM>
inline _OSTREAM& operator<< (_OSTREAM& o, const StopwatchBase& s)
{
  if (s.GetNumStarts() > 1) 
    o << s.GetName() << " avg time: " 
      << s.GetAvgTime() << " sec, (avg of " << s.GetNumStarts() << " periods)";
  else
    o << s.GetName() << " time: " << s.GetTime() << " sec";
  return o;
}



//------------------------------------------------------------------------------
// Implementations
//------------------------------------------------------------------------------

class StopwatchGeneric : public StopwatchBase
{
public:
  StopwatchGeneric(const char* name=0);
  virtual ~StopwatchGeneric();
protected:
  void markTime();
  float diffTime() const;
  struct timeb lastStamp;
};

#ifdef _WIN32
// Including windows.h is really bad.  Defines a ton of macros that conflict
// with other libraries.
//#include <windows.h>
// This isn't so great either, but it's better than the alternative.
// The one data structure needed was extracted from windows.h header.
// in the .cpp file we cast this to the actual LARGE_INTEGER type used by
// the windows calls.  So really this just needs to be at least as big as
// that type.  The details don't much matter.
typedef union {
    struct {
        unsigned int LowPart;
        long HighPart;
    } u;
    _int64 QuadPart;
} StopLargeInteger;

class StopwatchWin32 : public StopwatchBase
{
public:
  StopwatchWin32(const char* name=0);
  virtual ~StopwatchWin32();
protected:
  void markTime();
  float diffTime() const;
  StopLargeInteger lastStamp;
  //LARGE_INTEGER lastStamp;
  static StopLargeInteger clockFreq;
  //static LARGE_INTEGER clockFreq;
  static bool clockFreqSet;
};
#endif

class CPUStopwatchGeneric : public StopwatchBase
{
public:
  CPUStopwatchGeneric(const char* name=0);
  virtual ~CPUStopwatchGeneric();
protected:
  void markTime();
  float diffTime() const;
  clock_t lastStamp;
};

#ifndef _WIN32
class StopwatchGTOD : public StopwatchBase
{
public:
  StopwatchGTOD(const char* name=0);
  virtual ~StopwatchGTOD();
protected:
  void markTime();
  float diffTime() const;
  struct timeval lastStamp;
};
#endif /* !_WIN32 */

#ifdef CLOCK_SGI_CYCLE
class StopwatchSGI : public StopwatchBase
{
public:
  StopwatchSGI(const char* name=0);
  virtual ~StopwatchSGI();
protected:
  void markTime();
  float diffTime() const;
  timespec_t lastStamp;
};
#endif /* CLOCK_SGI_CYCLE */


//------------------------------------------------------------------------------
// Define Stopwatch and CPUStopwatch to be the best known clocks for platform
//------------------------------------------------------------------------------
typedef CPUStopwatchGeneric CPUStopwatch;  // Is there any other?

#ifdef _WIN32
//typedef StopwatchGeneric Stopwatch;
typedef StopwatchWin32 Stopwatch;
#else /* UNIX */
#ifdef CLOCK_SGI_CYCLE
typedef StopwatchSGI Stopwatch;
#else /* !CLOCK_SGI_CYCLE */
typedef StopwatchGTOD Stopwatch;
#endif /* !CLOCK_SGI_CYCLE */
#endif /* !_WIN32 */






//------------------------------------------------------------------------------
// Inline method implementations
//------------------------------------------------------------------------------

/**
 * Begin the clock running.
 * Typical usage one-period usage is Start() ... do something ... Stop(), 
 * GetTime(), Reset(). 
 *
 * For timing multiple periods, the typical pattern is:
 * 
 * @code
 *   timer.Start();
 *   //... do something ...
 *   timer.Start();
 *   //... do something again ...
 *   //... repeat
 * @endcode
 *
 * then finally call Stop(), GetAvgTime(), and Reset().
 */
inline void StopwatchBase::Start()
{
  if (running) {
    numStarts++;
    return;
  }
  running = true;
  numStarts++;
  // markTime call should be last thing in function so we avoid timing 
  // our own activity as much as possible.
  markTime();
}

/**
 * Stop the clock.
 */
inline void StopwatchBase::Stop()
{
  if (running) {
    elapsedTime += diffTime();
    running = false;
  }
}

/**
 * Reset all the internal clock data.  
 * Safe to call even if the clock is currently running.
 * (i.e. in between a Start() and a Stop() pair).
 */
inline void StopwatchBase::Reset()
{
  if (running) { markTime(); numStarts=1; }
  else numStarts = 0;
  elapsedTime = 0.0f;
}

/**
 * Return the current amount of time on the clock.
 * This is the total amout of time that has elapsed
 * between Start() and Stop() pairs since the last
 * Reset().
 */
inline float StopwatchBase::GetTime() const
{
  if (running) return (elapsedTime + diffTime());
  else return (elapsedTime);
}




#endif /* GLVU_STOPWATCH_INCLUDED */
