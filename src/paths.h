/*
 * Where the game's files live.
 *
 * Read-only data (fe2.s.bin, sfx/, music/, joystick.ini) is looked up in
 * the current directory first, then in the resource directory: next to the
 * executable, or Contents/Resources inside a macOS application bundle; and
 * last in the source tree the executable was built from, when that is
 * known (FRONTIER_SOURCE_DIR), for development builds run from anywhere.
 *
 * Inside a bundle the current directory is also moved to a writable place,
 * ~/Library/Application Support/Frontier, where the game keeps its saves
 * (./savs). Anywhere else the current directory is left alone, as before.
 */
#ifndef _PATHS_H
#define _PATHS_H

void Paths_Init (const char *argv0);

/* rel if it exists relative to the current directory, else the same path
 * in the resource directory. Valid until the 4th next call. */
const char *Paths_Resource (const char *rel);

#endif /* _PATHS_H */
