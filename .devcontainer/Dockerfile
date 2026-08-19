# Use Ubuntu 26.04 LTS to match the CI environment
# CI uses ubuntu:26.04 container in .github/workflows/ci.yml
FROM ubuntu:26.04

# Avoid prompts from apt
ENV DEBIAN_FRONTEND=noninteractive

# Set up locale
RUN apt-get update && apt-get install -y locales && \
    locale-gen en_US.UTF-8 && \
    update-locale LANG=en_US.UTF-8 && \
    rm -rf /var/lib/apt/lists/*

ENV LANG=en_US.UTF-8
ENV LANGUAGE=en_US:en
ENV LC_ALL=en_US.UTF-8

# Match the CI "Update base packages" step
RUN apt-get update && apt-get install -y eatmydata && \
    eatmydata apt-get --yes upgrade && \
    rm -rf /var/lib/apt/lists/*

# Install compilers matching CI:
#   - Primary:     GNU16   (gcc-16 / g++-16)
#   - Alternative: LLVM22  (clang-22 / clang++-22)
RUN eatmydata apt-get update && eatmydata apt-get --yes install \
    gcc-16 \
    g++-16 \
    clang-22 \
    libomp-22-dev \
    llvm-22-dev \
    libc++-22-dev \
    libc++abi1 \
    lld-22 \
    clang-tools-22 \
    mlir-22-tools \
    libmlir-22-dev \
    && rm -rf /var/lib/apt/lists/*

# Install base dependencies — exact match with CI
# (see "Install Base Dependencies" step in .github/workflows/ci.yml)
RUN eatmydata apt-get update && eatmydata apt-get --yes install \
    build-essential \
    cmake \
    appstream-util \
    desktop-file-utils \
    gettext \
    git \
    gdb \
    intltool \
    libarchive-dev \
    libatk1.0-dev \
    libavif-dev \
    libcairo2-dev \
    libcmocka-dev \
    libcolord-dev \
    libcolord-gtk-dev \
    libcups2-dev \
    libcurl4-gnutls-dev \
    libexiv2-dev \
    libgdk-pixbuf-2.0-dev \
    libglib2.0-dev \
    libgmic-dev \
    libgphoto2-dev \
    libgraphicsmagick1-dev \
    libgtk-3-dev \
    libheif-dev \
    libjpeg-dev \
    libjson-glib-dev \
    liblcms2-dev \
    liblensfun-dev \
    liblua5.4-dev \
    libonnxruntime-dev \
    libopencv-calib3d-dev \
    libopencv-core-dev \
    libopencv-features2d-dev \
    libopencv-flann-dev \
    libopencv-imgproc-dev \
    libopenexr-dev \
    libopenjp2-7-dev \
    libosmgpsmap-1.0-dev \
    libpango1.0-dev \
    libpng-dev \
    libportmidi-dev \
    libpotrace-dev \
    libpugixml-dev \
    libraw-dev \
    librsvg2-dev \
    libsaxon-java \
    libsdl2-dev \
    libsecret-1-dev \
    libsqlite3-dev \
    libtiff5-dev \
    libwebp-dev \
    libx11-dev \
    libxml2-dev \
    libxml2-utils \
    ninja-build \
    perl \
    po4a \
    python3-jsonschema \
    xsltproc \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Set the working directory
WORKDIR /workspace

CMD ["/bin/bash"]
