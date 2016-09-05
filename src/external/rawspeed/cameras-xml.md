#RawSpeed Camera Definition File

The camera definition file is used for decoding images which doesn’t require any code changes. This enables us to add support for cameras which where not yet released when the code was written.

```xml
<Camera make="Panasonic" model="DMC-FZ45" mode="4:3" supported="yes" decoder_version="0">
  <ID make="Panasonic" model="DMC-FZ45">Panasonic DMC-FZ45</ID>
  <CFA width="2" height="2">
    <Color x="0" y="0">GREEN</Color><Color x="1" y="0">BLUE</Color>
    <Color x="0" y="1">RED</Color><Color x="1" y="1">GREEN</Color>
  </CFA>
  <Crop x="0" y="0" width="-58" height="-10"/>
  <Sensor black="150" white="4097" iso_min="0" iso_max="0"/>
  <BlackAreas>
    <Vertical x="0" width="60"/>
    <Horizontal y="2" height="46"/>
  </BlackAreas>
  <Hints>
    <Hint name="coolpixsplit" value=""/>
  </Hints>
  <Aliases>
    <Alias id="DMC-FZ40">DMC-FZ40</Alias>
  </Aliases>
</Camera>
```

Let’s go through it line for line:

##Camera Name

```xml
<Camera make="Panasonic" model="DMC-FZ45" mode="4:3" supported="yes" decoder_version="0">
```

This the basic camera identification. In this the make and model are required. This must be exactly as specified in the EXIF data of the file.

Mode refers to specific decoder modes which are special for each manufacturer. For cameras with specific modes there is usually a default (no mode specified) and some for non-default operation. For Canon for instance mode refers to “sRaw1″ and “sRaw2″. For Panasonic it refers to cropping modes, since they require different cropping of the output image.

The supported tag specifies whether a camera is supported. If this tag isn’t added it is assumed to be supported.

The decoder_version is a possibility to disable decoding, if the decoder version is too old to properly decode the images from this camera. If the code version of RawSpeed is too old to decode this camera type, it will refuse to do so. If this isn’t specified it is assumed that all older versions of RawSpeed can decode the image.

##Camera ID

```xml
<ID make="Panasonic" model="DMC-FZ45">Panasonic DMC-FZ45</ID>
```

This sets the canonical name for the camera. The content of the tag should be the same as the UniqueCameraModel DNG field in the Adobe DNG converted raw file and can be used to match the camera against DCP files or other external references. The make and model attributes are clean names (no repetitions, spurious words, etc) that can be used in UI. If the Alias tag is omitted the make and model from the Camera tag are used instead (joined with a space for UniqueCameraModel), so in this particular case the tag is actually not needed.

##CFA Colors

```xml
  <CFA width="2" height="2">
    <Color x="0" y="0">GREEN</Color><Color x="1" y="0">BLUE</Color>
    <Color x="0" y="1">RED</Color><Color x="1" y="1">GREEN</Color>
  </CFA>
```

This refers to the color layout of the sensor. This is the position at of the colors on the uncropped image, so it will be the same no matter what crop you specify. Currently only 2×2 CFA patterns are possible.

From version 2, there is an alternative syntax; *CFA2*. This definition allows for sizes *bigger than 2x2* and has a simpler syntax:

```xml
	<CFA2 width="2" height="2">
		<ColorRow y="0">RG</ColorRow>
		<ColorRow y="1">GB</ColorRow>
	</CFA2>
```
Valid colors are:

Colors are G(reen), R(ed), B(blue) , F(uji green), C(yan), M(agenta) and Y(ellow).

##Image Cropping

```xml
  <Crop x="0" y="0" width="-58" height="-10"/>
```

This is the cropping to be applied to the image. x & y are specified relative to the top-left of the image and is specified in pixels. Width & Height can be a number which is the desired output size in pixels. A negative number for width or height specifies a number of pixels that must be cropped from the bottom/right side of the image.

##Sensor Info

```xml
  <Sensor black="150" white="4097" iso_min="0" iso_max="0"/>
```

This tag can be added more than 1 time, but at least 1 must be present.

This specifies the black and white levels of images captured. Black and white must be specified. On cameras (some Nikons for instance) and files (DNG images) where this can be read from the image files themselves this is overridden.

The iso_min and iso_max are optional which indicates an ISO range where this must be applied. If both are set to 0, or left undefined they act as default values for all ISO values. Note that not all cameras may decode the ISO value.

Both ISO values are inclusive, so specify ranges so they don’t overlap (0->399, 400->799, etc). If different entry ranges overlap the first match will be used.

For backward compatibility, leave the default value as the last entry.

##Sensor Black Areas

```xml
  <BlackAreas>
    <Vertical x="0" width="60"/>
    <Horizontal y="2" height="46"/>
  </BlackAreas>
```

This entry specifies one or more “black” areas on the sensor. This is areas where the sensor receives no light and it can therefore be used to accurately determine the black level of each image. The areas can be described as a vertical area starting a fixed number of pixels from the left and having a fixed width, or a horizontal, starting a fixed number of pixels down and having a fixed height.

All the areas are summed up in a histogram for each color component, and the median value is selected as the black value. This should ensure that noise and minor differences in hardware shouldn’t influence the calculations.

If any black areas are defined it will override any “black” value set in the Sensor definition.

##Decoder Hints

```xml
  <Hints>
    <Hint name="coolpixsplit" value=""/>
  </Hints>
```

This may contain manufacturer-specific hints for decoding. This can result in the code taking a specific decoder path, or otherwise treat the image differently. This is mainly used when it isn’t possible to determine which way to decode the image directly from the image data.

##Camera Model Aliases

```xml
  <Aliases>
    <Alias id="DMC-FZ40">DMC-FZ40</Alias>
  </Aliases>
```

This is a possibility to add one or more model aliases for a camera, which may have different model names in different regions. The id attribute specifies the clean model name for this alias, if ommited defaults to alias value (so in this case is not really needed).
