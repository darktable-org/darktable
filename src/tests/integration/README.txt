
Structure
---------

images/    : a directory containing test images

run.sh     : main driver

deltae     : python script to compute a delta-E between 2 images
             expected.jpg and output.jpg

nnnn-name/  : tests

How to add a new test (using default driver)
--------------------------------------------

1. Create a new directory

   <nnnn>-<meaningful name>

2. Start darktable, open one test image (or add a new one if needed)

3. Do a dev using whatever module

4. Copy the resulting .xmp into <nnnn>-<meaningful name>

   And rename it <meaningful name>.xmp

5. Do a first run of the test to get the expected output

   ./run <dir>

   The output.png will be copied to expected.png, double check that
   expected.png is correct and really the expected output.

6. Test that all is ok by running:

   ./run <dir>

   All values must be 0 as there is no change in darktable, so the
   expected output should be exactly the same image as the output.

   $ ./run.sh 0001-exposure
   Test ./0001-exposure
      image mire1.cr2
      Max  dE         0.0000
   OK

7. If all goes well commit the .xmp and expected.png files



How to add a new test (using specific driver)
--------------------------------------------

1. Create a new directory

   <nnnn>-<meaningful name>

2. Create a file named test.sh into this directory

   This test.sh is a specific driver that can do whatever is necessary
   for the test. At the end the driver must return 0 if all is OK and
   1 otherwise.
