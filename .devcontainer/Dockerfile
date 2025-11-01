# Use Ubuntu 24.04 LTS as base image
FROM ubuntu:24.04

# Avoid prompts from apt
ENV DEBIAN_FRONTEND=noninteractive

# Set up locale
RUN apt-get update && apt-get install -y locales && \
    locale-gen en_US.UTF-8 && \
    update-locale LANG=en_US.UTF-8

ENV LANG=en_US.UTF-8
ENV LANGUAGE=en_US:en
ENV LC_ALL=en_US.UTF-8

# Install build dependencies
RUN apt-get update && apt-get install -y \
    # Build tools
    build-essential \
    cmake \
    git \
    ninja-build \
    pkg-config \
    intltool \
    gettext \
    po4a \
    xsltproc \
    libxml2-utils \
    python3-jsonschema \
    # Required dependencies
    libgtk-3-dev \
    libglib2.0-dev \
    libsqlite3-dev \
    libcurl4-openssl-dev \
    libpng-dev \
    libexiv2-dev \
    libpugixml-dev \
    liblensfun-dev \
    libjpeg-dev \
    libtiff-dev \
    liblcms2-dev \
    librsvg2-dev \
    libjson-glib-dev \
    # Optional dependencies for full functionality
    libomp-dev \
    llvm-dev \
    ocl-icd-opencl-dev \
    liblua5.4-dev \
    libgphoto2-dev \
    libimath-dev \
    libavif-dev \
    libheif-dev \
    libjxl-dev \
    libwebp-dev \
    libcolord-dev \
    libportmidi-dev \
    libsdl2-dev \
    libcups2-dev \
    libopenexr-dev \
    libopenjp2-7-dev \
    libgraphicsmagick1-dev \
    # Additional tools
    clang-format \
    vim \
    nano \
    wget \
    curl \
    sudo \
    # AppImage building tools
    desktop-file-utils \
    && rm -rf /var/lib/apt/lists/*

# Set the working directory
WORKDIR /workspace

CMD ["/bin/bash"]
