# FindONNXRuntime.cmake
#
# Find ONNX Runtime pre-built binaries, downloading them automatically from
# NuGet (Linux, Windows) if not found. Searches these locations in order:
#
#   1. Source tree:  ${CMAKE_SOURCE_DIR}/src/external/onnxruntime/
#      (manually pre-installed; include/ + lib/ or extracted NuGet package)
#   2. Build tree:   ${CMAKE_BINARY_DIR}/_deps/onnxruntime/
#      (auto-download destination)
#   3. System:       e.g. libonnxruntime-dev on Linux, Homebrew on macOS
#
# After this module completes the imported target onnxruntime::onnxruntime is
# available for linking, along with the standard variables:
#
#   ONNXRuntime_FOUND          - TRUE if found
#   ONNXRuntime_INCLUDE_DIRS   - include directories
#   ONNXRuntime_LIBRARIES      - libraries to link
#   ONNXRuntime_LIB_DIR        - directory containing the shared library
#
# Cache variables that influence behaviour:
#
#   ONNXRUNTIME_VERSION          - CPU NuGet version for Linux (default 1.24.4)
#   ONNXRUNTIME_DIRECTML_VERSION - DirectML NuGet version for Windows (default 1.24.4)
#   ONNXRUNTIME_SHA512           - SHA512 (hex) of the package, only needed for
#                                  versions without a pinned hash (default empty)
#   ONNXRUNTIME_OFFLINE          - if TRUE, never attempt a download (default OFF)

# Skip if already found (avoid re-entry issues with cached variables)
if(ONNXRuntime_FOUND)
  return()
endif()

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
set(ONNXRUNTIME_VERSION "1.24.4" CACHE STRING "ONNX Runtime NuGet version to download on Linux")
set(ONNXRUNTIME_DIRECTML_VERSION "1.24.4" CACHE STRING "DirectML NuGet package version for Windows")
set(ONNXRUNTIME_SHA512 "" CACHE STRING "SHA512 (hex) of the ONNX Runtime NuGet package for versions without a pinned hash")
option(ONNXRUNTIME_OFFLINE "Disable automatic download of ONNX Runtime" OFF)

set(_ORT_SRC_ROOT "${CMAKE_SOURCE_DIR}/src/external/onnxruntime")
set(_ORT_BUILD_ROOT "${CMAKE_BINARY_DIR}/_deps/onnxruntime")

# Invalidate cached results if the files no longer exist on disk
# (prevents stale paths from a previous build or deleted installation)
if(_ORT_HEADER AND NOT EXISTS "${_ORT_HEADER}/onnxruntime_c_api.h")
  unset(_ORT_HEADER CACHE)
endif()
if(_ORT_LIBRARY AND NOT EXISTS "${_ORT_LIBRARY}")
  unset(_ORT_LIBRARY CACHE)
endif()

# ---------------------------------------------------------------------------
# Search for existing installation (source tree first, then build tree)
# ---------------------------------------------------------------------------
macro(_ort_find_at ROOT)
  # Standard layout: include/ + lib/
  find_path(_ORT_HEADER
    NAMES onnxruntime_c_api.h
    PATHS "${ROOT}/include"
    NO_DEFAULT_PATH
  )
  find_library(_ORT_LIBRARY
    NAMES onnxruntime
    PATHS "${ROOT}/lib"
    NO_DEFAULT_PATH
  )
  # NuGet layout: build/native/include/ + runtimes/<RID>/native/
  if(NOT _ORT_HEADER)
    find_path(_ORT_HEADER
      NAMES onnxruntime_c_api.h
      PATHS "${ROOT}/build/native/include"
      NO_DEFAULT_PATH
    )
  endif()
  if(NOT _ORT_LIBRARY AND WIN32)
    find_library(_ORT_LIBRARY
      NAMES onnxruntime
      PATHS "${ROOT}/runtimes/win-x64/native" "${ROOT}/runtimes/win-arm64/native"
      NO_DEFAULT_PATH
    )
  elseif(NOT _ORT_LIBRARY AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
      set(_ORT_FIND_RID "linux-arm64")
    else()
      set(_ORT_FIND_RID "linux-x64")
    endif()
    find_library(_ORT_LIBRARY
      NAMES onnxruntime
      PATHS "${ROOT}/runtimes/${_ORT_FIND_RID}/native"
      NO_DEFAULT_PATH
    )
  endif()
  if(_ORT_HEADER AND _ORT_LIBRARY)
    set(_ORT_ROOT "${ROOT}")
  endif()
endmacro()

# Try source tree (manually pre-installed)
_ort_find_at("${_ORT_SRC_ROOT}")

# Try build tree (populated by prior auto-download)
if(NOT _ORT_HEADER OR NOT _ORT_LIBRARY)
  unset(_ORT_HEADER CACHE)
  unset(_ORT_LIBRARY CACHE)
  _ort_find_at("${_ORT_BUILD_ROOT}")
endif()

# Try system-installed package (e.g. libonnxruntime-dev on Linux)
if(NOT _ORT_HEADER OR NOT _ORT_LIBRARY)
  unset(_ORT_HEADER CACHE)
  unset(_ORT_LIBRARY CACHE)
  find_path(_ORT_HEADER
    NAMES onnxruntime_c_api.h
    PATH_SUFFIXES onnxruntime
  )
  find_library(_ORT_LIBRARY NAMES onnxruntime)
  if(_ORT_HEADER AND _ORT_LIBRARY)
    get_filename_component(_ORT_INC_DIR "${_ORT_HEADER}" DIRECTORY)
    get_filename_component(_ORT_ROOT "${_ORT_INC_DIR}" DIRECTORY)
    set(_ORT_SYSTEM_PACKAGE TRUE)
    message(STATUS "Found system ONNX Runtime: ${_ORT_LIBRARY}")
  endif()
endif()

# ---------------------------------------------------------------------------
# Auto-download if not found
# ---------------------------------------------------------------------------
# All platforms download the official NuGet package from nuget.org (NuGet
# packages are ZIP files). The SHA512 of every supported package version is
# pinned below, so a compromised or replaced upstream package is rejected.
#
# To pin a new version, compute the hash of the .nupkg and add a line below:
#
#   curl -sSfL https://api.nuget.org/v3-flatcontainer/<id>/<ver>/<id>.<ver>.nupkg | sha512sum
#
# and cross-check it against the hash published in the NuGet catalog:
#
#   curl -sSfL https://api.nuget.org/v3/registration5-semver1/<id>/<ver>.json \
#     | jq -r .catalogEntry | xargs curl -sSfL | jq -r .packageHash \
#     | base64 -d | od -An -tx1 | tr -d ' \n'
#
# <id> is microsoft.ml.onnxruntime (Linux) or microsoft.ml.onnxruntime.directml
# (Windows).
set(_ORT_SHA512_microsoft.ml.onnxruntime_1.24.4
  "f5dd415dfcafcb3a7461f10a08f0337ea22c1ba8f8af81316daabf7496075add181aecb0de3cabebebdb9f5da3afbe507480aaf34b76e1189409088ccc5c2eac")
set(_ORT_SHA512_microsoft.ml.onnxruntime.directml_1.24.4
  "6633b2bf8f79be17d55e84c9a76bad4729fc8abd53148bc28f407d24ff106460574b4a05c7e958ed9099a927675528505fa12dc588b64f754eb585dc9814d5e0")

if(NOT _ORT_HEADER OR NOT _ORT_LIBRARY)
  if(ONNXRUNTIME_OFFLINE)
    message(STATUS "ONNX Runtime not found and ONNXRUNTIME_OFFLINE is ON.")
  else()
  # -- Determine package for current platform --
  set(_ORT_SUPPORTED TRUE)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # CPU package; contains runtimes/linux-x64 and runtimes/linux-arm64
    set(_ORT_VER "${ONNXRUNTIME_VERSION}")
    set(_ORT_NUGET_ID "microsoft.ml.onnxruntime")
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
      set(_ORT_NUGET_RID "linux-x64")
    elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
      set(_ORT_NUGET_RID "linux-arm64")
    else()
      message(FATAL_ERROR "Unsupported Linux architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    # DirectML package for GPU support (AMD/Intel/NVIDIA via DirectX 12)
    set(_ORT_VER "${ONNXRUNTIME_DIRECTML_VERSION}")
    set(_ORT_NUGET_ID "microsoft.ml.onnxruntime.directml")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
      set(_ORT_NUGET_RID "win-x64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|aarch64")
      set(_ORT_NUGET_RID "win-arm64")
    else()
      message(FATAL_ERROR "Unsupported Windows architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(_ORT_SUPPORTED FALSE)
    message(WARNING
      "ONNX Runtime is not downloaded automatically on macOS. "
      "Install it with 'brew install onnxruntime'.")
  else()
    message(FATAL_ERROR "Unsupported OS for ONNX Runtime auto-download: ${CMAKE_SYSTEM_NAME}")
  endif()

  if(_ORT_SUPPORTED)
  set(_ORT_PACKAGE "${_ORT_NUGET_ID}.${_ORT_VER}.nupkg")
  set(_ORT_URL "https://api.nuget.org/v3-flatcontainer/${_ORT_NUGET_ID}/${_ORT_VER}/${_ORT_PACKAGE}")
  set(_ORT_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_deps")
  set(_ORT_ARCHIVE "${_ORT_DOWNLOAD_DIR}/${_ORT_PACKAGE}")
  file(MAKE_DIRECTORY "${_ORT_DOWNLOAD_DIR}")

  # -- Integrity hash: user-supplied (ONNXRUNTIME_SHA512) or pinned table above --
  if(ONNXRUNTIME_SHA512)
    set(_ORT_HASH "${ONNXRUNTIME_SHA512}")
  else()
    set(_ORT_HASH "${_ORT_SHA512_${_ORT_NUGET_ID}_${_ORT_VER}}")
  endif()
  string(TOLOWER "${_ORT_HASH}" _ORT_HASH)

  string(LENGTH "${_ORT_HASH}" _ORT_HASH_LEN)
  if(NOT _ORT_HASH MATCHES "^[0-9a-f]+$" OR NOT _ORT_HASH_LEN EQUAL 128)
    message(WARNING
      "No valid pinned SHA512 for ${_ORT_PACKAGE}. "
      "Refusing to download without verification. "
      "Set ONNXRUNTIME_SHA512 to the package's SHA512 (hex), "
      "install ONNX Runtime manually, or set ONNXRUNTIME_OFFLINE=ON.")
  else() # valid hash — proceed with verified download

  # -- Verify cached archive if it exists --
  if(EXISTS "${_ORT_ARCHIVE}")
    file(SHA512 "${_ORT_ARCHIVE}" _ORT_CACHED_HASH)
    if(NOT _ORT_CACHED_HASH STREQUAL "${_ORT_HASH}")
      message(STATUS "Cached ONNX Runtime archive has wrong checksum, re-downloading...")
      file(REMOVE "${_ORT_ARCHIVE}")
    endif()
  endif()

  # -- Download --
  set(_ORT_DL_OK TRUE)
  if(NOT EXISTS "${_ORT_ARCHIVE}")
    message(STATUS "Downloading ONNX Runtime ${_ORT_VER} (${_ORT_PACKAGE}) from NuGet...")
    file(DOWNLOAD
      "${_ORT_URL}"
      "${_ORT_ARCHIVE}"
      STATUS _ORT_DL_STATUS
      SHOW_PROGRESS
      EXPECTED_HASH "SHA512=${_ORT_HASH}"
    )
    list(GET _ORT_DL_STATUS 0 _ORT_DL_CODE)
    list(GET _ORT_DL_STATUS 1 _ORT_DL_MSG)
    if(NOT _ORT_DL_CODE EQUAL 0)
      file(REMOVE "${_ORT_ARCHIVE}")
      set(_ORT_DL_OK FALSE)
      message(WARNING
        "Failed to download ONNX Runtime from ${_ORT_URL}: ${_ORT_DL_MSG}. "
        "Install ONNX Runtime manually or set ONNXRUNTIME_OFFLINE=ON.")
    endif()
  endif()

  if(_ORT_DL_OK)
  # -- Extract to a temporary directory --
  set(_ORT_EXTRACT_DIR "${_ORT_DOWNLOAD_DIR}/onnxruntime-extract")
  if(EXISTS "${_ORT_EXTRACT_DIR}")
    file(REMOVE_RECURSE "${_ORT_EXTRACT_DIR}")
  endif()
  message(STATUS "Extracting ${_ORT_PACKAGE}...")
  file(ARCHIVE_EXTRACT
    INPUT "${_ORT_ARCHIVE}"
    DESTINATION "${_ORT_EXTRACT_DIR}"
  )

  # -- Install into build tree --
  # NuGet package layout:
  #   build/native/include/  -> headers
  #   runtimes/<RID>/native/ -> shared libraries (and .lib files on Windows)
  if(EXISTS "${_ORT_BUILD_ROOT}")
    file(REMOVE_RECURSE "${_ORT_BUILD_ROOT}")
  endif()
  file(MAKE_DIRECTORY "${_ORT_BUILD_ROOT}/include")
  file(MAKE_DIRECTORY "${_ORT_BUILD_ROOT}/lib")
  file(GLOB _ORT_NUGET_HEADERS "${_ORT_EXTRACT_DIR}/build/native/include/*")
  file(GLOB _ORT_NUGET_LIBS "${_ORT_EXTRACT_DIR}/runtimes/${_ORT_NUGET_RID}/native/*")
  if(NOT _ORT_NUGET_HEADERS OR NOT _ORT_NUGET_LIBS)
    message(WARNING
      "Unexpected layout in ${_ORT_PACKAGE}: missing build/native/include "
      "or runtimes/${_ORT_NUGET_RID}/native.")
  else()
    file(COPY ${_ORT_NUGET_HEADERS} DESTINATION "${_ORT_BUILD_ROOT}/include")
    file(COPY ${_ORT_NUGET_LIBS} DESTINATION "${_ORT_BUILD_ROOT}/lib")
  endif()

  # -- Cleanup extraction directory --
  file(REMOVE_RECURSE "${_ORT_EXTRACT_DIR}")

  # -- Set root and re-search --
  set(_ORT_ROOT "${_ORT_BUILD_ROOT}")

  unset(_ORT_HEADER CACHE)
  unset(_ORT_LIBRARY CACHE)

  find_path(_ORT_HEADER
    NAMES onnxruntime_c_api.h
    PATHS "${_ORT_ROOT}/include"
    NO_DEFAULT_PATH
  )
  find_library(_ORT_LIBRARY
    NAMES onnxruntime
    PATHS "${_ORT_ROOT}/lib"
    NO_DEFAULT_PATH
  )
  endif() # _ORT_DL_OK
  endif() # valid hash
  endif() # _ORT_SUPPORTED
  endif() # NOT ONNXRUNTIME_OFFLINE
endif()

# ---------------------------------------------------------------------------
# Create onnxruntime::onnxruntime imported target
# ---------------------------------------------------------------------------
if(_ORT_HEADER AND _ORT_LIBRARY AND NOT TARGET onnxruntime::onnxruntime)

  # _ORT_HEADER is the directory where find_path() located onnxruntime_c_api.h.
  # Use it directly — it's correct for all layouts (GitHub releases: include/,
  # NuGet: build/native/include/, system: /usr/include/onnxruntime).
  set(_ORT_INCLUDE_DIR "${_ORT_HEADER}")

  # Try the shipped CMake config files first (they exist under lib/cmake/)
  set(_ORT_CMAKE_DIR "${_ORT_ROOT}/lib/cmake/onnxruntime")
  set(_ORT_CMAKE_TARGETS "${_ORT_CMAKE_DIR}/onnxruntimeTargets.cmake")

  # Patch the known include-path bug before loading the config
  if(EXISTS "${_ORT_CMAKE_TARGETS}")
    file(READ "${_ORT_CMAKE_TARGETS}" _ORT_TARGETS_CONTENT)
    if(_ORT_TARGETS_CONTENT MATCHES "include/onnxruntime"
       AND NOT EXISTS "${_ORT_ROOT}/include/onnxruntime")
      string(REPLACE "/include/onnxruntime" "/include"
        _ORT_TARGETS_CONTENT "${_ORT_TARGETS_CONTENT}")
      file(WRITE "${_ORT_CMAKE_TARGETS}" "${_ORT_TARGETS_CONTENT}")
      message(STATUS "Patched ONNX Runtime CMake config: include/onnxruntime -> include")
    endif()
  endif()

  # Patch the lib64 vs lib path issue in CMake targets
  file(GLOB _ORT_CMAKE_TARGET_FILES "${_ORT_CMAKE_DIR}/onnxruntimeTargets*.cmake")
  foreach(_ORT_TARGET_FILE ${_ORT_CMAKE_TARGET_FILES})
    if(EXISTS "${_ORT_TARGET_FILE}")
      file(READ "${_ORT_TARGET_FILE}" _ORT_TARGET_CONTENT)
      if(_ORT_TARGET_CONTENT MATCHES "/lib64/"
         AND NOT EXISTS "${_ORT_ROOT}/lib64")
        string(REPLACE "/lib64/" "/lib/" _ORT_TARGET_CONTENT "${_ORT_TARGET_CONTENT}")
        file(WRITE "${_ORT_TARGET_FILE}" "${_ORT_TARGET_CONTENT}")
        message(STATUS "Patched ONNX Runtime CMake config: lib64 -> lib")
      endif()
    endif()
  endforeach()

  find_package(onnxruntime QUIET
    PATHS "${_ORT_ROOT}"
    NO_DEFAULT_PATH
  )

  # Fallback: create the imported target manually
  if(NOT TARGET onnxruntime::onnxruntime)
    message(STATUS "Creating onnxruntime::onnxruntime imported target manually")
    add_library(onnxruntime::onnxruntime SHARED IMPORTED)
    set_target_properties(onnxruntime::onnxruntime PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${_ORT_INCLUDE_DIR}"
    )
    if(WIN32)
      # On Windows find_library finds the .lib import library;
      # IMPORTED_IMPLIB = .lib, IMPORTED_LOCATION = .dll
      set_target_properties(onnxruntime::onnxruntime PROPERTIES
        IMPORTED_IMPLIB "${_ORT_LIBRARY}"
      )
      # Search both standard (lib/) and NuGet (runtimes/<RID>/native/) layouts
      find_file(_ORT_DLL NAMES onnxruntime.dll
        PATHS "${_ORT_ROOT}/lib"
              "${_ORT_ROOT}/runtimes/win-x64/native"
              "${_ORT_ROOT}/runtimes/win-arm64/native"
        NO_DEFAULT_PATH
      )
      if(_ORT_DLL)
        set_target_properties(onnxruntime::onnxruntime PROPERTIES
          IMPORTED_LOCATION "${_ORT_DLL}"
        )
      endif()
    else()
      set_target_properties(onnxruntime::onnxruntime PROPERTIES
        IMPORTED_LOCATION "${_ORT_LIBRARY}"
      )
      if(APPLE)
        get_filename_component(_ORT_LIB_NAME "${_ORT_LIBRARY}" NAME)
        set_target_properties(onnxruntime::onnxruntime PROPERTIES
          IMPORTED_SONAME "@rpath/${_ORT_LIB_NAME}"
        )
      endif()
    endif()
  endif()
endif()

# ---------------------------------------------------------------------------
# Standard find_package result handling
# ---------------------------------------------------------------------------
mark_as_advanced(_ORT_HEADER _ORT_LIBRARY)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(ONNXRuntime
  REQUIRED_VARS _ORT_LIBRARY _ORT_HEADER
)

if(ONNXRuntime_FOUND)
  set(ONNXRuntime_INCLUDE_DIRS "${_ORT_INCLUDE_DIR}")
  set(ONNXRuntime_LIBRARIES "${_ORT_LIBRARY}")
  # Derive library directory from the actual found library path
  # (works for all layouts: standard lib/, NuGet runtimes/<RID>/native/, system)
  get_filename_component(ONNXRuntime_LIB_DIR "${_ORT_LIBRARY}" DIRECTORY)
  set(ONNXRuntime_LIB_DIR "${ONNXRuntime_LIB_DIR}" CACHE PATH "ONNX Runtime library directory")
  set(ONNXRuntime_SYSTEM_PACKAGE ${_ORT_SYSTEM_PACKAGE} CACHE BOOL "Using system ONNX Runtime package")
  mark_as_advanced(ONNXRuntime_LIB_DIR ONNXRuntime_SYSTEM_PACKAGE)
endif()
