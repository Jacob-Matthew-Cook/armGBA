#!/bin/sh

# Copyright (c) 2026 Magnominus
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Builds android/out/armGBA-arm64.apk: SDL2 and the standalone frontend with the core for arm64-v8a
# with the NDK, then SDL's Java and armGBA's, packed and signed with the Android SDK's tools, without
# Gradle. ANDROID_NDK and ANDROID_HOME say where they are; SDL2_SOURCE, SDL2's source, else its
# release is downloaded. The signing key is kept in ~/.android/armgba.keystore, so each build installs
# over the last.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$here/..
out=$here/out
ndk=${ANDROID_NDK:-$HOME/android-ndk-r27c}
sdk=${ANDROID_HOME:-$HOME/Android/Sdk}
tools=$sdk/build-tools/34.0.0
platform=$sdk/platforms/android-34/android.jar
abi=arm64-v8a
api=24
mkdir -p "$out"

if [ -z "$SDL2_SOURCE" ]; then
	SDL2_SOURCE=$out/SDL2-2.32.10
	if [ ! -d "$SDL2_SOURCE" ]; then
		curl -sSfL -o "$out/SDL2-2.32.10.tar.gz" https://www.libsdl.org/release/SDL2-2.32.10.tar.gz
		echo "5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165  $out/SDL2-2.32.10.tar.gz" | sha256sum -c - > /dev/null
		tar -xzf "$out/SDL2-2.32.10.tar.gz" -C "$out"
	fi
fi

# SDL2, then the frontend as libmain.so.
android="-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake -DANDROID_ABI=$abi -DANDROID_PLATFORM=$api -DCMAKE_BUILD_TYPE=Release"
cmake -S "$SDL2_SOURCE" -B "$out/sdl-$abi" -G Ninja $android -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF > /dev/null
cmake --build "$out/sdl-$abi" > "$out/sdl-$abi.log" 2>&1 || { tail -20 "$out/sdl-$abi.log"; exit 1; }
cmake -S "$root" -B "$out/app-$abi" -G Ninja $android -DARMGBA_SDL=ON \
	"-DSDL2_INCLUDE_DIR=$SDL2_SOURCE/include;$out/sdl-$abi/include;$out/sdl-$abi/include-config-release/SDL2" \
	"-DSDL2_LIBRARY=$out/sdl-$abi/libSDL2.so;log;android" > /dev/null
cmake --build "$out/app-$abi" --target armgba-sdl > "$out/app-$abi.log" 2>&1 || { tail -20 "$out/app-$abi.log"; exit 1; }

# The Java: SDL's activity and armGBA's, as dex, by the command-line tools' d8 (build-tools 34's fails
# on SDL's classes).
rm -rf "$out/classes" "$out/dex" "$out/apk"
mkdir -p "$out/classes" "$out/dex" "$out/apk/lib/$abi"
javac -nowarn --release 8 -classpath "$platform" -d "$out/classes" \
	$(find "$SDL2_SOURCE/android-project/app/src/main/java" "$here/java" -name '*.java') 2> "$out/javac.log" || { cat "$out/javac.log"; exit 1; }
"$sdk/cmdline-tools/latest/bin/d8" --release --lib "$platform" --min-api $api --output "$out/dex" $(find "$out/classes" -name '*.class')

# The package: manifest and icon, then the code and libraries, aligned and signed.
version=$(git -C "$root" rev-list --count HEAD)
name="1.1-$(git -C "$root" rev-parse --short HEAD)"
"$tools/aapt2" compile --dir "$here/res" -o "$out/res.zip"
"$tools/aapt2" link -I "$platform" --manifest "$here/AndroidManifest.xml" -o "$out/apk/base.apk" "$out/res.zip" \
	--min-sdk-version $api --target-sdk-version 34 --version-code "$version" --version-name "$name"
cp "$out/dex/classes.dex" "$out/apk/"
cp "$out/sdl-$abi/libSDL2.so" "$out/app-$abi/libmain.so" "$out/apk/lib/$abi/"
"$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" "$out/apk/lib/$abi/"*.so
(cd "$out/apk" && zip -q base.apk classes.dex && zip -q -0 base.apk lib/$abi/*.so)
"$tools/zipalign" -p -f 4 "$out/apk/base.apk" "$out/apk/aligned.apk"
key=$HOME/.android/armgba.keystore
if [ ! -f "$key" ]; then
	mkdir -p "$(dirname "$key")"
	keytool -genkeypair -keystore "$key" -storepass android -keypass android -alias armgba -keyalg RSA -keysize 2048 \
		-validity 10000 -dname "CN=armGBA" > /dev/null 2>&1
fi
"$tools/apksigner" sign --ks "$key" --ks-pass pass:android --key-pass pass:android --out "$out/armGBA-arm64.apk" "$out/apk/aligned.apk"
echo "$(ls -la "$out/armGBA-arm64.apk" | awk '{print $5}') bytes  armGBA-arm64.apk  ($name)"
