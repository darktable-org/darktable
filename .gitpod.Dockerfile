FROM gitpod/workspace-full-vnc

# Install eatmydata package
RUN sudo apt-get update && \
    sudo apt-get install -y eatmydata && \
    sudo rm -rf /var/lib/apt/lists/*

ENV CC="clang-15"
ENV CXX="clang++-15"

# Install compiler packages
RUN sudo apt-get update && \
    sudo eatmydata apt-get -y install \
    clang-15 libomp-15-dev llvm-15-dev libc++-15-dev libc++abi1-15 lld-15 clang-tools-15 mlir-15-tools libmlir-15-dev cmake && \
    sudo rm -rf /var/lib/apt/lists/*

# Install Base Dependencies
RUN sudo apt-get update && \
    sudo eatmydata apt-get -y install \
    build-essential \
    appstream-util \
    desktop-file-utils \
    gettext \
    git \
    gdb \
    intltool \
    libatk1.0-dev \
    libavif-dev \
    libcairo2-dev \
    libcolord-dev \
    libcolord-gtk-dev \
    libcmocka-dev \
    libcups2-dev \
    libcurl4-gnutls-dev \
    libexiv2-dev \
    libgdk-pixbuf2.0-dev \
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
    libopenexr-dev \
    libopenjp2-7-dev \
    libosmgpsmap-1.0-dev \
    libpango1.0-dev \
    libpng-dev \
    libportmidi-dev \
    libpugixml-dev \
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
    zlib1g-dev && \
    sudo rm -rf /var/lib/apt/lists/*
