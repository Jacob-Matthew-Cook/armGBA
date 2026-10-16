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
	src/arm.c
	src/audio.c
	src/bus.c
	src/cart.c
	src/cheats.c
	src/decode.c
	src/dma.c
	src/gba.c
	src/gpio.c
	src/io.c
	src/memory.c
	src/overrides.c
	src/psg.c
	src/render.c
	src/render-thread.c
	src/savedata.c
	src/step.c
	src/sio.c
	src/state.c
	src/timer.c
	src/timing.c
	src/video.c
)

option(ARMGBA_JIT "Build the recompiler, where there is a backend for the CPU" ON)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
	list(APPEND CORE_SOURCES src/render-x86.c)
	set(BACKEND src/x64/backend.c)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
	list(APPEND CORE_SOURCES src/render-neon.c)
	set(BACKEND src/a64/backend.c)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^arm")
	list(APPEND CORE_SOURCES src/render-neon.c)
	set(BACKEND src/a32/backend.c)
endif()
if(ARMGBA_JIT AND BACKEND)
	list(APPEND CORE_SOURCES src/jit.c ${BACKEND})
else()
	message(STATUS "No recompiler: every instruction runs on the exact CPU")
	list(APPEND CORE_SOURCES src/nojit.c)
endif()

add_library(armgba STATIC ${CORE_SOURCES})
target_link_libraries(armgba PUBLIC pthread)
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
add_library(armgba_libretro SHARED libretro/libretro.c libretro/gb-gambatte.cpp)
target_link_libraries(armgba_libretro armgba gambatte m)
target_compile_definitions(armgba_libretro PRIVATE ARMGBA_GB)
# A toolchain file may leave C++ without an optimisation level
target_compile_options(armgba_libretro PRIVATE -O2)
set_target_properties(armgba_libretro PROPERTIES PREFIX "")
target_link_options(armgba_libretro PRIVATE -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/libretro/link.T -Wl,--no-undefined)
