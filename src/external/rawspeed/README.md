#RawSpeed Developer Information

##What is RawSpeed?

RawSpeed…

- is capable of decoding various images in RAW file format.
- is intended to provide the fastest decoding speed possible.
- supports the most common DSLR and similar class brands.
- supplies unmodified RAW data, optionally scaled to 16 bit, or normalized to 0->1 float point data.
- supplies CFA layout for all known cameras.
- provides automatic black level calculation for cameras having such information.
- optionally crops off  “junk” areas of images, containing no valid image information.
- can add support for new cameras by adding definitions to an xml file.
- is extensively crash-tested on broken files.
- decodes images from memory, not a file stream. You can use a memory mapped file, but it is rarely faster.
- is currently tested on more than 500 unique cameras.
- can add support for new cameras by updating an xml file.
- open source under the LGPL v2 license.

RawSpeed does NOT…

- read metadata information, beside whitebalance information.
- do any color correction or whitebalance correction.
- de-mosaic the image.
- supply a viewable image or thumbnail.
- crop the image to the same sizes as manufactures, but supplies biggest possible images.

So RawSpeed is not intended to be a complete RAW file display library,  but only act as the first stage decoding, delivering the RAW data to your application.

##Version 2, new cameras and features
- Support for Sigma foveon cameras.
- Support for Fuji cameras.
- Support old Minolta, Panasonic, Sony cameras (contributed by Pedro Côrte-Real)
- Arbitrary CFA definition sizes.
- Use [pugixml](http://pugixml.org/) for xml parsing to avoid depending on libxml.


##Getting Source Code

You can get access to the lastest version using [from here](https://github.com/klauspost/rawspeed). You will need to include the “RawSpeed” and “data” folder in your own project.

This includes a Microsoft Visual Studio project to build a test application. The test application uses [libgfl](http://www.xnview.com/en/gfl.html) to output 16 bit images. This library is not required for your own implementation though.

To see a GCC-based implementation, you can check out [this directory](http://rawstudio.org/svn/rawstudio/trunk/plugins/load-rawspeed/), which is the implementation Rawstudio uses to load the images. This also includes an automake file to set up. You can also have a look at the [darktable implementation](https://github.com/darktable-org/darktable/blob/master/src/common/imageio_rawspeed.cc), for which there is a [Cmake based build file](https://github.com/darktable-org/darktable/tree/master/src/external/rawspeed).


##Background of RawSpeed

So my main objectives were to make a very fast loader that worked for 75% of the cameras out there, and was able to decode a RAW file at close to the optimal speed. The last 25% of the cameras out there could be serviced by a more generic loader, or convert their images to DNG – which as a sidenote usually compresses better than your camera.

RawSpeed is not at the moment a separate library, so you have to include it in your project directly.


##Include files

All needed headers are available by including “RawSpeed-API.h”. You must have the pthread library and headers installed and available.

RawSpeed uses pthreads ~~and libxml2~~, which is the only external requirements beside standard C/C++ libraries. As of v2, libxml is no longer required.

You must implement a single function: “int rawspeed_get_number_of_processor_cores();”, which should return the maximum number of threads that should be used for decoding, if multithreaded decoding is possible.

Everything is encapsulated on a “RawSpeed” namespace. To avoid clutter the examples below assume you have a “using namespace RawSpeed;” before using the code.

##The Camera Definition file

This file describes basic information about different cameras, so new cameras can be supported without code changes. See the separate documentation on the [Camera Definition File](cameras-xml.md).

The camera definitions are read into the CameraMetaData object, which you can retain for re-use later. You initialize this data by doing

```cpp
static CameraMetaData *metadata = NULL;
if (NULL == metadata)
{
  try {
    metadata = new CameraMetaData("path_to_cameras.xml");
  } catch (CameraMetadataException &e) {
    // Reading metadata failed. e.what() will contain error message.
  }
}
```

The memory impact of this object is quite small, so you don’t have to free it every time. You can however delete and re-create it, if you know the metadata file has been updated.

You can disable specific cameras in the xml file, or if you would want to do it in code, you can use:

```cpp
    // Disable specific camera
    metadata.disableCamera("Canon", "Canon EOS 100D")

    // Disable all cameras from maker:
    metadata.disableCamera("Fuji")
```

##Using RawSpeed

You need to have the file data in a FileMap object. This can either be created by supplying the file content in memory using FileMap(buffer_pointer, size_of_buffer), or use a “FileReader” object to read the content of a file, like this:

```cpp
FileReader reader(filename);
FileMap* map = NULL;
try {
  map = reader.readFile();
} catch (FileIOException &e) {
  // Handle errors
}
```

The next step is to start decoding. The first step is to get a decoder:

```cpp
RawParser parser(map);
RawDecoder *decoder = parser.getDecoder();
```

This will do basic parsing of the file, and return a decoder that is capable of decoding the image. If no decoder can be found or another error occurs a “RawDecoderException” object will be thrown. The next step is to determine whether the specific camera is supported:

```cpp
decoder->failOnUnknown = FALSE;
decoder->checkSupport(metadata);
```

The “failOnUnknown” property will indicate whether the decoder should refuse to decode unknown cameras. Otherwise RawSpeed will only refuse to decode the image, if it is confirmed that the camera type cannot be decoded correctly. If the image isn’t supported a “RawDecoderException” will be thrown.

Reaching this point should be very fast in terms of CPU time, so the support check is very quick, if file data is quickly available. Next we decode the image:

```cpp
decoder->decodeRaw();
decoder->decodeMetaData(metadata);
RawImage raw = decoder->mRaw;
```

This will decode the image, and apply metadata information. The RawImage is at this point completely untouched Raw data, however the image has been cropped to the active image area in decodeMetaData. Error reporting is: If a fatal error occurs a RawDecoderException is thrown.

Non-fatal errors are pushed into a "vector" array in the decoder object called "errors". With these types of errors, there WILL be a raw image available, but it will likely contain junk sections in undecodable parts. However, as much as it was possible to decode will be available. So treat these messages as warnings.

Another thing to note here is that the RawImage object is automatically refcounted, so you can pass the object around  without worrying about the image being freed before all instances are out of scope. Do however keep this in mind if you pass the pointer to image data to another part of your application.

```cpp
raw->scaleBlackWhite();
```

This will apply the black/white scaling to the image, so the data is normalized into the 0->65535 range no matter what the sensor adjustment is (for 16 bit images). This function does no throw any errors. Now you can retrieve information about the image:

```cpp
int components_per_pixel = raw->getCpp();
RawImageType type = raw->getDataType();
bool is_cfa = r->isCFA;
```

Components per pixel indicates how many components are present per pixel. Common values are 1 on CFA images, and 3, found in some DNG images for instance. Do note, you cannot assume that an images is CFA just because it is 1 cpp - greyscale dng images from things like scanners can be saved like that.

The RawImageType can be TYPE_USHORT16 (most common) which indicates unsigned 16 bit data or TYPE_FLOAT32 (found in some DNGs)

The isCFA indicates whether the image has all components per pixel, or if it was taken with a colorfilter array. This usually corresponds to the number of components per pixel (1 on CFA, 3 on non-CFA).

The ColorfilterArray contains information about the placement of colors in the CFA:

```cpp
if (TRUE == is_cfa) {
  ColorFilterArray cfa = raw->cfa;
  int dcraw_filter = cfa.getDcrawFilter();
  int cfa_width = cfa.size.x;
  int cfa_height = cfa.size.y;
  CFAColor c = cfa.getColorAt(0,0);
}
```

To get this information as a dcraw compatible filter information, you can use getDcrawFilter() function.

You can also use getColorAt(x, y) to get a single color information. ~~Note that unlike dcraw, RawSpeed only supports 2×2 patterns, so you can reuse this information.~~ CFAColor can be CFA_RED, CFA_GREEN, CFA_BLUE for instance.

Finally information about the image itself:

```cpp
unsigned char* data = raw->getData(0,0);
int width = raw->dim.x;
int height = raw->dim.y;
int pitch_in_bytes = raw->pitch;
```

The getData(x, y) function will give you a pointer to the Raw data at pixel x, y. This is the coordinate after crop, so you can start copying data right away. Don’t use this function on every pixel, but instead increment the pointer yourself. The width and height gives you the size of the image in pixels – again after crop.

Pitch is the number of bytes between lines, since this is usually NOT width * components_per_pixel * bytes_per_component. So in this instance, to calculate a pointer at line y, use &data[y * raw->pitch] for instance.

Finally to clean up, use:

```cpp
delete map;
delete decoder;
```

Actually the map and decoder can be deallocated once the metadata has been decoded. The RawImage will automatically be deallocated when it goes out of scope and the decoder has been deallocated. After that all data pointers that have been retrieved will no longer be usable.

##Tips & Tricks

You will most likely find that a relatively long time is spent actually reading the file. The biggest trick to speeding up raw reading is to have some sort of prefetching going on while the file is being decoded. This is the main reason why RawSpeed decodes from memory, and doesn’t use direct file reads while decoding.

The simplest solution is to start a thread that simply reads the file, and rely on the system cache to cache the data. This is fairly simple and works in 99% of all cases. So if you are doing batch processing simply start a process reading the next file, when the current image starts decoding. This will ensure that your file is read linearly, which gives the highest possible throughput.

A more complex option is to read the file to a memory portion, which is then given to RawSpeed to decode. This might be a few milliseconds faster in the best case, but I have found no practical difference between that and simply relying on system caching.

You might want to try out memory mapped files. However this approach has in practical tests shown to be just as fast in best cases (when file is cached), or slower (uncached files).

##Bad pixel elimination

A few cameras will mark bad pixels within their RAW files in various ways. For the camera we know how to this will be picked up by RawSpeed. By default these pixels are eliminated by 4-way interpolating to the closest valid pixels in an on-axis search from the current pixel.

If you want to do bad pixel interpolation yourself you can set:

```cpp
RawDecoder.interpolateBadPixels = FALSE;
```

Before calling the decoder. This will disable the automatic interpolation of bad pixels. You can retrieve the bad pixels by using:

```cpp
std::vector<uint32> RawImage->mBadPixelPositions;
```

This is a vector that contains the positions of the detected bad pixels in the image. The positions are stored as x | (y << 16), so maximum pixel position is 65535, which also corresponds with the limit of the image sizes within RawSpeed. you can loop through all bad pixels with a loop like this:

```cpp
for (vector<uint32>::iterator i=mBadPixelPositions.begin(); i != mBadPixelPositions.end(); i++)  {
    uint32 pos_x = (*i)&0xffff;
    uint32 pos_y = (*i)>>16;
    ushort16* pix = (ushort16*)getDataUncropped(pos_x, pos_y);
}
```

This however may not be most optimal format for you. You can also call RawImage->transferBadPixelsToMap(). This will create a bit-mask for you with all bad pixels. Each byte correspond to 8 pixels with the least significant bit for the leftmost pixel. To set position x,y this operation is used:

```cpp
RawImage->mBadPixelMap[(x >> 8) + y * mBadPixelMapPitch] |=  1 << (x & 7);
```

This enables you to quickly search through the array. If you for instance cast the array to integers you can check 32 pixels at the time.

Note that all positions are uncropped image positions. Also note that if you keep the interpolation enabled you can still retrieve the mBadPixelMap, but the mBadPixelPositions will be cleared.

##Updating Camera Support

If you implement an autoupdate feature, you simply update “cameras.xml” and delete and re-create the CameraMetaData object.

There might of course be some specific cameras that require code-changes to work properly. However, there is a versioning check inplace, whereby cameras requirering a specific code version to decode properly will be marked as such.

That means you should safely be able to update cameras.xml to a newer version, and cameras requiring a code update will then simply refuse to open.


##Format Specific Notes

###Canon sRaw/mRaw
Canon reduced resolution Raws (mRaw/sRaw) are returned as RGB with 3 component per pixel without whitebalance compensation, so color balance should match ordinary CR2 images. The subsampled color components are linearly interpolated.

This is even more complicated by the fact that Canon has changed the way they store the sraw whitebalance values. This means that on newer cameras, you might have to specify "invert_sraw_wb" as a hint to properly decode the whitebalance on these casmeras. To see examples of this, search cameras.xml for "invert_sraw_wb".

###Sigma Foveon Support

Sigma Foveon (x3f-based) images are delivered as raw image values. dcraw offers a "cleanup" function, that will reduce noise in Foveon images. RawSpeed does not have an equivalent function, so if you want to use RawSpeed as a drop-in replacement, you will either have to convert the dcraw "foveon_interpolate", or implement similar noise reduction, if you want it.

###Fuji Rotated Support

By default RawSpeed delivers Fuji SuperCCD images as 45 degree rotated images.

RawSpeed does however use two camera hints to do this. The first hint is "fuji_rotate": When this is specified in cameras.xml, the images are rotated.

To check if an image has been rotated, check RawImage->fujiWidth after calling RawDecoder->decodeMetaData(...) If it is > 0, then the image has been rotated, and you can use this value to calculate the un-rotated image size. See [here](https://rawstudio.org/svn/rawstudio/trunk/plugins/fuji-rotate/fuji-rotate.c) for an example on how to rotate the image back after de-mosaic.

If you do NOT want your images to be delivered rotated, you can disable it when decoding.
```cpp
RawDecoder->fujiRotate = FALSE;
```
Do however note the CFA colors are still referring to the rotated color positions.


##Other options

###RawDecoder -> uncorrectedRawValues
If you enable this on the decoder before calling RawDecoder->decodeRaw(), you will get complely unscaled values. Some cameras have a "compressed" mode, where a non-linear compression curve is applied to the image data. If you enable this parameter the compression curve will not be applied to the image. Currently there is no way to retrieve the compression curve, so this option is only useful for diagnostics.


###RawImage.mDitherScale
This option will determine whether dither is applied when values are scaled to 16 bits. Dither is applied as a random value between "+-scalefactor/2". This will make it so that images with less number of bits/pixel doesn't have a big tendency for posterization, since values close to eachother will be spaced out a bit.

Another way of putting it, is that if your camera saves 12 bit per pixel, when RawSpeed upscales this to 16 bits, the 4 "new" bits will be random instead of always the same value.

##Memory Usage

RawSpeed will need:

* Size of Raw File.
* Image width * image height * 2 for ordinary Raw images with 16 bit output.
* Image width * image height * 4 for float point images with float point output .
* Image width * image height * 6 for ordinary Raw images with float point output.
* Image width * image height / 8 for images with bad pixels.

##Submitting Requests and Patches

Please go to the [github page](https://github.com/klauspost/rawspeed) and submit your (pull)requests and issues there.
