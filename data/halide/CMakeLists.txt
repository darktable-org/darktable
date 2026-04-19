# Halide-based image processing generators for darktable
#
# This builds Halide generator executables at compile time, then runs them
# to produce optimized static libraries (.h + .a) for each target platform.
# The generated code is linked into individual IOP plugin modules.

find_package(Halide REQUIRED)

# Build the generator executable (runs on the host at build time)
add_halide_generator(dt_halide_generators
    SOURCES
        generators/exposure_generator.cpp
)

# Generate CPU-optimized code for the host platform
# "host" auto-detects arch + OS + SIMD features (NEON on ARM64, AVX on x86, etc.)
add_halide_library(halide_exposure
    FROM dt_halide_generators
    GENERATOR exposure
    TARGETS host
    FUNCTION_NAME dt_halide_exposure
)

# Generate Metal GPU code for Apple Silicon (macOS only)
# Uses the "flat" 2D generator to avoid stride issues with Metal's buffer handling.
# The flat generator treats each row as width*4 contiguous floats.
if(APPLE)
    add_halide_library(halide_exposure_metal
        FROM dt_halide_generators
        GENERATOR exposure_flat
        TARGETS host-metal
        FUNCTION_NAME dt_halide_exposure_metal
    )
endif()

# Generate Vulkan GPU code (cross-platform: Linux, Windows, macOS via MoltenVK)
# Uses the same flat 2D generator as Metal. Vulkan loader is dlopen'd at runtime,
# so no link-time dependency — if no Vulkan driver is present, the call simply fails
# and we fall through to CPU Halide.
find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    add_halide_library(halide_exposure_vulkan
        FROM dt_halide_generators
        GENERATOR exposure_flat
        TARGETS host-vulkan
        FUNCTION_NAME dt_halide_exposure_vulkan
    )
    set(HALIDE_HAS_VULKAN TRUE PARENT_SCOPE)
endif()
