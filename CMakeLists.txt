#
# CMake build for glfrontier, converted from Makefile-C.
#
# Typical usage:
#   cmake -B build
#   cmake --build build -j
#
# fe2.s.c, fixups.h and gen/*.c are the C translation of the original
# 68k binary (fe2.s), produced by the as68k tool. They are already
# checked into the repository, so a normal build does not need to
# regenerate them. If you ever modify fe2.s, regenerate with:
#   cmake --build build --target gen68k
#   cmake build          # reconfigure to pick up new/removed gen/*.c files
#
cmake_minimum_required(VERSION 3.16)
project(frontier C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED OFF)
set(CMAKE_C_EXTENSIONS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(WITH_GL "Use the OpenGL renderer (src/gl.c) instead of the software one (src/soft.c)" ON)
option(WITH_OGG_MUSIC "Enable Ogg Vorbis music support" ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(SDL REQUIRED IMPORTED_TARGET sdl)
pkg_check_modules(VORBISFILE REQUIRED IMPORTED_TARGET vorbisfile)
pkg_check_modules(VORBIS REQUIRED IMPORTED_TARGET vorbis)
pkg_check_modules(OGG REQUIRED IMPORTED_TARGET ogg)

if(WITH_GL)
	find_package(OpenGL REQUIRED)
else()
	# Only the software renderer (src/soft.c) needs SDL_image.
	pkg_check_modules(SDL_IMAGE REQUIRED IMPORTED_TARGET SDL_image)
endif()

# --- as68k: the 68k assembler/"transpiler" used to (re)generate fe2.s.c ---
add_executable(as68k
	as68k/as68k.c
	as68k/dict.c
	as68k/output.c
	as68k/output_c.c
	as68k/output_i386.c
)

# Regenerates fe2.s.c, fixups.h and gen/*.c from fe2.s. Not part of the
# default build (ALL) since those files are already committed.
add_custom_target(gen68k
	COMMAND $<TARGET_FILE:as68k> --output-c fe2.s
	WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
	DEPENDS as68k
	COMMENT "Regenerating fe2.s.c, fixups.h and gen/*.c from fe2.s"
	VERBATIM
)

if(NOT EXISTS ${CMAKE_SOURCE_DIR}/fe2.s.c OR NOT EXISTS ${CMAKE_SOURCE_DIR}/fixups.h)
	message(FATAL_ERROR
		"fe2.s.c / fixups.h are missing. Generate them first with:\n"
		"  cmake --build <builddir> --target gen68k\n"
		"then re-run cmake to reconfigure.")
endif()

file(GLOB GEN_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/gen/*.c)
if(NOT GEN_SOURCES)
	message(FATAL_ERROR "gen/*.c is empty. Run the gen68k target first (see above).")
endif()

set(FRONTIER_SOURCES
	host.c
	fe2.s.c
	${GEN_SOURCES}
	src/audio.c
	src/hostcall.c
	src/keymap.c
	src/main.c
	src/shortcut.c
	src/input.c
	src/joystick.c
	src/ini.c
)

if(WITH_GL)
	list(APPEND FRONTIER_SOURCES src/gl.c)
else()
	list(APPEND FRONTIER_SOURCES src/soft.c)
endif()

add_executable(frontier ${FRONTIER_SOURCES})

target_include_directories(frontier PRIVATE
	${CMAKE_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
)

target_compile_options(frontier PRIVATE
	-Wall -Wno-unused
)

target_link_libraries(frontier PRIVATE
	PkgConfig::SDL
	PkgConfig::VORBISFILE
	PkgConfig::VORBIS
	PkgConfig::OGG
	m
)

if(WITH_OGG_MUSIC)
	target_compile_definitions(frontier PRIVATE OGG_MUSIC)
endif()

if(WITH_GL)
	target_compile_definitions(frontier PRIVATE WITH_GL)
	target_link_libraries(frontier PRIVATE OpenGL::GL OpenGL::GLU)
	if(APPLE)
		# The renderer intentionally uses legacy fixed-function OpenGL.
		target_compile_definitions(frontier PRIVATE GL_SILENCE_DEPRECATION)
	endif()
else()
	target_link_libraries(frontier PRIVATE PkgConfig::SDL_IMAGE)
endif()
