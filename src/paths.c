/*
 * Where the game's files live: see paths.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir(p, m) _mkdir (p)
#endif

#include "paths.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static char resource_dir[PATH_MAX];

/* the full path of the running executable, or "" */
static void exe_path (const char *argv0, char *out, size_t size)
{
	out[0] = '\0';
#if defined(__APPLE__)
	{
		char buf[PATH_MAX];
		uint32_t len = sizeof (buf);
		if (_NSGetExecutablePath (buf, &len) == 0 && realpath (buf, out)) return;
	}
#elif defined(_WIN32)
	if (GetModuleFileNameA (NULL, out, (DWORD) size) > 0) {
		char *p;
		for (p = out; *p; p++) if (*p == '\\') *p = '/';
		return;
	}
#else
	{
		ssize_t n = readlink ("/proc/self/exe", out, size - 1);
		if (n > 0) {
			out[n] = '\0';
			return;
		}
	}
#endif
	/* last resort */
	if (argv0 && strchr (argv0, '/')) {
#ifndef _WIN32
		if (realpath (argv0, out)) return;
#endif
		snprintf (out, size, "%s", argv0);
	}
}

static void mkdir_p (const char *path)
{
	char buf[PATH_MAX], *p;

	snprintf (buf, sizeof (buf), "%s", path);
	for (p = buf + 1; *p; p++) {
		if (*p != '/') continue;
		*p = '\0';
		mkdir (buf, 0755);
		*p = '/';
	}
	mkdir (buf, 0755);
}

void Paths_Init (const char *argv0)
{
	char exe[PATH_MAX], *slash;
	size_t len;
	static const char bundle_suffix[] = "/Contents/MacOS";

	exe_path (argv0, exe, sizeof (exe));
	slash = strrchr (exe, '/');
	if (!slash) return;
	*slash = '\0';

	len = strlen (exe);
	if (len > sizeof (bundle_suffix) - 1 &&
	    strcmp (exe + len - (sizeof (bundle_suffix) - 1), bundle_suffix) == 0) {
		/* Frontier.app/Contents/MacOS/frontier */
		const char *home = getenv ("HOME");
		char data[PATH_MAX], savs[PATH_MAX];

		exe[len - (sizeof ("/MacOS") - 1)] = '\0';
		snprintf (resource_dir, sizeof (resource_dir), "%s/Resources", exe);

		/* launched from the Finder the current directory is /, which is
		 * not writable: saves go to Application Support instead */
		if (home) {
			snprintf (data, sizeof (data), "%s/Library/Application Support/Frontier", home);
			snprintf (savs, sizeof (savs), "%s/savs", data);
			mkdir_p (savs);
			if (chdir (data) != 0)
				fprintf (stderr, "Cannot use %s for the saved games\n", data);
		}
	} else {
		snprintf (resource_dir, sizeof (resource_dir), "%s", exe);
	}
}

const char *Paths_Resource (const char *rel)
{
	static char buf[4][PATH_MAX];
	static int next;
	char *b;

	if (!resource_dir[0] || rel[0] == '/' || access (rel, R_OK) == 0)
		return rel;
	b = buf[next++ & 3];
	snprintf (b, PATH_MAX, "%s/%s", resource_dir, rel);
	return b;
}
