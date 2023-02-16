#    This file is part of darktable.
#    copyright (c) 2016-2020 Roman Lebedev.
#
#    darktable is free software: you can redistribute it and/or modify
#    it under the terms of the GNU General Public License as published by
#    the Free Software Foundation, either version 3 of the License, or
#    (at your option) any later version.
#
#    darktable is distributed in the hope that it will be useful,
#    but WITHOUT ANY WARRANTY; without even the implied warranty of
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#    GNU General Public License for more details.
#
#    You should have received a copy of the GNU General Public License
#    along with darktable.  If not, see <http://www.gnu.org/licenses/>.

# docker build -t darktable/darktable .

# !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! WARNING !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
# !!! hub.docker.com will not auto-rebuild the image                        !!!
# !!! after making changes here, or if you just want to manually refresh    !!!
# !!! the image, you need to go to:                                         !!!
# https://hub.docker.com/r/darktable/darktable/~/settings/automated-builds/ !!!
# !!!                            and press the "Trigger" button.            !!!
# !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!! WARNING !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

FROM debian:testing
MAINTAINER Roman Lebedev <lebedev.ri@gmail.com>

# needed at least for python-based jsonschema :(
# see https://github.com/Julian/jsonschema/issues/299
# and https://github.com/docker-library/python/issues/13
ENV LANG C.UTF-8
ENV LC_ALL C.UTF-8
ENV LC_MESSAGES C.UTF-8
ENV LANGUAGE C.UTF-8

ENV DEBIAN_FRONTEND noninteractive

# Paper over occasional network flakiness of some mirrors.
RUN echo 'Acquire::Retries "10";' > /etc/apt/apt.conf.d/80retry

# Do not install recommended packages
RUN echo 'APT::Install-Recommends "false";' > /etc/apt/apt.conf.d/80recommends

# Do not install suggested packages
RUN echo 'APT::Install-Suggests "false";' > /etc/apt/apt.conf.d/80suggests

# Assume yes
RUN echo 'APT::Get::Assume-Yes "true";' > /etc/apt/apt.conf.d/80forceyes

# Fix broken packages
RUN echo 'APT::Get::Fix-Missing "true";' > /etc/apt/apt.conf.d/80fixmissin

ENV GCC_VER=9
ENV LLVM_VER=10

# pls keep sorted :)
RUN rm -rf /var/lib/apt/lists/* && apt-get update && \
    apt-get install \
    appstream-util \
    clang-$LLVM_VER \
    cmake \
    desktop-file-utils \
    g++-$GCC_VER \
    gcc-$GCC_VER \
    gettext \
    git \
    intltool \
    libatk1.0-dev \
    libc++-$LLVM_VER-dev \
    libcairo2-dev \
    libcolord-dev \
    libcolord-gtk-dev \
    libcmocka-dev \
    libcups2-dev \
    libcurl4-gnutls-dev \
    libexiv2-dev \
    libgdk-pixbuf2.0-dev \
    libglib2.0-dev \
    libgphoto2-dev \
    libgraphicsmagick1-dev \
    libgtk-3-dev \
    libheif-dev \
    libjpeg-dev \
    libjson-glib-dev \
    liblcms2-dev \
    liblensfun-dev \
    liblua5.2-dev \
    liblua5.3-dev \
    libomp-$LLVM_VER-dev \
    libopenexr-dev \
    libopenjp2-7-dev \
    libosmgpsmap-1.0-dev \
    libpango1.0-dev \
    libpng-dev \
    libpugixml-dev \
    librsvg2-dev \
    libsaxon-java \
    libsecret-1-dev \
    libsqlite3-dev \
    libtiff5-dev \
    libwebp-dev \
    libx11-dev \
    libxml2-dev \
    libxml2-utils \
    make \
    ninja-build \
    perl \
    po4a \
    python3-jsonschema \
    xsltproc \
    zlib1g-dev && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

# i'd like to explicitly use ld.gold
# while it may be just immeasurably faster, it is known to cause more issues
# than traditional ld.bfd; plus, at this time, ld.gold seems like the future.
RUN dpkg-divert --add --rename --divert /usr/bin/ld.original /usr/bin/ld && \
    ln -s /usr/bin/ld.gold /usr/bin/ld

# optional: opencl kernels test-compilation
# pls keep sorted :)
RUN rm -rf /var/lib/apt/lists/* && apt-get update && \
    apt-get install clang-$LLVM_VER libclang-common-$LLVM_VER-dev \
    llvm-$LLVM_VER-dev && \
    apt-get clean && rm -rf /var/lib/apt/lists/*

# optional: usermanual deps
# pls keep sorted :)
RUN rm -rf /var/lib/apt/lists/* && apt-get update && \
    apt-get install default-jdk-headless default-jre-headless docbook \
    docbook-xml docbook-xsl docbook-xsl-saxon fop gnome-doc-utils imagemagick \
    libsaxon-java xsltproc && apt-get clean && rm -rf /var/lib/apt/lists/*
