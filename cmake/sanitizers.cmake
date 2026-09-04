# Runtime sanitizer support for darktable.
#
# Driven by the DT_SANITIZE cache variable (see DefineOptions.cmake), which
# takes a comma or semicolon separated list of:
#
#   address    AddressSanitizer, implies LeakSanitizer  (~2x slower, ~3x RSS)
#   undefined  UndefinedBehaviorSanitizer               (~1.2x slower)
#   leak       LeakSanitizer standalone                 (~native speed)
#   thread     ThreadSanitizer                          (~5-15x slower)
#
# Unlike rawspeed, which models this as dedicated CMAKE_BUILD_TYPEs
# (Sanitize/TSan, see src/external/rawspeed/cmake/build-type.cmake), we apply
# the instrumentation as an orthogonal set of compile/link options. That way it
# composes with RelWithDebInfo, it does not need a remap in
# src/external/CMakeLists.txt for rawspeed's build type whitelist, and it does
# not collide with the CMAKE_C_FLAGS_SANITIZE cache entries rawspeed FORCEs.
#
# This file must be included from the top level CMakeLists.txt before
# add_subdirectory(src), so that the directory-inherited options reach both
# darktable itself and everything under src/external.

if(NOT DT_SANITIZE)
  return()
endif()

include(CheckCSourceCompiles)

# ---------------------------------------------------------------------------
# Parse and validate the requested set
# ---------------------------------------------------------------------------

string(TOLOWER "${DT_SANITIZE}" _dt_san_input)
string(REPLACE "," ";" _dt_san_list "${_dt_san_input}")
list(REMOVE_ITEM _dt_san_list "")
list(REMOVE_DUPLICATES _dt_san_list)

set(_dt_san_known address undefined leak thread)
foreach(_san IN LISTS _dt_san_list)
  if(NOT _san IN_LIST _dt_san_known)
    if(_san STREQUAL "memory")
      message(FATAL_ERROR
        "DT_SANITIZE: MemorySanitizer is not supported. It only produces usable "
        "results when every dependency (glib, GTK, lcms2, exiv2, libjpeg, ...) is "
        "MSan-instrumented too; against stock system libraries it reports nothing "
        "but false positives.")
    endif()
    message(FATAL_ERROR
      "DT_SANITIZE: unknown sanitizer '${_san}'. Known values: ${_dt_san_known}")
  endif()
endforeach()

if("thread" IN_LIST _dt_san_list AND "address" IN_LIST _dt_san_list)
  message(FATAL_ERROR "DT_SANITIZE: 'thread' and 'address' are mutually exclusive")
endif()
if("thread" IN_LIST _dt_san_list AND "leak" IN_LIST _dt_san_list)
  message(FATAL_ERROR "DT_SANITIZE: 'thread' and 'leak' are mutually exclusive")
endif()
if("address" IN_LIST _dt_san_list AND "leak" IN_LIST _dt_san_list)
  message(FATAL_ERROR
    "DT_SANITIZE: 'address' already includes LeakSanitizer, drop 'leak'. "
    "Use 'leak' on its own if you want leak checking without ASan's slowdown.")
endif()

# ---------------------------------------------------------------------------
# Assemble the flags
# ---------------------------------------------------------------------------

# Keep stack traces readable and complete.
set(_dt_san_flags -fno-omit-frame-pointer -fno-optimize-sibling-calls)
# -g is already added unconditionally in src/CMakeLists.txt.

if("address" IN_LIST _dt_san_list)
  # -fno-common so that globals get redzones, -U_FORTIFY_SOURCE because the
  # distro default conflicts with ASan's interceptors.
  #
  # -fsanitize-recover=address is what gives ASAN_OPTIONS' halt_on_error=0 any
  # effect. Without it every ASan finding is a hard abort, so one recoverable
  # over-read during start-up ends the run and hides everything behind it. It
  # belongs here rather than only in the 'undefined' branch below, so that a
  # plain -DDT_SANITIZE=address build recovers as well. Genuinely fatal faults
  # (SIGSEGV, allocator failures) still terminate the process.
  list(APPEND _dt_san_flags -fsanitize=address -fno-common -U_FORTIFY_SOURCE
                            -fsanitize-recover=address)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    list(APPEND _dt_san_flags -fsanitize-address-use-after-scope)
  endif()
endif()

if("undefined" IN_LIST _dt_san_list)
  # -fsanitize-recover=all keeps the process alive after a finding, so a single
  # run of the test suite collects every report instead of dying on the first.
  # vptr is off because the dlopen'd C++ modules are built with hidden
  # visibility (cmake/manage-symbol-visibility.cmake), which makes the check
  # unreliable across module boundaries.
  list(APPEND _dt_san_flags -fsanitize=undefined -fno-sanitize=vptr -fsanitize-recover=all)
  if(DT_SANITIZE_EXTRA_CHECKS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
      list(APPEND _dt_san_flags
        -fsanitize=integer,implicit-conversion
        -fno-sanitize=unsigned-shift-base)
    else()
      message(WARNING
        "DT_SANITIZE_EXTRA_CHECKS is clang-only and has no effect with "
        "${CMAKE_C_COMPILER_ID}")
    endif()
  endif()
endif()

if("leak" IN_LIST _dt_san_list)
  list(APPEND _dt_san_flags -fsanitize=leak)
endif()

if("thread" IN_LIST _dt_san_list)
  list(APPEND _dt_san_flags -fsanitize=thread)
endif()

# ---------------------------------------------------------------------------
# Fail early and clearly if the runtime libraries are missing
# ---------------------------------------------------------------------------

string(REPLACE ";" " " _dt_san_flags_str "${_dt_san_flags}")
set(CMAKE_REQUIRED_FLAGS "${_dt_san_flags_str}")
set(CMAKE_REQUIRED_LINK_OPTIONS ${_dt_san_flags})
check_c_source_compiles("int main(void) { return 0; }" DT_SANITIZE_USABLE)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)

if(NOT DT_SANITIZE_USABLE)
  message(FATAL_ERROR
    "DT_SANITIZE=${DT_SANITIZE}: the compiler accepted the flags but linking "
    "failed. The sanitizer runtime libraries are probably not installed "
    "(libasan/libubsan/libtsan/liblsan for GCC, compiler-rt for Clang). "
    "Tried: ${_dt_san_flags_str}")
endif()

# ---------------------------------------------------------------------------
# Apply
# ---------------------------------------------------------------------------

add_compile_options(${_dt_san_flags})
add_link_options(${_dt_san_flags})

# ---------------------------------------------------------------------------
# Warn about build type combinations that quietly weaken the instrumentation
# ---------------------------------------------------------------------------

if("undefined" IN_LIST _dt_san_list AND CMAKE_BUILD_TYPE MATCHES "^[Rr][Ee][Ll][Ee][Aa][Ss][Ee]$")
  message(WARNING
    "DT_SANITIZE includes 'undefined' but CMAKE_BUILD_TYPE is Release, which "
    "adds -ffast-math -fno-finite-math-only. The compiler is then allowed to "
    "assume no NaN/Inf, making UBSan's floating point checks unreliable. "
    "Use -DCMAKE_BUILD_TYPE=RelWithDebInfo instead.")
endif()

if(CMAKE_BUILD_TYPE MATCHES "^[Dd][Ee][Bb][Uu][Gg]$"
   AND ("address" IN_LIST _dt_san_list OR "leak" IN_LIST _dt_san_list))
  message(WARNING
    "DT_SANITIZE includes 'address'/'leak' but CMAKE_BUILD_TYPE is Debug, which "
    "defines _DEBUG. dt_alloc_aligned() then over-allocates by one cacheline and "
    "hands out an interior pointer (src/common/darktable.c), so ASan's redzones "
    "no longer sit next to the user buffer and small over/underflows go "
    "undetected. Use -DCMAKE_BUILD_TYPE=RelWithDebInfo instead.")
endif()

# ---------------------------------------------------------------------------
# Generate the runtime environment helper
# ---------------------------------------------------------------------------

# Distributions commonly ship only the version-suffixed binary (Debian and
# Ubuntu put llvm-symbolizer-N in /usr/bin and leave the unsuffixed name to the
# llvm meta-package), so look for both. Without it the runtimes fall back to
# addr2line, which is far slower and resolves fewer frames.
find_program(DT_LLVM_SYMBOLIZER
  NAMES llvm-symbolizer
        llvm-symbolizer-21 llvm-symbolizer-20 llvm-symbolizer-19
        llvm-symbolizer-18 llvm-symbolizer-17 llvm-symbolizer-16
        llvm-symbolizer-15 llvm-symbolizer-14)
if(NOT DT_LLVM_SYMBOLIZER)
  set(DT_LLVM_SYMBOLIZER "")
  message(STATUS "llvm-symbolizer not found, sanitizer stack traces will be slower to symbolize")
endif()

string(REPLACE ";" "," DT_SANITIZE_ACTIVE "${_dt_san_list}")
set(DT_SANITIZE_SUPPRESSION_DIR "${CMAKE_SOURCE_DIR}/tools/sanitizer")

configure_file(
  "${CMAKE_SOURCE_DIR}/tools/sanitizer/sanitizer-env.sh.in"
  "${DARKTABLE_BINDIR}/sanitizer-env.sh"
  @ONLY)

message(STATUS "Sanitizers: ${DT_SANITIZE_ACTIVE}")
message(STATUS "Sanitizer flags: ${_dt_san_flags_str}")
message(STATUS "Sanitizer runtime env: ${DARKTABLE_BINDIR}/sanitizer-env.sh")
