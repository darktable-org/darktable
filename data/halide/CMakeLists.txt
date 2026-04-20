# Halide-based image processing generators for darktable
#
# This builds Halide generator executables at compile time, then runs them
# to produce optimized static libraries (.h + .a) for each target platform.
# The generated code is linked into individual IOP plugin modules.

find_package(Halide REQUIRED)

# Build the generator executables (run on the host at build time)
add_halide_generator(dt_halide_generators
    SOURCES
        generators/exposure_generator.cpp
)

add_halide_generator(dt_halide_bspline_generators
    SOURCES
        generators/bspline_generator.cpp
)

add_halide_generator(dt_halide_diffuse_generators
    SOURCES
        generators/diffuse_pde_generator.cpp
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
# B-spline wavelet decomposition for diffuse/sharpen module
# CPU target: separable 5-tap blur + detail extraction in one pass
add_halide_library(halide_bspline_decompose
    FROM dt_halide_bspline_generators
    GENERATOR bspline_decompose
    TARGETS host
    FUNCTION_NAME dt_halide_bspline_decompose
)

# Metal GPU target for B-spline decomposition (macOS only)
if(APPLE)
    add_halide_library(halide_bspline_decompose_metal
        FROM dt_halide_bspline_generators
        GENERATOR bspline_decompose
        TARGETS host-metal
        FUNCTION_NAME dt_halide_bspline_decompose_metal
    )
endif()

# Heat PDE diffusion kernel for diffuse/sharpen module
add_halide_library(halide_diffuse_pde
    FROM dt_halide_diffuse_generators
    GENERATOR diffuse_pde
    TARGETS host
    FUNCTION_NAME dt_halide_diffuse_pde
)

# Metal GPU target for PDE diffusion (macOS only)
if(APPLE)
    add_halide_library(halide_diffuse_pde_metal
        FROM dt_halide_diffuse_generators
        GENERATOR diffuse_pde
        TARGETS host-metal
        FUNCTION_NAME dt_halide_diffuse_pde_metal
    )
endif()

find_package(Vulkan QUIET)
if(Vulkan_FOUND)
    add_halide_library(halide_exposure_vulkan
        FROM dt_halide_generators
        GENERATOR exposure_flat
        TARGETS host-vulkan
        FUNCTION_NAME dt_halide_exposure_vulkan
    )
    add_halide_library(halide_bspline_decompose_vulkan
        FROM dt_halide_bspline_generators
        GENERATOR bspline_decompose
        TARGETS host-vulkan
        FUNCTION_NAME dt_halide_bspline_decompose_vulkan
    )
    add_halide_library(halide_diffuse_pde_vulkan
        FROM dt_halide_diffuse_generators
        GENERATOR diffuse_pde
        TARGETS host-vulkan
        FUNCTION_NAME dt_halide_diffuse_pde_vulkan
    )
    set(HALIDE_HAS_VULKAN TRUE PARENT_SCOPE)
endif()
