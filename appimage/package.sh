#!/bin/sh

# Copyright (c) 2026 Magnominus
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Makes appimage/out/armGBA-x86_64.AppImage and armGBA-aarch64.AppImage from what build.sh built.
set -e
here=$(cd "$(dirname "$0")" && pwd)
docker build -q -t armgba-appimage "$here" > /dev/null
docker run --rm -u "$(id -u):$(id -g)" -v "$here:/appimage" -w /appimage/out armgba-appimage sh -c '
	set -e
	for arch in x86_64 aarch64; do
		rm -rf AppDir-$arch
		mkdir -p AppDir-$arch/usr/bin AppDir-$arch/usr/share/applications AppDir-$arch/usr/share/icons/hicolor/256x256/apps
		cp $arch/armgba AppDir-$arch/usr/bin/
		cp ../armgba.desktop AppDir-$arch/usr/share/applications/
		cp ../armgba.png AppDir-$arch/usr/share/icons/hicolor/256x256/apps/
		cp ../armgba.desktop ../armgba.png AppDir-$arch/
		ln -s usr/bin/armgba AppDir-$arch/AppRun
		ARCH=$arch /opt/appimage/appimagetool --appimage-extract-and-run --no-appstream --runtime-file /opt/appimage/runtime-$arch \
			AppDir-$arch armGBA-$arch.AppImage > package-$arch.log 2>&1 || { tail -20 package-$arch.log; exit 1; }
		rm -rf AppDir-$arch
		echo "$(ls -la armGBA-$arch.AppImage | awk "{print \$5}") bytes  armGBA-$arch.AppImage"
	done
'
