
=========  Installing LibRaw (CMake version) ==========

I. Installation steps

1. Unpack the distribution file:

 $  tar xzvf LibRaw-0.xx.yy.tar.gz

2. Copy this LibRaw-cmake scripts in LibRaw-... folder

3. Go to LibRaw folder and run ./configure and make:

 $ cd LibRaw-0.xx.yy
 $ ./cmake  [...optional args...] .
 $ make

4. install by run make install as root:
 
 $ sudo make install  


II. ./cmake options

-DENABLE_OPENMP=ON/OFF

  Enable/disable OpenMP support if compiler supports it.
  OpenMP is enabled by default.


-DENABLE_LCMS=ON/OFF

  Enable/disable LCMS color engine support. If enabled, ./cmake will try to 
  find lcms library.  Both LCMS-1.x and LCMS-2.x are supported
  LCMS support is enabled by default


-DENABLE_EXAMPLES=ON/OFF

  Enables/disables examples compilation and installation. Enabled by default


-DENABLE_RAWSPEED=ON/OFF
-DRAWSPEED_RPATH=FOLDERNAME

  Enables/disables support of additional code from RawStudio project
  You need to download RawSpeed source code to use this feature.
  See README.RawSpeed.txt for details.
  ./cmake will try to find RawSpeed code in:
     
     a) If folder is specified via  -DRAWSPEED_RPATH=FOLDERNAME 
        command-line option, then only this folder will be checked.
        
     b) If no folder is specified in -DENABLE_RAWSPEED switch:

          ./RawSpeed (in LibRaw folder)



-DENABLE_DCRAW_DEBUG=ON/OFF

  Enables/disables support of additional debug traces from dcraw operations. Disabled by default
