cmake_minimum_required(VERSION 2.6)

include_directories("${CMAKE_CURRENT_BINARY_DIR}/../" "${CMAKE_CURRENT_SOURCE_DIR}")

set(MODULES tonecurve gamma temperature colorcorrection exposure
	colorin colorout clipping sharpen monochrome basecurve colorzones
	basecurve highlights velvia vignette splittoning grain bilateral
	profile_gamma colorcontrast lowpass vibrance
	channelmixer graduatednd relight zonesystem demosaic atrous tonemap
  colortransfer colorize invert shadhi
  cacorrect overexposed bloom soften clahe equalizer highpass
  hotpixels lowlight spots rawdenoise borders nlmeans levels
  flip globaltonemap bilat denoiseprofile dither)

add_library(soften MODULE "soften.c")
add_library(bloom MODULE "bloom.c")
add_library(highpass MODULE "highpass.c")
add_library(lowpass MODULE "lowpass.c")
add_library(shadhi MODULE "shadhi.c")
add_library(tonemap MODULE "tonemap.cc")
add_library(tonecurve MODULE "tonecurve.c")
add_library(gamma MODULE "gamma.c")
add_library(temperature MODULE "temperature.c")
add_library(colorcorrection MODULE "colorcorrection.c")
# add_library(shrecovery MODULE "shrecovery.c")
add_library(exposure MODULE "exposure.c")
add_library(equalizer MODULE "equalizer.c")
add_library(colorin MODULE "colorin.c")
add_library(colorout MODULE "colorout.c")
add_library(clipping MODULE "clipping.c")
add_library(sharpen MODULE "sharpen.c")
add_library(dither MODULE "dither.c")
add_library(monochrome MODULE "monochrome.c")
add_library(basecurve MODULE "basecurve.c")
add_library(colorzones MODULE "colorzones.c")
add_library(highlights MODULE "highlights.c")
add_library(velvia MODULE "velvia.c")
add_library(vignette MODULE "vignette.c")
add_library(splittoning MODULE "splittoning.c")
add_library(grain MODULE "grain.c")
add_library(clahe MODULE "clahe.c")
# add_library(anlfyeni MODULE "anlfyeni.c")
add_library(bilateral MODULE "bilateral.cc")
add_library(profile_gamma MODULE "profile_gamma.c")
add_library(colortransfer MODULE "colortransfer.c")
add_library(channelmixer MODULE "channelmixer.c")
add_library(graduatednd MODULE "graduatednd.c")
add_library(relight MODULE "relight.c")
add_library(zonesystem MODULE "zonesystem.c")
add_library(demosaic MODULE "demosaic.c")
add_library(atrous MODULE "atrous.c")
add_library(cacorrect MODULE "cacorrect.c")
add_library(overexposed MODULE "overexposed.c")
add_library(hotpixels MODULE "hotpixels.c")
add_library(lowlight MODULE "lowlight.c")
add_library(spots MODULE "spots.c")
add_library(rawdenoise MODULE "rawdenoise.c")
add_library(borders MODULE "borders.c")
add_library(nlmeans MODULE "nlmeans.c")
add_library(colorcontrast MODULE "colorcontrast.c")
add_library(levels MODULE "levels.c")
add_library(colorize MODULE "colorize.c")
add_library(invert MODULE "invert.c")
add_library(vibrance MODULE "vibrance.c")
add_library(flip MODULE "flip.c")
add_library(globaltonemap MODULE "globaltonemap.c")
add_library(bilat MODULE "bilat.c")
add_library(denoiseprofile MODULE "denoiseprofile.c")

if(RSVG2_FOUND)
	add_library(watermark MODULE "watermark.c")
	set(MODULES ${MODULES} watermark)
endif(RSVG2_FOUND)

if(LENSFUN_FOUND)
	add_library(lens MODULE "lens.c")
	set(MODULES ${MODULES} lens)
endif(LENSFUN_FOUND)

# Add libs references
foreach(module ${MODULES})
        target_link_libraries(${module} ${LIBS})
endforeach(module)
foreach(module ${EXPERIMENTAL_MODULES})
        target_link_libraries(${module} ${LIBS})
endforeach(module)

foreach(module ${MODULES})
	install(TARGETS  ${module} DESTINATION ${LIB_INSTALL}/darktable/plugins)
endforeach(module)

if(INSTALL_IOP_EXPERIMENTAL)
  message("-- Installing experimental iop plugins")
  foreach(module ${EXPERIMENTAL_MODULES})
    	install(TARGETS  ${module} DESTINATION ${LIB_INSTALL}/darktable/plugins)
  endforeach(module)
endif(INSTALL_IOP_EXPERIMENTAL)


