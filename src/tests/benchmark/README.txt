darktable-bench is a simple script to apply standardized processing to
a standard image for the purpose of comparing performance between
systems.  It reports the average time taken for the processing over
multiple runs, as well as a performance metric which is useful for
comparison (a system with a rating twice as high as another will let
you export twice as many images in a given time).

Performance numbers will only be directly comparable when using the
same version of darktable, the same image, and the same sidecar file.
Unless you override the image or sidecar, they will remain the same.
Comparisons between different darktable versions will also reflect
performance changes between the versions (see the "Comparative
Performance" section at the end of this file).

Note that on a slow machine, it could easily take three to five
minutes to run the benchmark.

Usage
-----

In the simplest invocation, you can run the program directly from the
top-level directory of your darktable source hierarchy:

   src/tests/benchmark/darktable-bench

This will then run the default sidecar (v3.6) on the default image
(mire1.cr2 from the integration test suite) using the darktable-cli in
the build directory, or the darktable-cli on your search path.


The following commandline options are available:

   -i / --image FILE
   		specify the image to use instead of mire1.cr2

   -v / --version VER
   		use alternate sidecar darktable-bench-VER.xmp

   -p / --program PATH
   		specify the program to execute

   -x / --xmp NAME
   		override the base name of the sidecar file and use
   		NAME-VER.xmp instead

   -r N / --reps N
   		run the development N times instead of 3

   -t N / --threads N
   		tell darktable-cli to run with N threads (default is
		the number of hardware threads)

   -C / --cpuonly
		disable OpenCL GPU acceleration and run using the CPU
		only

   -T PATH / --tempdir PATH
   		store temporary files in a scratch directory under
   		PATH (default /tmp)

Report
------

darktable-bench prints the time each development took, as well as the
average and the throughput rating, which is the approximate number of
images of this size with this processing that could be exported per
hour.  (You will likely see greater throughput on your own images, as
the benchmark processing is deliberately very compute-intensive.)  The
reported times are the total pixelpipe processing time reported by
darktable-cli, and the pixelpipe time plus load/save time.  As
darktable-cli currently does not report the time needed to write the
final result, darktable-bench assumes that the save time is the same
as the load time.

Sample output

      Preparing...done
	   run # 1:   8.595 pixpipe,    8.817 total
	   run # 2:   8.572 pixpipe,    8.796 total
	   run # 3:   8.548 pixpipe,    8.770 total

      darktable 3.2.1 ::: benchmark v3.4 ::: image mire1.cr2
      Average pixelpipe processing time:      8.572 seconds
      Average overall processing time:        8.794 seconds
      Throughput rating (higher is better):   409.4 (CPU only)

If you specified the number of threads to use (for example, to check
whether hyperthreading helps or hinders performance), that number will
be included in the report

      darktable 3.5.0+2252~g0fffe6150 ::: benchmark v3.6 ::: image mire1.cr2
      Number of threads used:                    32
      Average pixelpipe processing time:      5.381 seconds
      Average overall processing time:        5.599 seconds
      Throughput rating (higher is better):   642.9 (CPU only)


Structure
---------

darktable-bench		 : the benchmarking script (Python 3)

darktable-bench-null.xmp : a sidecar file with minimal processing,
			   used to warm up disk caches

darktable-bench-3.6.xmp  : the default benchmarking sidecar
darktable-bench-3.4.xmp  : alternate sidecar for older version

../integration/images/mire1.cr2 : the default benchmarking image


How to add a new benchmark
--------------------------

1. open an image in darktable and apply whatever processing you desire

2. copy the generated .xmp sidecar into src/tests/benchmark under the
   name 'darktable-bench-XYZ.xmp'

3. run

   darktable-bench -v XYZ

   to apply your new sidecar to the standard image from the
   integration test suite (src/tests/integration/images/mire1.cr2).


Comparative Performance
-----------------------

Reported performance numbers depend on the hardware, darktable
version, image, and sidecar file used.  The following are some example
throughput ratings using the standard image.

Thruput Sidecar dt           Hardware
~410*   3.4     3.2.1        32-core AMD Threadripper 3970X, 64GB PC3600, no GPU
~645    3.4     3.4.0        32-core AMD Threadripper 3970X, 64GB PC3600, no GPU
~690    3.4     3.6.0        32-core AMD Threadripper 3970X, 64GB PC3600, no GPU
720     3.4     3.7.0+440    32-core AMD Threadripper 3970X, 64GB PC3600, no GPU
713     3.4     3.9.0+1630   32-core AMD Threadripper 3970X, 64GB PC3600, no GPU

659     3.6     3.7.0+440    32-core AMD Threadripper 3970X, 64GB PC3600, no GPU
661     3.6     3.9.0+1630   32-core AMD Threadripper 3970X, 64GB PC3600, no GPU

644	3.8	3.7.0+1370   32-core AMD Threadripper 3970X, 64GB PC3600, no GPU
666     3.8     3.9.0+1630   32-core AMD Threadripper 3970X, 64GB PC3600, no GPU

[*] darktable 3.2.1 using the v3.4 sidecar skips two modules which
  didn't yet exist, so this number is actually over-reporting the
  comparative performance.
