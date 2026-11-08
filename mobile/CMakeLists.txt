# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.22.1)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(dt_mobile LANGUAGES C CXX)
endif()

option(DT_MOBILE_BUILD_TESTS "Build host mobile API tests" OFF)
# The standalone probe owns its RawSpeed target. A root-engine build already
# owns that target and discovers its dependencies through the desktop graph.
set(_dt_mobile_dependency_probe_default ${ANDROID})
if(TARGET lib_darktable)
  set(_dt_mobile_dependency_probe_default OFF)
endif()
option(DT_MOBILE_ANDROID_DEPENDENCIES "Link the standalone pinned Android native dependency probe bundle" ${_dt_mobile_dependency_probe_default})
option(DT_MOBILE_WITH_DARKTABLE_ENGINE "Enable the upstream darktable processing engine" ${ANDROID})
option(DT_MOBILE_WITH_GPR_SDK "Build the optional GoPro SDK reference decoder and verification tests" OFF)

add_library(dt_mobile_core STATIC core/dt_mobile.c)
target_include_directories(dt_mobile_core PUBLIC include)
target_compile_features(dt_mobile_core PRIVATE c_std_11)

if(DT_MOBILE_WITH_GPR_SDK)
  include(cmake/GprSdk.cmake)
  target_link_libraries(dt_mobile_core PUBLIC dt_mobile_gpr)
  target_compile_definitions(dt_mobile_core PRIVATE DT_MOBILE_HAS_GPR=1)
endif()

if(DT_MOBILE_WITH_DARKTABLE_ENGINE)
  if(NOT TARGET lib_darktable)
    message(FATAL_ERROR "DT_MOBILE_WITH_DARKTABLE_ENGINE requires the upstream lib_darktable target; include mobile/ from the darktable build")
  endif()
  target_sources(dt_mobile_core PRIVATE core/darktable_mobile_engine.c core/dt_mobile_export.c)
  # The adapter uses the same internal headers as the desktop library. Their
  # compile context includes GTK/GLib, generated config.h and feature macros;
  # linking the library alone does not propagate its private directory context.
  target_include_directories(dt_mobile_core PRIVATE core
    "$<TARGET_PROPERTY:lib_darktable,INCLUDE_DIRECTORIES>"
    "${DARKTABLE_BINDIR}")
  target_compile_definitions(dt_mobile_core PRIVATE
    "$<TARGET_PROPERTY:lib_darktable,COMPILE_DEFINITIONS>")
  target_compile_options(dt_mobile_core PRIVATE
    "$<TARGET_PROPERTY:lib_darktable,COMPILE_OPTIONS>")
  # The pinned desktop RawSpeed already decodes DNG compression 9 (GoPro VC-5).
  target_compile_definitions(dt_mobile_core PRIVATE
    DT_MOBILE_HAS_DARKTABLE_ENGINE=1 DT_MOBILE_HAS_RAWSPEED_GPR=1
    # src/ adds this through its directory-local CMAKE_C_FLAGS_DEBUG, so it is
    # not part of the target properties copied above. It changes dt_develop_t's
    # embedded mutex layout and must match the desktop library's ABI.
    "$<$<CONFIG:Debug>:_DEBUG>")
  target_link_libraries(dt_mobile_core PUBLIC lib_darktable)
  # Processing and format modules are opened dynamically and are invisible to
  # the JNI link dependency graph. Build the whole runtime when building the
  # adapter, including configurations which request only the JNI target.
  get_property(_dt_mobile_runtime_iops GLOBAL PROPERTY DT_PLUGIN_IOPS)
  get_property(_dt_mobile_runtime_formats GLOBAL PROPERTY DT_PLUGIN_FORMATS)
  get_property(_dt_mobile_runtime_storages GLOBAL PROPERTY DT_PLUGIN_STORAGES)
  add_custom_target(dt_mobile_modules)
  add_dependencies(dt_mobile_modules lib_darktable
    ${_dt_mobile_runtime_iops} ${_dt_mobile_runtime_formats} ${_dt_mobile_runtime_storages}
    deploy darktablerc_file)
  if(TARGET locales)
    # The normal installation also installs these generated runtime files.
    add_dependencies(dt_mobile_modules locales)
  endif()
  add_dependencies(dt_mobile_core dt_mobile_modules)
  # Keep the public runtime target complete without making the core depend on
  # the JNI library which in turn links the core.
  add_custom_target(dt_mobile_runtime)
  add_dependencies(dt_mobile_runtime dt_mobile_core dt_mobile_modules)
  if(USE_OPENMP)
    target_link_libraries(dt_mobile_core PRIVATE OpenMP::OpenMP_C)
  endif()
endif()

if(DT_MOBILE_ANDROID_DEPENDENCIES)
  if(NOT ANDROID)
    message(FATAL_ERROR "The mobile dependency bundle is Android-only; disable DT_MOBILE_ANDROID_DEPENDENCIES for host tests")
  endif()
  if(TARGET rawspeed)
    message(FATAL_ERROR "DT_MOBILE_ANDROID_DEPENDENCIES is a standalone probe; disable it when building the root lib_darktable graph, which already owns RawSpeed")
  endif()
  include(cmake/AndroidDependencies.cmake)
  add_library(dt_mobile_dependency_probe STATIC core/android_dependency_probe.cpp)
  target_compile_features(dt_mobile_dependency_probe PRIVATE cxx_std_17)
  target_link_libraries(dt_mobile_dependency_probe PUBLIC dt_mobile_android_dependencies)
  target_link_libraries(dt_mobile_core PUBLIC dt_mobile_dependency_probe)
  target_compile_definitions(dt_mobile_core PRIVATE DT_MOBILE_HAS_ANDROID_DEPENDENCIES=1)
endif()

if(ANDROID)
  add_library(dt_mobile SHARED jni/dt_mobile_jni.c)
  target_link_libraries(dt_mobile PRIVATE dt_mobile_core log)
  if(TARGET dt_mobile_runtime)
    add_dependencies(dt_mobile_runtime dt_mobile)
  endif()
  target_compile_options(dt_mobile PRIVATE -Wall -Wextra -Werror)
  set_target_properties(dt_mobile PROPERTIES INSTALL_RPATH "$ORIGIN")
  if(CMAKE_INSTALL_LIBDIR)
    set(_dt_mobile_install_libdir "${CMAKE_INSTALL_LIBDIR}")
  else()
    set(_dt_mobile_install_libdir lib)
  endif()
  install(TARGETS dt_mobile LIBRARY DESTINATION "${_dt_mobile_install_libdir}/darktable"
    COMPONENT DTApplication)
  if(DT_MOBILE_WITH_DARKTABLE_ENGINE)
    # A staging consumer must verify every dynamically loaded plugin from this
    # configuration, not merely the core's DT_NEEDED link dependencies.
    set(_dt_mobile_library_manifest "")
    foreach(_dt_mobile_runtime_target lib_darktable dt_mobile
        ${_dt_mobile_runtime_iops} ${_dt_mobile_runtime_formats} ${_dt_mobile_runtime_storages})
      string(APPEND _dt_mobile_library_manifest "$<TARGET_FILE_NAME:${_dt_mobile_runtime_target}>\n")
    endforeach()
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/android-runtime-libraries-$<CONFIG>.txt"
      CONTENT "${_dt_mobile_library_manifest}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/android-runtime-libraries-$<CONFIG>.txt"
      DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/darktable-mobile"
      RENAME android-runtime-libraries.txt COMPONENT DTApplication)
  endif()
endif()

if(DT_MOBILE_BUILD_TESTS)
  enable_testing()
  set(_dt_mobile_decoder_timeout 180)
  set(_dt_mobile_raw_engine_timeout 300)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Unoptimized VC-5 and full-resolution pixelpipes are much slower than
    # release builds, especially on two-core verification environments.
    set(_dt_mobile_decoder_timeout 600)
    set(_dt_mobile_raw_engine_timeout 900)
  endif()
  add_executable(dt_mobile_api_test tests/dt_mobile_api_test.c)
  if(DT_MOBILE_WITH_DARKTABLE_ENGINE)
    add_library(dt_mobile_noengine_test_core STATIC core/dt_mobile.c)
    target_include_directories(dt_mobile_noengine_test_core PUBLIC include)
    target_compile_features(dt_mobile_noengine_test_core PRIVATE c_std_11)
    target_link_libraries(dt_mobile_api_test PRIVATE dt_mobile_noengine_test_core)
  else()
    target_link_libraries(dt_mobile_api_test PRIVATE dt_mobile_core)
  endif()
  add_test(NAME dt_mobile_api_test COMMAND dt_mobile_api_test)
  add_executable(dt_mobile_zonemap_test tests/dt_mobile_zonemap_test.c)
  target_include_directories(dt_mobile_zonemap_test PRIVATE ../src)
  target_compile_features(dt_mobile_zonemap_test PRIVATE c_std_11)
  target_compile_options(dt_mobile_zonemap_test PRIVATE -Wall -Wextra -Werror)
  add_test(NAME dt_mobile_zonemap_test COMMAND dt_mobile_zonemap_test)
  # This target substitutes only the adapter boundary, and does not claim RAW
  # decoding coverage. Real pixelpipe and VC-5 tests are separate targets below.
  add_executable(dt_mobile_mock_api_test core/dt_mobile.c
    tests/dt_mobile_mock_engine.c tests/dt_mobile_mock_api_test.c)
  target_include_directories(dt_mobile_mock_api_test PRIVATE include core tests)
  target_compile_features(dt_mobile_mock_api_test PRIVATE c_std_11)
  target_compile_definitions(dt_mobile_mock_api_test PRIVATE
    DT_MOBILE_HAS_DARKTABLE_ENGINE=1)
  target_compile_options(dt_mobile_mock_api_test PRIVATE -Wall -Wextra -Werror)
  add_test(NAME dt_mobile_mock_api_test COMMAND dt_mobile_mock_api_test)
  # The extracted desktop spectral core can be checked without the GTK graph.
  # Standalone API-only builds may omit LCMS2; engine builds already require it.
  find_path(DT_MOBILE_TEST_LCMS_INCLUDE lcms2.h HINTS ${LCMS2_INCLUDE_DIRS})
  find_library(DT_MOBILE_TEST_LCMS_LIBRARY NAMES lcms2 HINTS ${LCMS2_LIBRARY_DIRS})
  if(DT_MOBILE_TEST_LCMS_INCLUDE AND DT_MOBILE_TEST_LCMS_LIBRARY)
    add_executable(dt_mobile_temperature_test tests/dt_mobile_temperature_test.c
      ../src/common/whitebalance.c)
    target_include_directories(dt_mobile_temperature_test PRIVATE ../src ${DT_MOBILE_TEST_LCMS_INCLUDE})
    target_link_libraries(dt_mobile_temperature_test PRIVATE ${DT_MOBILE_TEST_LCMS_LIBRARY} m)
    target_compile_features(dt_mobile_temperature_test PRIVATE c_std_11)
    target_compile_options(dt_mobile_temperature_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME dt_mobile_temperature_test COMMAND dt_mobile_temperature_test)
  endif()
  if(DT_MOBILE_WITH_GPR_SDK)
    add_executable(dt_mobile_gpr_test tests/dt_mobile_gpr_test.cpp)
    target_link_libraries(dt_mobile_gpr_test PRIVATE dt_mobile_gpr dt_mobile_gpr_sdk)
    # SDK round-trip assertions intentionally stay active in release builds.
    target_compile_options(dt_mobile_gpr_test PRIVATE -UNDEBUG)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gpr-test-cache")
    file(GLOB _dt_gpr_samples "${dt_gpr_source_SOURCE_DIR}/data/samples/*/*.GPR")
    if(NOT _dt_gpr_samples)
      message(FATAL_ERROR "GPR verification requires the official SDK RAW samples")
    endif()
    foreach(_dt_gpr_sample IN LISTS _dt_gpr_samples)
      get_filename_component(_dt_gpr_sample_name "${_dt_gpr_sample}" NAME_WE)
      add_test(NAME "dt_mobile_gpr_${_dt_gpr_sample_name}" COMMAND dt_mobile_gpr_test
        "${_dt_gpr_sample}" "${CMAKE_CURRENT_BINARY_DIR}/gpr-test-cache")
      set_tests_properties("dt_mobile_gpr_${_dt_gpr_sample_name}" PROPERTIES TIMEOUT ${_dt_mobile_decoder_timeout})
    endforeach()
  endif()
  if(DT_MOBILE_WITH_DARKTABLE_ENGINE)
    if(NOT ANDROID)
      # Exercise the actual resolver with both desktop and Android data layouts
      # without treating a host platform-macro test as device execution proof.
      foreach(_dt_location_platform IN ITEMS desktop android)
        set(_dt_location_target "dt_mobile_locations_${_dt_location_platform}_test")
        add_executable(${_dt_location_target} tests/dt_mobile_locations_test.c ../src/common/file_location.c)
        target_include_directories(${_dt_location_target} PRIVATE
          "$<TARGET_PROPERTY:dt_mobile_core,INCLUDE_DIRECTORIES>")
        target_compile_definitions(${_dt_location_target} PRIVATE
          "$<TARGET_PROPERTY:dt_mobile_core,COMPILE_DEFINITIONS>")
        target_compile_options(${_dt_location_target} PRIVATE
          "$<TARGET_PROPERTY:dt_mobile_core,COMPILE_OPTIONS>")
        target_link_libraries(${_dt_location_target} PRIVATE lib_darktable)
        if(_dt_location_platform STREQUAL "android")
          target_compile_definitions(${_dt_location_target} PRIVATE __ANDROID__)
        endif()
        add_test(NAME ${_dt_location_target} COMMAND ${_dt_location_target})
      endforeach()
    endif()
    add_executable(dt_mobile_engine_test tests/dt_mobile_engine_test.c)
    target_link_libraries(dt_mobile_engine_test PRIVATE dt_mobile_core)
    # The headless mask-ownership fixture uses real internal develop structs.
    # Match the adapter's context, including the Debug mutex layout.
    target_include_directories(dt_mobile_engine_test PRIVATE
      "$<TARGET_PROPERTY:dt_mobile_core,INCLUDE_DIRECTORIES>")
    target_compile_definitions(dt_mobile_engine_test PRIVATE
      "$<TARGET_PROPERTY:dt_mobile_core,COMPILE_DEFINITIONS>")
    target_compile_options(dt_mobile_engine_test PRIVATE
      "$<TARGET_PROPERTY:dt_mobile_core,COMPILE_OPTIONS>")
    if(USE_OPENMP)
      target_link_libraries(dt_mobile_engine_test PRIVATE OpenMP::OpenMP_C)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
      # Count actual adapter reclamation without a production test hook. Calls
      # inside lib_darktable remain untouched; only the static adapter's calls
      # are wrapped, and the wrapper still invokes the original cleanup.
      target_link_options(dt_mobile_engine_test PRIVATE "LINKER:--wrap=dt_iop_cleanup_module")
      target_compile_definitions(dt_mobile_engine_test PRIVATE DT_MOBILE_TEST_INSTANCE_RECLAMATION=1)
    endif()
    # These modules are loaded dynamically, so the linker cannot express this
    # dependency. Building the smoke test must also build its actual pixelpipe.
    get_property(_dt_mobile_iops GLOBAL PROPERTY DT_PLUGIN_IOPS)
    add_dependencies(dt_mobile_engine_test ${_dt_mobile_iops} jpeg png deploy darktablerc_file)
    if(TARGET gimp_xcf)
      add_dependencies(dt_mobile_engine_test gimp_xcf)
      target_compile_definitions(dt_mobile_engine_test PRIVATE DT_MOBILE_TEST_REQUIRE_XCF=1)
    endif()
    add_test(NAME dt_mobile_engine_test COMMAND dt_mobile_engine_test
      "${CMAKE_CURRENT_SOURCE_DIR}/../data/pixmaps/256x256/darktable.png"
      "${CMAKE_CURRENT_BINARY_DIR}/dt-mobile-engine-output.png")
    set_tests_properties(dt_mobile_engine_test PROPERTIES
      ENVIRONMENT "DT_MOBILE_DATADIR=${DARKTABLE_DATADIR};DT_MOBILE_MODULEDIR=${DARKTABLE_LIBDIR};OMP_NUM_THREADS=2"
      # This fixture covers reviewed scalar/array controls, native form history,
      # file imports and original-pipeline pixel/undo renders. Allow the same
      # coverage in Debug builds and on slower CI runners.
      TIMEOUT 600)
    if(DT_MOBILE_WITH_GPR_SDK)
      add_executable(dt_mobile_rawspeed_gpr_test tests/dt_mobile_rawspeed_gpr_test.cpp)
      target_compile_features(dt_mobile_rawspeed_gpr_test PRIVATE cxx_std_20)
      target_link_libraries(dt_mobile_rawspeed_gpr_test PRIVATE rawspeed dt_mobile_gpr_sdk)
      add_executable(dt_mobile_raw_engine_test tests/dt_mobile_raw_engine_test.c)
      target_link_libraries(dt_mobile_raw_engine_test PRIVATE dt_mobile_core dt_mobile_gpr_sdk)
      add_dependencies(dt_mobile_raw_engine_test ${_dt_mobile_iops} png deploy darktablerc_file)
      foreach(_dt_gpr_sample IN LISTS _dt_gpr_samples)
        get_filename_component(_dt_gpr_sample_name "${_dt_gpr_sample}" NAME_WE)
        add_test(NAME "dt_mobile_rawspeed_${_dt_gpr_sample_name}" COMMAND dt_mobile_rawspeed_gpr_test
          "${_dt_gpr_sample}" "${CMAKE_CURRENT_SOURCE_DIR}/../src/external/rawspeed/data/cameras.xml")
        set_tests_properties("dt_mobile_rawspeed_${_dt_gpr_sample_name}" PROPERTIES
          ENVIRONMENT "OMP_NUM_THREADS=2" TIMEOUT ${_dt_mobile_decoder_timeout} RUN_SERIAL TRUE)
        add_test(NAME "dt_mobile_raw_${_dt_gpr_sample_name}" COMMAND dt_mobile_raw_engine_test
          "${_dt_gpr_sample}" "${CMAKE_CURRENT_BINARY_DIR}/raw-${_dt_gpr_sample_name}.png")
        set_tests_properties("dt_mobile_raw_${_dt_gpr_sample_name}" PROPERTIES
          ENVIRONMENT "DT_MOBILE_DATADIR=${DARKTABLE_DATADIR};DT_MOBILE_MODULEDIR=${DARKTABLE_LIBDIR};OMP_NUM_THREADS=2"
          TIMEOUT ${_dt_mobile_raw_engine_timeout} RUN_SERIAL TRUE)
        if(_dt_gpr_sample_name STREQUAL "GPBK7066")
          add_test(NAME "dt_mobile_raw_whitebalance_${_dt_gpr_sample_name}" COMMAND dt_mobile_raw_engine_test
            "${_dt_gpr_sample}" "${CMAKE_CURRENT_BINARY_DIR}/wb-${_dt_gpr_sample_name}.png" --whitebalance)
          set_tests_properties("dt_mobile_raw_whitebalance_${_dt_gpr_sample_name}" PROPERTIES
            ENVIRONMENT "DT_MOBILE_DATADIR=${DARKTABLE_DATADIR};DT_MOBILE_MODULEDIR=${DARKTABLE_LIBDIR};OMP_NUM_THREADS=2"
            TIMEOUT ${_dt_mobile_raw_engine_timeout} RUN_SERIAL TRUE)
        endif()
      endforeach()
    endif()
  endif()
endif()
