cmake_minimum_required(VERSION 3.13)
project(armGBA C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

set(CORE_SOURCES
	src/bios-image.c
	src/bus.c
	src/cheats.c
	src/gba.c
	src/memory.c
	src/state.c
	src/timing.c
	src/audio/audio.c
	src/audio/psg.c
	src/cart/cart.c
	src/cart/gpio.c
	src/cart/overrides.c
	src/cart/savedata.c
	src/cpu/arm.c
	src/cpu/decode.c
	src/cpu/step.c
	src/io/dma.c
	src/io/io.c
	src/io/rfu.c
	src/io/sio.c
	src/io/timer.c
	src/video/render-thread.c
	src/video/render.c
	src/video/video.c
)

option(ARMGBA_JIT "Build the recompiler, where there is a backend for the CPU" ON)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
	list(APPEND CORE_SOURCES src/video/render-x86.c)
	set(BACKEND src/cpu/x64/backend.c)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
	list(APPEND CORE_SOURCES src/video/render-neon.c)
	set(BACKEND src/cpu/a64/backend.c)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
	list(APPEND CORE_SOURCES src/video/render-neon.c)
	set(BACKEND src/cpu/a32/backend.c)
endif()
if(ARMGBA_JIT AND BACKEND)
	list(APPEND CORE_SOURCES src/cpu/jit.c ${BACKEND})
else()
	message(STATUS "No recompiler: every instruction runs on the exact CPU")
	list(APPEND CORE_SOURCES src/cpu/nojit.c)
endif()

add_library(armgba STATIC ${CORE_SOURCES})
find_package(Threads REQUIRED)
target_link_libraries(armgba PUBLIC Threads::Threads)
# Whatever a distribution's toolchain asks for, the core needs the loop vectorizer
target_compile_options(armgba PRIVATE -O3)
# The core is small: let the compiler inline across its files
include(CheckIPOSupported)
check_ipo_supported(RESULT ipo)
if(ipo)
	set_property(TARGET armgba PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()
target_include_directories(armgba PUBLIC src)

# Differential tests against mGBA: -DMGBA_DIR=<mGBA source tree> -DMGBA_LIB=<libmgba.a built with -DCOLOR_16_BIT>,
# from a tree with tests/mgba-oracle.patch applied (flash saves complete instantly, as in armGBA)
if(MGBA_DIR AND MGBA_LIB)
	add_library(oracle-mgba STATIC tests/oracle-mgba.c)
	target_include_directories(oracle-mgba PRIVATE ${MGBA_DIR}/include ${MGBA_DIR}/src ${MGBA_BUILD_INCLUDE})
	# Must match how libmgba was built, or its structures won't line up
	target_compile_definitions(oracle-mgba PRIVATE BUILD_STATIC ENABLE_DIRECTORIES ENABLE_VFS ENABLE_VFS_FD
		HAVE_LOCALTIME_R HAVE_STRDUP HAVE_STRLCPY HAVE_STRNDUP M_CORE_GB M_CORE_GBA USE_PTHREADS _GNU_SOURCE COLOR_16_BIT)
	add_executable(cpufuzz tests/cpufuzz.c tests/oracle-armgba.c)
	target_link_libraries(cpufuzz armgba oracle-mgba ${MGBA_LIB} m pthread z)

	add_library(game-mgba STATIC tests/game-mgba.c)
	target_include_directories(game-mgba PRIVATE ${MGBA_DIR}/include ${MGBA_DIR}/src ${MGBA_BUILD_INCLUDE})
	target_compile_definitions(game-mgba PRIVATE BUILD_STATIC ENABLE_DIRECTORIES ENABLE_VFS ENABLE_VFS_FD
		HAVE_LOCALTIME_R HAVE_STRDUP HAVE_STRLCPY HAVE_STRNDUP M_CORE_GB M_CORE_GBA USE_PTHREADS _GNU_SOURCE COLOR_16_BIT)
	add_executable(gamecmp tests/gamecmp.c tests/game-armgba.c tests/game-ref.c tests/ref.c tests/mgba-state.c)
	target_link_libraries(gamecmp armgba game-mgba ${MGBA_LIB} m pthread z)
	add_executable(bench tests/bench.c tests/game-armgba.c tests/game-ref.c tests/ref.c tests/mgba-state.c)
	target_link_libraries(bench armgba game-mgba ${MGBA_LIB} m pthread z)
endif()

# Game Boy and Game Boy Color in the libretro core: Gambatte-Speedrun's core (gambatte/, GPL-2.0), which
# makes the core as a whole GPL-2.0
file(GLOB_RECURSE GAMBATTE_SOURCES gambatte/libgambatte/src/*.cpp)
list(FILTER GAMBATTE_SOURCES EXCLUDE REGEX "/(file_zip|cinterface)\\.cpp$|/unzip/")
add_library(gambatte STATIC ${GAMBATTE_SOURCES})
target_include_directories(gambatte PUBLIC gambatte/libgambatte/include PRIVATE gambatte/libgambatte/src gambatte/common)
target_compile_definitions(gambatte PUBLIC HAVE_STDINT_H)
target_compile_options(gambatte PRIVATE -w -O2 -fno-exceptions -fno-rtti)
set_target_properties(gambatte PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 11)

# The libretro core
# RetroArch's archive reading (libretro/deps), for player 2's game in a .zip or .7z.
file(GLOB_RECURSE ARCHIVE_SOURCES libretro/deps/libretro-common/*.c libretro/deps/7zip/*.c)
add_library(retro-archive STATIC ${ARCHIVE_SOURCES})
target_include_directories(retro-archive PUBLIC libretro/deps/libretro-common/include PRIVATE libretro/deps)
target_compile_definitions(retro-archive PRIVATE HAVE_ZLIB HAVE_7ZIP)
target_compile_options(retro-archive PRIVATE -w -O2)
set_target_properties(retro-archive PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(LIBRETRO_SOURCES libretro/libretro.c libretro/lcd.c libretro/options.c libretro/saves.c libretro/dolphin.c libretro/link.c libretro/games.c libretro/savefiles.c libretro/patch.c libretro/dmg07.c libretro/workers.c libretro/gb-gambatte.cpp)
add_library(armgba_libretro SHARED ${LIBRETRO_SOURCES})
# RetroArch for Android lists cores by the _android ending.
if(ANDROID)
	set_target_properties(armgba_libretro PROPERTIES OUTPUT_NAME armgba_libretro_android)
endif()
target_link_libraries(armgba_libretro armgba gambatte retro-archive m z)
# A toolchain file may leave C++ without an optimisation level
target_compile_options(armgba_libretro PRIVATE -O2)
set_target_properties(armgba_libretro PROPERTIES PREFIX "")
target_link_options(armgba_libretro PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/libretro/link.T -Wl,--no-undefined)

# armgba, a standalone frontend (frontend/): the core linked into an SDL2 program. SDL2 comes from
# pkg-config, or SDL2_INCLUDE_DIR and SDL2_LIBRARY when given.
option(ARMGBA_SDL "Build armgba, the standalone SDL2 frontend" OFF)
if(ARMGBA_SDL)
	if(NOT SDL2_LIBRARY)
		find_package(PkgConfig REQUIRED)
		pkg_check_modules(SDL2 REQUIRED sdl2)
		set(SDL2_INCLUDE_DIR ${SDL2_INCLUDE_DIRS})
		set(SDL2_LIBRARY ${SDL2_LINK_LIBRARIES})
	endif()
	file(GLOB_RECURSE RCHEEVOS_SOURCES libretro/deps/rcheevos/src/*.c)
	add_library(rcheevos STATIC ${RCHEEVOS_SOURCES})
	target_include_directories(rcheevos PUBLIC libretro/deps/rcheevos/include libretro/deps/rcheevos/src
		PRIVATE libretro/deps/libretro-common/include)
	target_compile_options(rcheevos PRIVATE -w -O2)
	set(FONTS ${CMAKE_CURRENT_SOURCE_DIR}/frontend/fonts/Inter-Medium.otf ${CMAKE_CURRENT_SOURCE_DIR}/frontend/fonts/Inter-SemiBold.otf
		${CMAKE_CURRENT_SOURCE_DIR}/frontend/fonts/ChakraPetch-Bold.ttf)
	string(REPLACE ";" "|" FONT_LIST "${FONTS}")
	add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/fonts.inc
		COMMAND ${CMAKE_COMMAND} -DFONTS=${FONT_LIST} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/fonts.inc -P ${CMAKE_CURRENT_SOURCE_DIR}/frontend/fonts/embed.cmake
		DEPENDS ${FONTS} frontend/fonts/embed.cmake VERBATIM)
	file(GLOB FRONTEND_SOURCES frontend/*.c)
	# On Android it is libmain.so, which SDL's activity loads and starts (android/).
	if(ANDROID)
		add_library(armgba-sdl SHARED ${FRONTEND_SOURCES} ${LIBRETRO_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/fonts.inc)
		set_target_properties(armgba-sdl PROPERTIES OUTPUT_NAME main)
	else()
		add_executable(armgba-sdl ${FRONTEND_SOURCES} ${LIBRETRO_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/fonts.inc)
		set_target_properties(armgba-sdl PROPERTIES OUTPUT_NAME armgba)
	endif()
	target_include_directories(armgba-sdl PRIVATE libretro ${SDL2_INCLUDE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
	target_link_libraries(armgba-sdl armgba gambatte rcheevos retro-archive ${SDL2_LIBRARY} m z ${CMAKE_DL_LIBS})
	target_compile_options(armgba-sdl PRIVATE -O2)
endif()
