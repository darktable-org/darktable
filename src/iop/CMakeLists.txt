cmake_minimum_required(VERSION 2.6)

include_directories("${CMAKE_CURRENT_BINARY_DIR}/../" "${CMAKE_CURRENT_SOURCE_DIR}")

set(MODULES tonecurve gamma temperature colorcorrection exposure equalizer
	colorin colorout clipping sharpen monochrome rawimport basecurve colorzones
	basecurve highlights velvia vignette splittoning grain clahe bilateral profile_gamma
	colortransfer channelmixer graduatednd relight zonesystem watermark
)

add_library(tonecurve MODULE "tonecurve.c")
add_library(gamma MODULE "gamma.c")
add_library(temperature MODULE "temperature.c")
add_library(colorcorrection MODULE "colorcorrection.c")
add_library(exposure MODULE "exposure.c")
add_library(equalizer MODULE "equalizer.c")
add_library(colorin MODULE "colorin.c")
add_library(colorout MODULE "colorout.c")
add_library(clipping MODULE "clipping.c")
add_library(sharpen MODULE "sharpen.c")
add_library(monochrome MODULE "monochrome.c")
add_library(rawimport MODULE "rawimport.c")
add_library(basecurve MODULE "basecurve.c")
add_library(colorzones MODULE "colorzones.c")
add_library(highlights MODULE "highlights.c")
add_library(velvia MODULE "velvia.c")
add_library(vignette MODULE "vignette.c")
add_library(splittoning MODULE "splittoning.c")
add_library(grain MODULE "grain.c")
add_library(clahe MODULE "clahe.c")
add_library(bilateral MODULE "bilateral.cc")
add_library(profile_gamma MODULE "profile_gamma.c")
add_library(colortransfer MODULE "colortransfer.c")
add_library(channelmixer MODULE "channelmixer.c")
add_library(graduatednd MODULE "graduatednd.c")
add_library(relight MODULE "relight.c")
add_library(zonesystem MODULE "zonesystem.c")
add_library(watermark MODULE "watermark.c")

foreach(module ${MODULES})
	install(TARGETS  ${module} DESTINATION lib/darktable/plugins)
endforeach(module)

