# darktable-curve-tool


## About


The _darktable-curve-tool_ program will help you approximate more accurately the transfer
curves used by your in-camera JPEG engine.

This tool does so by analyzing both the RAW data and the resulting
JPEG data from your camera.


## Limitations


The computed curves are by no mean a way to have the exact same rendering as
your in camera JPEG engine. Many more algorithms are used by your camera to
generate the JPEG. The curves are only one of them.

The tool has some known limitations:

 - it computes RGB basecurve on the sole G channel, though all three channels are
   analysed
 - the tool supposes the JPEG files are sRGB ones. It doesn't know or understands
   about ICC profiles (not even AdobeRGB).
 - the tool is happily confused by JPEG files that are portrait rotated, and their
   raw is not. The helper script tries to auto correct that during the conversion step.

## Requirements


Utilities mentioned below require following dependencies to be installed: `dcraw`, `libexiv2-dev`, `sqlite3` and `gnuplot`.

## Building


You can build the tool using the following commands:

    $ cd "$DARKATBLE_SRC_ROOT/tools/basecurve"
    $ mkdir build
    $ cd build/
    $ cmake -DCMAKE_INSTALL_PREFIX="@CMAKE_INSTALL_PREFIX@" -DCMAKE_INSTALL_LIBEXECDIR="@CMAKE_INSTALL_LIBEXECDIR@" -DCMAKE_BUILD_TYPE=Release ..
    $ cmake --build . -- install

You are invited to print the help message to get to know the tool's options:

    $ "@CMAKE_INSTALL_FULL_LIBEXECDIR@/darktable/tools/darktable-curve-tool" -h

It may help you better understand the following paragraphs.


## Determining a basecurve/tonecurve using _darktable-curve-tool-helper_ script


An additional helper script called _darktable-curve-tool-helper_ is provided. This
script should automate many steps of the curve determination process.

It is assumed that `$YOUR_INSTALL_PATH` is in your `$PATH`. If not you can run in bash/zsh:

export PATH="$PATH:@CMAKE_INSTALL_FULL_LIBEXECDIR@/darktable/tools/"


### Gathering the statistics


    $ for raw in my raw file list ; do
         darktable-curve-tool-helper "$raw"
      done

_darktable-curve-tool-helper_ will look for corresponding JPEG files by itself; if no
corresponding JPEG file is found, the embedded JPEG file from the raw is extracted.


### Computing the curves


    $ darktable-curve-tool -z -e <one of the RAW files> | tee mycameracurves.sh

At this point, you should have some console output explaining how to apply these
curves, or submit them for final inclusion by the darktable developers


### Applying the curves


The following command will inject the computed curves in your database.
It is highly recommended to back it up first!

    $ cp $HOME/.config/darktable/data.db $HOME/.config/darktable/data.db.bcp

Now you can safely run the inject script:

    $ sh ./mycameracurves.sh


## Determining a basecurve/tonecurve with _darktable-curve-tool_ alone


Using _darktable-curve-tool-helper_ may not be sufficient and you need to either
have more control or understand what is done behind the hood by the script.
The following chapters will explain in depth all the steps required for
determining a curve with _darktable-curve-tool_ alone


### Creating the PPM versions of your JPEG and raw files


Let's say you have FILE.RAW (eg: .NEF/.CR2) and FILE.JPG

    $ dcraw -6 -W -g 1 1 -w FILE.RAW
    $ mv FILE.ppm FILE-raw.ppm

This creates a PPM file, named FILE.ppm, that we rename to FILE-raw.ppm. This
file contains the data from your sensor in a convenient format for darktable-curve-tool
to read. This data represents the data used as input by your in camera JPEG
engine.

Let's now convert the JPEG file to the same convenient format:

    $ convert FILE.JPG FILE-jpeg.ppm

This creates another PPM file. But this new PPM file contains the data that your
in camera JPEG engine has output. This step may also involve a rotation of your
image so that the PPM from the raw and the JPEG share the same orientation.


### Gathering a round of statistics


It is now time to let _darktable-curve-tool_ analyse these two files so that it can gather
some statistical data for a later computation of the curves

It is assumed _darktable-curve-tool_ is in your `$PATH`.

    $ darktable-curve-tool FILE-raw.ppm FILE-jpeg.ppm

This command loads and analyses the corresponding pixels found in both images. It
writes, to a state file, the correspondence found for each pixel value.

Given the histogram of each photography, you may need to repeat this operation
multiple times to cover the whole range of values that your camera is able to
capture. There is no exact number of files to be analysed, this all depends on
your camera tonal range, and the scenes being photographed.

The only thing you have to take care, is to point _darktable-curve-tool_ to the same save
state file with the option _-s_ (which stands for **s**tate file). Let's say you specify
the _-s_ option even on first run like this

    $ darktable-curve-tool -s "$HOME/tmp/mycamera.dat" FILE-raw.ppm FILE-jpeg.ppm

You are then able to accumulate more data for this camera doing something like this

    $ darktable-curve-tool -s "$HOME/tmp/mycamera.dat" FILE-raw2.ppm FILE-jpeg2.ppm
    $ darktable-curve-tool -s "$HOME/tmp/mycamera.dat" FILE-raw3.ppm FILE-jpeg3.ppm
    ...
    $ darktable-curve-tool -s "$HOME/tmp/mycamera.dat" FILE-rawN.ppm FILE-jpegN.ppm

Beware that _darktable-curve-tool_ uses 32bit counters internally to keep track of the number
of times a RGB/Lab sample has been encountered. As cameras these days do have many pixels
a photo, do not be zealous; do not run the tool on your complete catalog. In the
case too many pixels have been sampled already, an error is printed on the
console and _darktable-curve-tool_ refuses to process any further image.

It may be smart to pick from 20 to 50 pics covering the whole tonal range of your
camera; there is no need for thousands of pictures, firstly, it'd be real slow, and
secondly the resulting accuracy would not be improved significantly.

It is now time to analyse the data and output the curves.


### Analysing and outputting the curves


So you gathered data in `$HOME/tmp/mycamera.dat`, that's perfect. Let's compute the
curves now.

    $ darktable-curve-tool -z -e <one of the RAW files> -s ~/tmp/mycamera.dat | tee mycameracurves.sh
    [this will print you a script on screen and in the mycameracurves.sh file]

Little explanation before trying out the computed curves.

The _-z_ option tells the _darktable-curve-tool_ program to read the save state and compute
the curves. The _-e_ option is just a convenient option for pointing _darktable-curve-tool_
to a file containing EXIF data that can provide your camera Model name.

You can generate curves with more or less points to approximate the values
gathered during step 1. See option _-n_. The tool does not accept more than
20 points maximum. Something between 10 to 16 should be enough.


### Applying the curves


Feeling adventurous ? Ready to try your curves ?

First backup your darktable database:

    $ cp "$HOME/.config/darktable/data.db" "$HOME/.config/darktable/data.db.bck"

Then go on, import the curves:

    $ sh mycameracurves.sh

Spawn _darktable_, and check you got a new curve in the tonecurve module presets
and the basecurve module presets. If you provided the _-e_ option to the final
_darktable-curve-tool_ command run, the preset should be named as your camera Model name.
Otherwise, they will be named 'measured basecurve/tonecurve'

### Applying the curves automatically

Do not hesitate to setup the preset so that it is automatically applied when you
import/edit photos from this camera model.

Use the usual darktable GUI options for that. Either global options preset editor
or the module preset little tiny button once you selected that preset->Edit preset.

### Plotting the data for checking validity of data and fit

On the final tool invocation (with _-z_ option) you may be interested in looking at
what _darktable-curve-tool_ munged and analysed for you.

Two [GNUPlot](http://gnuplot.info/) scripts are provided in the same source directory to do so.
They require files `basecurve.dat` and `basecurve.fit.dat` resp. `tonecurve.dat`
and `tonecurve.fit.dat` to be present in the `$DARKATBLE_SRC_ROOT/tools/basecurve` directory.

    $ gnuplot -c "@CMAKE_INSTALL_FULL_DATAROOTDIR@/darktable/tools/basecurve/gnuplot.tonecurve"
    $ gnuplot -c "@CMAKE_INSTALL_FULL_DATAROOTDIR@/darktable/tools/basecurve/gnuplot.basecurve"

This generates a `basecurve.pdf` resp. `tonecurve.pdf` file with a graph of
the gathered data and the fitted curves. This can help you measuring how much of
the tonal range your sampling photos have covered.
