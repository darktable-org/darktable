include_directories("${CMAKE_CURRENT_BINARY_DIR}/../" "${CMAKE_CURRENT_SOURCE_DIR}")

include(manage-symbol-visibility)

add_definitions(-include common/module_api.h)
add_definitions(-include iop/iop_api.h)

set(_iop_list "")
set(_iop_default_visible_list "")

# parameters:
# 1. iop name
# 2. iop main source file
# and after that you may pass additional iop source files
# and DEFAULT_VISIBLE keyword - which means that the iop will be visible/listed
# by default (user can manipulate iop visibility via "more modules" lib)
macro (add_iop _lib _src)
  string(LENGTH ${_lib} _lib_namelength)
  if(${_lib_namelength} GREATER 19)
    message(FATAL_ERROR "IOP name \"${_lib}\" is too long (${_lib_namelength} symbols), it should not be greater than 19 symbols.")
  endif(${_lib_namelength} GREATER 19)

  set(_input ${CMAKE_CURRENT_SOURCE_DIR}/${_src})
  set(_output ${CMAKE_CURRENT_BINARY_DIR}/introspection_${_src}) # keep ${_src} in the end to keep the file extension (c vs. cc)
  add_custom_command(
    DEPENDS ${_input} ${CMAKE_SOURCE_DIR}/src/common/introspection.h
    OUTPUT ${_output}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tools/introspection/
    COMMAND ${perl_BIN} ${CMAKE_SOURCE_DIR}/tools/introspection/parser.pl ${CMAKE_CURRENT_SOURCE_DIR}/../ ${_input} ${_output}
  )

  # do not want compiler to look for "DEFAULT_VISIBLE"
  set(OPT_SRC "${ARGN}")
  list(REMOVE_ITEM OPT_SRC "DEFAULT_VISIBLE")

  set_source_files_properties(${_input} PROPERTIES LANGUAGE "")
  # yes, input is added as part of the library. since we just set it's LANGUAGE
  # to "", no compilation will happen. this is needed for proper IDE support.
  add_library(${_lib} MODULE ${_output} ${OPT_SRC} ${_input})
  target_link_libraries(${_lib} lib_darktable)

  if (WIN32)
    _detach_debuginfo (${_lib} ${CMAKE_INSTALL_LIBDIR}/darktable/plugins)
  else()
      # Note that $ORIGIN is not a variable but has a special meaning at runtime.
      # The string "$ORIGIN" should end up in the executable as-is.
      set(RPATH_DT "$ORIGIN")
      if (APPLE)
          # The string "@loader_path" should end up in the executable as-is.
          set(RPATH_DT "@loader_path")
      endif()
      set_target_properties(${_lib}
                            PROPERTIES
                            INSTALL_RPATH ${RPATH_DT}/../${CMAKE_INSTALL_LIBDIR}/darktable)
  endif(WIN32)

  install(TARGETS  ${_lib} DESTINATION ${CMAKE_INSTALL_LIBDIR}/darktable/plugins COMPONENT DTApplication)

  list(APPEND _iop_list ${_lib})

  # find whether magic keyword DEFAULT_VISIBLE was passed
  # it defines whether the iop will be listed by default
  cmake_parse_arguments(add_iop "DEFAULT_VISIBLE" "" "" ${ARGN})
  if(${add_iop_DEFAULT_VISIBLE})
    list(APPEND _iop_default_visible_list ${_lib})
  endif(${add_iop_DEFAULT_VISIBLE})
endmacro (add_iop)

# add_iop(useless "useless.c")
add_iop(rawprepare "rawprepare.c")
add_iop(soften "soften.c")
add_iop(bloom "bloom.c")
add_iop(highpass "highpass.c")
add_iop(lowpass "lowpass.c")
add_iop(shadhi "shadhi.c" DEFAULT_VISIBLE)
add_iop(colorreconstruct "colorreconstruction.c")
add_iop(tonemap "tonemap.cc")
add_iop(tonecurve "tonecurve.c" DEFAULT_VISIBLE)
add_iop(colisa "colisa.c" DEFAULT_VISIBLE)
add_iop(gamma "gamma.c")
add_iop(temperature "temperature.c" DEFAULT_VISIBLE)
add_iop(colorcorrection "colorcorrection.c" DEFAULT_VISIBLE)
add_iop(exposure "exposure.c" DEFAULT_VISIBLE)
add_iop(equalizer "equalizer.c")
add_iop(colorbalance "colorbalance.c")
add_iop(colorin "colorin.c" DEFAULT_VISIBLE)
add_iop(colorout "colorout.c" DEFAULT_VISIBLE)
add_iop(colorchecker "colorchecker.c")
add_iop(clipping "clipping.c" DEFAULT_VISIBLE)
add_iop(sharpen "sharpen.c" DEFAULT_VISIBLE)
add_iop(dither "dither.c")
add_iop(monochrome "monochrome.c" DEFAULT_VISIBLE)
add_iop(basecurve "basecurve.c" DEFAULT_VISIBLE)
add_iop(colorzones "colorzones.c")
add_iop(highlights "highlights.c" DEFAULT_VISIBLE)
add_iop(rawoverexposed "rawoverexposed.c")
add_iop(velvia "velvia.c")
add_iop(vignette "vignette.c" DEFAULT_VISIBLE)
add_iop(splittoning "splittoning.c")
add_iop(grain "grain.c" DEFAULT_VISIBLE)
add_iop(clahe "clahe.c" DEFAULT_VISIBLE)
add_iop(bilateral "bilateral.cc")
add_iop(profile_gamma "profile_gamma.c")
add_iop(colortransfer "colortransfer.c")
add_iop(colormapping "colormapping.c")
add_iop(channelmixer "channelmixer.c")
add_iop(graduatednd "graduatednd.c" DEFAULT_VISIBLE)
add_iop(relight "relight.c")
add_iop(zonesystem "zonesystem.c")
add_iop(demosaic "demosaic.c" "amaze_demosaic_RT.cc" DEFAULT_VISIBLE)
add_iop(rotatepixels "rotatepixels.c")
add_iop(scalepixels "scalepixels.c")
add_iop(atrous "atrous.c")
add_iop(cacorrect "cacorrect.c")
add_iop(overexposed "overexposed.c")
add_iop(hotpixels "hotpixels.c")
add_iop(lowlight "lowlight.c")
add_iop(spots "spots.c")
add_iop(retouch "retouch.c")
add_iop(liquify "liquify.c")
add_iop(rawdenoise "rawdenoise.c")
add_iop(borders "borders.c")
add_iop(nlmeans "nlmeans.c")
add_iop(colorcontrast "colorcontrast.c")
add_iop(levels "levels.c" DEFAULT_VISIBLE)
add_iop(colorize "colorize.c")
add_iop(invert "invert.c")
add_iop(vibrance "vibrance.c")
add_iop(flip "flip.c" DEFAULT_VISIBLE)
add_iop(finalscale "finalscale.c")
add_iop(globaltonemap "globaltonemap.c")
add_iop(bilat "bilat.c")
add_iop(denoiseprofile "denoiseprofile.c")
add_iop(defringe "defringe.c")
add_iop(ashift "ashift.c")
add_iop(hazeremoval "hazeremoval.c")
add_iop(filmic "filmic.c" DEFAULT_VISIBLE)

if(RSVG2_FOUND)
  add_iop(watermark "watermark.c")
endif(RSVG2_FOUND)

if(LENSFUN_FOUND)
  add_iop(lens "lens.c" DEFAULT_VISIBLE)
endif(LENSFUN_FOUND)

# fix for Mac when OpenMP is only available in C compiler
if(APPLE)
  set_target_properties(demosaic PROPERTIES LINKER_LANGUAGE C)
endif(APPLE)

set_property(GLOBAL PROPERTY DT_PLUGIN_IOPS ${_iop_list})
set_property(GLOBAL PROPERTY DT_PLUGIN_IOPS_VISIBLE_BY_DEFAULT ${_iop_default_visible_list})
