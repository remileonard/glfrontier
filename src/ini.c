/*
 * Minimal INI file reader, vendored as a drop-in replacement for the
 * system "libini" dependency. See ini.h for the API description.
 *
 * Supports:
 *   [Section Name]
 *   key = value
 *   # full-line comment
 *   ; full-line comment
 *
 * Leading/trailing whitespace around keys and values is trimmed.
 * Blank lines and comment lines are skipped.
 */
#include "ini.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct INI {
	char *data;
	size_t size;
	size_t pos;

	int have_pending;
	const char *pending_name;
	size_t pending_len;
};

static char *ini_next_line(struct INI *ini, size_t *linelen)
{
	while (ini->pos < ini->size) {
		char *start = ini->data + ini->pos;
		char *nl = memchr(start, '\n', ini->size - ini->pos);
		size_t len = nl ? (size_t) (nl - start) : (ini->size - ini->pos);

		ini->pos += len + (nl ? 1 : 0);

		if (len > 0 && start[len - 1] == '\r')
			len--;

		char *p = start;
		size_t l = len;

		while (l > 0 && (*p == ' ' || *p == '\t')) {
			p++;
			l--;
		}

		if (l == 0 || *p == '#' || *p == ';')
			continue;

		while (l > 0 && (p[l - 1] == ' ' || p[l - 1] == '\t'))
			l--;

		*linelen = l;
		return p;
	}

	return NULL;
}

struct INI *ini_open(const char *path)
{
	struct INI *ini;
	FILE *f;
	long size;

	f = fopen(path, "rb");
	if (!f)
		return NULL;

	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}

	size = ftell(f);
	if (size < 0) {
		fclose(f);
		return NULL;
	}
	rewind(f);

	ini = malloc(sizeof(*ini));
	if (!ini) {
		fclose(f);
		return NULL;
	}

	ini->data = malloc((size_t) size + 1);
	if (!ini->data) {
		free(ini);
		fclose(f);
		return NULL;
	}

	if (size > 0 && fread(ini->data, 1, (size_t) size, f) != (size_t) size) {
		free(ini->data);
		free(ini);
		fclose(f);
		return NULL;
	}
	fclose(f);

	ini->data[size] = '\0';
	ini->size = (size_t) size;
	ini->pos = 0;
	ini->have_pending = 0;
	ini->pending_name = NULL;
	ini->pending_len = 0;

	return ini;
}

void ini_close(struct INI *ini)
{
	if (!ini)
		return;

	free(ini->data);
	free(ini);
}

int ini_next_section(struct INI *ini, const char **name, size_t *len)
{
	if (!ini)
		return -1;

	if (ini->have_pending) {
		*name = ini->pending_name;
		*len = ini->pending_len;
		ini->have_pending = 0;
		return 1;
	}

	for (;;) {
		size_t linelen;
		char *line = ini_next_line(ini, &linelen);
		char *end;

		if (!line)
			return 0;

		if (line[0] != '[')
			continue;

		end = memchr(line, ']', linelen);
		if (!end)
			continue;

		*name = line + 1;
		*len = (size_t) (end - (line + 1));
		return 1;
	}
}

int ini_read_pair(struct INI *ini, const char **key, size_t *lkey,
		   const char **val, size_t *lval)
{
	if (!ini)
		return -1;

	if (ini->have_pending)
		return 0;

	for (;;) {
		size_t linelen;
		char *line = ini_next_line(ini, &linelen);
		char *eq;
		char *v;
		size_t klen, vlen;

		if (!line)
			return 0;

		if (line[0] == '[') {
			char *end = memchr(line, ']', linelen);

			if (end) {
				ini->pending_name = line + 1;
				ini->pending_len = (size_t) (end - (line + 1));
				ini->have_pending = 1;
			}
			return 0;
		}

		eq = memchr(line, '=', linelen);
		if (!eq)
			continue;

		klen = (size_t) (eq - line);
		while (klen > 0 && (line[klen - 1] == ' ' || line[klen - 1] == '\t'))
			klen--;

		v = eq + 1;
		vlen = linelen - (size_t) (eq - line) - 1;
		while (vlen > 0 && (*v == ' ' || *v == '\t')) {
			v++;
			vlen--;
		}
		while (vlen > 0 && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t'))
			vlen--;

		*key = line;
		*lkey = klen;
		*val = v;
		*lval = vlen;
		return 1;
	}
}
