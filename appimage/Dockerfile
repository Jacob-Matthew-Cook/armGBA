# The machine armGBA's AppImages are built on: Debian 11 (glibc 2.31), so they run on distributions as
# old as that. Debian 11's packages are now on Debian's archive.
FROM debian:bullseye
RUN printf 'deb http://archive.debian.org/debian bullseye main\ndeb-src http://archive.debian.org/debian bullseye main\ndeb http://archive.debian.org/debian-security bullseye-security main\n' > /etc/apt/sources.list \
	&& dpkg --add-architecture arm64 && apt-get -o Acquire::Check-Valid-Until=false update && apt-get install -y --no-install-recommends \
	build-essential dpkg-dev cmake pkg-config file ca-certificates curl python3 \
	libsdl2-dev zlib1g-dev \
	crossbuild-essential-arm64 libsdl2-dev:arm64 zlib1g-dev:arm64 \
	&& rm -rf /var/lib/apt/lists/*
# SDL2 from Debian's source, as a static library that loads X11, Wayland, KMS, ALSA and PulseAudio
# when it runs, from the system it runs on: the AppImage then needs only glibc.
RUN mkdir /sdl && cd /sdl && apt-get -o Acquire::Check-Valid-Until=false update && apt-get source libsdl2 && rm -rf /var/lib/apt/lists/* \
	&& for arch in x86_64 aarch64; do \
		mkdir /sdl/build-$arch && cd /sdl/build-$arch \
		&& if [ $arch = aarch64 ]; then host=--host=aarch64-linux-gnu; export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig; else host=; unset PKG_CONFIG_LIBDIR; fi \
		&& ../libsdl2-*/configure $host --prefix=/opt/sdl2-$arch --disable-shared --enable-static \
			--enable-x11-shared --enable-wayland-shared --enable-kmsdrm-shared --enable-alsa-shared --enable-pulseaudio-shared \
			--disable-jack --disable-esd --disable-arts --disable-nas --disable-sndio --disable-directfb --disable-fusionsound \
			--disable-rpath > /sdl/configure-$arch.log \
		&& make -j"$(nproc)" > /sdl/make-$arch.log && make install > /dev/null || exit 1; \
	done
# appimagetool, and the AppImage runtimes for both machines.
RUN mkdir /opt/appimage && cd /opt/appimage \
	&& curl -sSfL -o appimagetool https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage \
	&& curl -sSfL -o runtime-x86_64 https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64 \
	&& curl -sSfL -o runtime-aarch64 https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-aarch64 \
	&& chmod +x appimagetool
