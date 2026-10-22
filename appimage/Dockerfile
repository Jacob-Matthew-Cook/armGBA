# Copyright (c) 2026 Jacob Cook
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# The machine armGBA's AppImages are built on: Debian 11, whose glibc is old enough that they run on
# glibc 2.29 or later. Debian 11's packages are now on Debian's archive.
FROM debian:bullseye
RUN printf 'deb http://archive.debian.org/debian bullseye main\ndeb-src http://archive.debian.org/debian bullseye main\ndeb http://archive.debian.org/debian-security bullseye-security main\n' > /etc/apt/sources.list \
	&& dpkg --add-architecture arm64 && apt-get -o Acquire::Check-Valid-Until=false update && apt-get install -y --no-install-recommends \
	build-essential dpkg-dev cmake pkg-config file ca-certificates curl python3 \
	libsdl2-dev zlib1g-dev libdrm-dev libgbm-dev \
	crossbuild-essential-arm64 libsdl2-dev:arm64 zlib1g-dev:arm64 libdrm-dev:arm64 libgbm-dev:arm64 \
	&& rm -rf /var/lib/apt/lists/*
# SDL2 2.32.10, as ROCKNIX ships it, as a static library that loads X11, Wayland, KMS, ALSA and
# PulseAudio when it runs, from the system it runs on: the AppImage then needs only glibc.
RUN mkdir /sdl && cd /sdl && curl -sSfLO https://www.libsdl.org/release/SDL2-2.32.10.tar.gz \
	&& echo "5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165  SDL2-2.32.10.tar.gz" | sha256sum -c - \
	&& tar xzf SDL2-2.32.10.tar.gz \
	&& for arch in x86_64 aarch64; do \
		mkdir /sdl/build-$arch && cd /sdl/build-$arch \
		&& if [ $arch = aarch64 ]; then host=--host=aarch64-linux-gnu; export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig; else host=; unset PKG_CONFIG_LIBDIR; fi \
		&& ../SDL2-2.32.10/configure $host --prefix=/opt/sdl2-$arch --disable-shared --enable-static \
			--enable-x11-shared --enable-wayland-shared --enable-kmsdrm-shared --enable-alsa-shared --enable-pulseaudio-shared \
			--disable-jack --disable-sndio --disable-pipewire --disable-rpath > /sdl/configure-$arch.log \
		&& make -j"$(nproc)" > /sdl/make-$arch.log && make install > /dev/null || exit 1; \
	done
# appimagetool, and the AppImage runtimes for both machines.
RUN mkdir /opt/appimage && cd /opt/appimage \
	&& curl -sSfL -o appimagetool https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage \
	&& curl -sSfL -o runtime-x86_64 https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64 \
	&& curl -sSfL -o runtime-aarch64 https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-aarch64 \
	&& chmod +x appimagetool
