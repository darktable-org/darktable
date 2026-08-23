#!/usr/bin/env bash
# Run all Linux CI matrix configurations against a candidate Docker image.
# Used by build-docker.yml; can also be called locally to validate a new image.
# Usage: test-image.sh <image> <src-dir>
set -euo pipefail

IMAGE="${1:?Usage: $0 <image> <src-dir>}"
SRC_DIR="${2:?Usage: $0 <image> <src-dir>}"

run_config() {
    local name="$1"; shift
    local build_dir="${SRC_DIR}/build/${name}"
    local install_dir="${SRC_DIR}/install/${name}"
    mkdir -p "${build_dir}" "${install_dir}"
    printf '\n=== Testing configuration: %s ===\n\n' "${name}"
    docker run --rm \
        --tmpfs /tmp:exec \
        -v "${SRC_DIR}:${SRC_DIR}" \
        -e SRC_DIR="${SRC_DIR}" \
        -e BUILD_DIR="${build_dir}" \
        -e INSTALL_PREFIX="${install_dir}" \
        -e GENERATOR=Ninja \
        "$@" \
        "${IMAGE}" \
        "${SRC_DIR}/.ci/ci-script.sh"
    printf '\n=== Configuration %s passed ===\n' "${name}"
    printf 'Cleaning up build and install directories for %s\n' "${name}"
    docker run --rm \
        -v "${SRC_DIR}:${SRC_DIR}" \
        "${IMAGE}" \
        bash -c 'rm -rf -- "$1" "$2"' _ "${build_dir}" "${install_dir}"
}

# Mirror all Linux matrix configurations from .github/workflows/ci.yml
run_config GNU16_Release \
    -e CC=gcc-16 -e CXX=g++-16 \
    -e CMAKE_BUILD_TYPE=Release \
    -e TARGET=skiptest \
    -e ECO="-DDONT_USE_INTERNAL_LIBRAW=ON"

run_config LLVM22_Release \
    -e CC=clang-22 -e CXX=clang++-22 \
    -e CMAKE_BUILD_TYPE=Release \
    -e TARGET=skiptest \
    -e ECO="-DDONT_USE_INTERNAL_LIBRAW=ON"

run_config GNU16_Debug \
    -e CC=gcc-16 -e CXX=g++-16 \
    -e CMAKE_BUILD_TYPE=Debug \
    -e TARGET=skiptest \
    -e ECO="-DDONT_USE_INTERNAL_LIBRAW=OFF"

run_config GNU16_Release_tests \
    -e CC=gcc-16 -e CXX=g++-16 \
    -e CMAKE_BUILD_TYPE=Release \
    -e TARGET=build \
    -e ECO="-DDONT_USE_INTERNAL_LIBRAW=ON"

printf '\n=== All configurations passed ===\n'
