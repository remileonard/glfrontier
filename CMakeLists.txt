#
# CMake build for glfrontier, converted from Makefile-C.
#
# Typical usage:
#   cmake -B build
#   cmake --build build -j
#
# This builds a plain 'frontier' executable. On macOS Frontier.app (with the
# game data inside, using the SDL, Ogg Vorbis... installed on this Mac) is
# made by installing:
#   cmake --install build --prefix dist      # -> dist/Frontier.app
# or, the same into <builddir>/dist:
#   cmake --build build --target dist
#
# fe2.s.c, fixups.h, gen/*.c and fe2.s.bin are the C translation of the
# original 68k program (fe2.s), produced by the as68k tool. They are
# generated automatically into <builddir>/gen68k, and regenerated
# whenever fe2.s (or as68k itself) changes: just run the build again.
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

# --- fe2.s -> C, with as68k ----------------------------------------------
#
# as68k splits the program into a few hundred gen/__F*.c files whose names
# depend on the contents of fe2.s, so they cannot be declared as the
# outputs of a build step. Instead they are generated at configure time,
# and fe2.s and the as68k sources are configure dependencies: when one of
# them changes, the next build re-runs CMake, which regenerates them and
# picks up the new file list.
#
# as68k appends to the gen/*.c files, so it always runs in an empty
# directory; the results are then copied to gen68k/ only where they
# changed, so that only those get recompiled.

set(AS68K_SOURCES
	${CMAKE_SOURCE_DIR}/as68k/as68k.c
	${CMAKE_SOURCE_DIR}/as68k/dict.c
	${CMAKE_SOURCE_DIR}/as68k/output.c
	${CMAKE_SOURCE_DIR}/as68k/output_c.c
	${CMAKE_SOURCE_DIR}/as68k/output_i386.c
)
set(FE2_SOURCE ${CMAKE_SOURCE_DIR}/fe2.s)
set(GEN68K_DIR ${CMAKE_BINARY_DIR}/gen68k)
set(GEN68K_WORK ${CMAKE_BINARY_DIR}/gen68k-work)

# as68k runs on the build machine: when cross compiling, give it a native
# compiler with -DAS68K_HOST_CC=cc
if(CMAKE_CROSSCOMPILING)
	set(AS68K_HOST_CC cc CACHE STRING "Native C compiler used to build the as68k tool")
else()
	set(AS68K_HOST_CC ${CMAKE_C_COMPILER} CACHE STRING "Native C compiler used to build the as68k tool")
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${FE2_SOURCE} ${AS68K_SOURCES})

set(GEN68K_KEY "")
foreach(f ${FE2_SOURCE} ${AS68K_SOURCES})
	file(SHA256 ${f} h)
	string(APPEND GEN68K_KEY "${h}")
endforeach()
string(SHA256 GEN68K_KEY "${GEN68K_KEY}")

set(GEN68K_STAMP ${GEN68K_DIR}/.stamp)
set(GEN68K_OLD_KEY "")
if(EXISTS ${GEN68K_STAMP})
	file(READ ${GEN68K_STAMP} GEN68K_OLD_KEY)
endif()

if(NOT GEN68K_KEY STREQUAL GEN68K_OLD_KEY)
	message(STATUS "Generating C from fe2.s with as68k")
	file(REMOVE_RECURSE ${GEN68K_WORK})
	file(MAKE_DIRECTORY ${GEN68K_WORK}/gen)

	execute_process(
		COMMAND ${AS68K_HOST_CC} -O2 -o ${GEN68K_WORK}/as68k ${AS68K_SOURCES}
		RESULT_VARIABLE res OUTPUT_VARIABLE out ERROR_VARIABLE out)
	if(NOT res EQUAL 0)
		message(FATAL_ERROR "Building as68k failed:\n${out}")
	endif()

	# as68k writes its outputs next to the source it is given
	file(COPY ${FE2_SOURCE} DESTINATION ${GEN68K_WORK})
	execute_process(
		COMMAND ${GEN68K_WORK}/as68k --output-c fe2.s
		WORKING_DIRECTORY ${GEN68K_WORK}
		RESULT_VARIABLE res OUTPUT_VARIABLE out ERROR_VARIABLE out)
	if(NOT res EQUAL 0)
		message(FATAL_ERROR "as68k failed on fe2.s:\n${out}")
	endif()

	# copy what changed, drop what is gone
	file(GLOB new_gen RELATIVE ${GEN68K_WORK} ${GEN68K_WORK}/gen/*.c)
	set(new_files fe2.s.c fixups.h fe2.s.bin ${new_gen})
	foreach(f ${new_files})
		file(SHA256 ${GEN68K_WORK}/${f} new_hash)
		set(old_hash "")
		if(EXISTS ${GEN68K_DIR}/${f})
			file(SHA256 ${GEN68K_DIR}/${f} old_hash)
		endif()
		if(NOT new_hash STREQUAL old_hash)
			get_filename_component(dest ${GEN68K_DIR}/${f} DIRECTORY)
			file(COPY ${GEN68K_WORK}/${f} DESTINATION ${dest})
		endif()
	endforeach()
	file(GLOB old_gen RELATIVE ${GEN68K_DIR} ${GEN68K_DIR}/gen/*.c)
	foreach(f ${old_gen})
		if(NOT f IN_LIST new_files)
			file(REMOVE ${GEN68K_DIR}/${f})
		endif()
	endforeach()
	file(REMOVE_RECURSE ${GEN68K_WORK})
	file(WRITE ${GEN68K_STAMP} "${GEN68K_KEY}")
endif()

file(GLOB GEN_SOURCES ${GEN68K_DIR}/gen/*.c)
if(NOT GEN_SOURCES)
	message(FATAL_ERROR "as68k produced no gen/*.c files")
endif()

# The game loads fe2.s.bin from the current directory at startup, and it has
# to match the generated code: put it next to the executable, and at the top
# of the source tree for those running it from there.
configure_file(${GEN68K_DIR}/fe2.s.bin ${CMAKE_BINARY_DIR}/fe2.s.bin COPYONLY)
configure_file(${GEN68K_DIR}/fe2.s.bin ${CMAKE_SOURCE_DIR}/fe2.s.bin COPYONLY)

set(FRONTIER_SOURCES
	host.c
	${GEN68K_DIR}/fe2.s.c
	${GEN_SOURCES}
	src/audio.c
	src/hostcall.c
	src/keymap.c
	src/main.c
	src/shortcut.c
	src/input.c
	src/joystick.c
	src/ini.c
	src/paths.c
)

if(WITH_GL)
	list(APPEND FRONTIER_SOURCES src/gl.c src/planet.c)
else()
	list(APPEND FRONTIER_SOURCES src/soft.c)
endif()

add_executable(frontier ${FRONTIER_SOURCES})

target_include_directories(frontier PRIVATE
	${CMAKE_SOURCE_DIR}
	${CMAKE_SOURCE_DIR}/src
	${GEN68K_DIR}
)

# -fwrapv: the C translation of the 68k code computes the overflow flag
# from signed additions and subtractions that may overflow, which is
# undefined behaviour in C. clang (macOS) optimizes such code assuming it
# never overflows, and loops relying on the flag ('add.l d0,d0 / bvc', in
# the planet projection code among others) then never end. -fwrapv makes
# signed arithmetic wrap around, like the 68000's does.
target_compile_options(frontier PRIVATE
	-Wall -Wno-unused
	-fwrapv
)

# lets a build run from its build directory find sfx/ etc. (see src/paths.c)
set_source_files_properties(src/paths.c PROPERTIES
	COMPILE_DEFINITIONS "FRONTIER_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")

target_link_libraries(frontier PRIVATE
	PkgConfig::SDL
	PkgConfig::VORBISFILE
	PkgConfig::VORBIS
	PkgConfig::OGG
)

# gl.c / hostcall.c use sqrt, sin, cos... from libm, which is a separate
# library on Linux (it is part of libSystem on macOS).
if(UNIX AND NOT APPLE)
	target_link_libraries(frontier PRIVATE m)
endif()

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

# --- macOS application bundle, made by 'cmake --install' -------------------
#
# Frontier.app/Contents/MacOS/Frontier is the executable built above, still
# linked to the libraries installed on this Mac (SDL, Ogg Vorbis...): the
# application is not meant to be copied to another machine.
# Contents/Resources gets everything the game reads at run time (see
# src/paths.c): fe2.s.bin, sfx/, music/ (if present), joystick.ini and an
# optional Frontier.icns. Saved games go to
# ~/Library/Application Support/Frontier.
if(APPLE)
	set(APP Frontier.app/Contents)

	set(BUNDLE_ICON "")
	if(EXISTS ${CMAKE_SOURCE_DIR}/Frontier.icns)
		set(BUNDLE_ICON "	<key>CFBundleIconFile</key>
	<string>Frontier.icns</string>
")
		install(FILES ${CMAKE_SOURCE_DIR}/Frontier.icns DESTINATION ${APP}/Resources)
	endif()
	file(WRITE ${CMAKE_BINARY_DIR}/Info.plist "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>English</string>
	<key>CFBundleExecutable</key>
	<string>Frontier</string>
	<key>CFBundleIdentifier</key>
	<string>org.glfrontier.frontier</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>Frontier</string>
	<key>CFBundleDisplayName</key>
	<string>Frontier: Elite 2</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>3.0</string>
	<key>CFBundleVersion</key>
	<string>3.0</string>
${BUNDLE_ICON}	<key>NSHighResolutionCapable</key>
	<true/>
</dict>
</plist>
")

	install(PROGRAMS $<TARGET_FILE:frontier> DESTINATION ${APP}/MacOS RENAME Frontier)
	install(FILES ${CMAKE_BINARY_DIR}/Info.plist DESTINATION ${APP})
	install(FILES ${GEN68K_DIR}/fe2.s.bin ${CMAKE_SOURCE_DIR}/joystick.ini DESTINATION ${APP}/Resources)
	install(DIRECTORY ${CMAKE_SOURCE_DIR}/sfx/ DESTINATION ${APP}/Resources/sfx
		FILES_MATCHING PATTERN "*.wav")
	# the music is not part of the repository: put the .ogg files in music/
	if(EXISTS ${CMAKE_SOURCE_DIR}/music)
		install(DIRECTORY ${CMAKE_SOURCE_DIR}/music/ DESTINATION ${APP}/Resources/music
			FILES_MATCHING PATTERN "*.ogg")
	endif()

	# cmake --build build --target dist  ->  build/dist/Frontier.app
	add_custom_target(dist
		COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/dist/Frontier.app
		COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --prefix ${CMAKE_BINARY_DIR}/dist
		DEPENDS frontier
		COMMENT "Making ${CMAKE_BINARY_DIR}/dist/Frontier.app"
		VERBATIM)
endif()
