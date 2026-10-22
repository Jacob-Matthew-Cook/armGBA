#!/bin/sh

# Copyright (c) 2026 Magnominus
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Builds armgba, the standalone frontend, for x86-64 and 64-bit ARM in the Debian 11 container
# (Dockerfile), into appimage/out/<arch>/armgba. package.sh then makes the AppImages.
set -e
here=$(cd "$(dirname "$0")" && pwd)
docker build -q -t armgba-appimage "$here" > /dev/null
docker run --rm -u "$(id -u):$(id -g)" -v "$here/..:/src" -w /src armgba-appimage sh -c '
	set -e
	for arch in x86_64 aarch64; do
		toolchain=
		[ $arch = aarch64 ] && toolchain=-DCMAKE_TOOLCHAIN_FILE=/src/appimage/aarch64.cmake
		cmake -S . -B appimage/out/build-$arch $toolchain -DCMAKE_BUILD_TYPE=Release -DARMGBA_SDL=ON \
			-DSDL2_INCLUDE_DIR=/opt/sdl2-$arch/include/SDL2 \
			"-DSDL2_LIBRARY=/opt/sdl2-$arch/lib/libSDL2.a;-ldl;-lrt;-lpthread;-static-libstdc++;-static-libgcc" > /dev/null
		cmake --build appimage/out/build-$arch -j"$(nproc)" --target armgba-sdl > appimage/out/build-$arch.log 2>&1 || { tail -20 appimage/out/build-$arch.log; exit 1; }
		mkdir -p appimage/out/$arch
		cp appimage/out/build-$arch/armgba appimage/out/$arch/armgba
		strip -s appimage/out/$arch/armgba 2>/dev/null || $([ $arch = aarch64 ] && echo aarch64-linux-gnu-strip) -s appimage/out/$arch/armgba
		echo "$arch: $(file -b appimage/out/$arch/armgba | cut -c1-80)"
	done
'
