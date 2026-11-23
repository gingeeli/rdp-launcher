/* rdp-launcher: config location and host-file parsing. */
#include "rl.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

static const char host_template[] =
"# rdp-launcher host file: one machine per file; the file name is its key.\n"
"# Lines are \"key = value\"; \"#\" at the start or after a space starts a comment.\n"
"\n"
"# Shown in the picker.\n"
"name      = %s\n"
"\n"
"# Space-separated, tried in order; the first that answers is used.\n"
"# Put the Bonjour/avahi name (<hostname>.local) first and an IP as a fallback.\n"
"addresses = %s.local\n"
"\n"
"# 3389 = GNOME Remote Login (headless). GNOME Desktop Sharing moves to 3390\n"
"# when Remote Login is also on.\n"
"port      = 3389\n"
"\n"
"# RDP login name. For GNOME Remote Login: the one set with\n"
"#   sudo grdctl --system rdp set-credentials\n"
"user      =\n"
"\n"
"# freerdp (any OS) or windows-app (macOS only; opens Microsoft Windows App).\n"
"client    = freerdp\n"
"\n"
"# Extra FreeRDP options, space-separated (no quoting).\n"
"args      = /dynamic-resolution\n";

/* Keys become file names and password-store labels: keep them boring. */
int rl_valid_key(const char *key)
{
	const char *p;
	size_t len = strlen(key);

	if (len == 0 || len >= RL_KEY || key[0] == '.' || key[0] == '_' || key[0] == '-')
		return 0;
	for (p = key; *p; p++) {
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		      (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.'))
			return 0;
	}
	return 1;
}

int rl_config_dir(char *out, size_t n)
{
	char base[RL_PATH];
	int r;

	if (rl_xdg_dir("XDG_CONFIG_HOME", ".config", base, sizeof base))
		return -1;
	r = snprintf(out, n, "%s/rdp-launcher", base);
	return (r < 0 || (size_t)r >= n) ? -1 : 0;
}

int rl_hosts_dir(char *out, size_t n, int create)
{
	char cfg[RL_PATH];
	struct stat st;
	int r;

	if (rl_config_dir(cfg, sizeof cfg))
		return -1;
	r = snprintf(out, n, "%s/hosts", cfg);
	if (r < 0 || (size_t)r >= n)
		return -1;
	if (create && stat(out, &st) != 0) {
		if (rl_mkdir_p(out)) {
			rl_err("cannot create %s: %s", out, strerror(errno));
			return -1;
		}
		rl_note("created %s (put one <name>.conf per machine here)", out);
	}
	return 0;
}

int rl_host_path(const char *key, char *out, size_t n)
{
	char dir[RL_PATH];
	int r;

	if (rl_hosts_dir(dir, sizeof dir, 1))
		return -1;
	r = snprintf(out, n, "%s/%s.conf", dir, key);
	return (r < 0 || (size_t)r >= n) ? -1 : 0;
}

static char *trim(char *s)
{
	char *e;

	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
		*--e = '\0';
	return s;
}

static void strip_comment(char *s)
{
	char *p;

	for (p = s; *p; p++) {
		if (*p == '#' && (p == s || p[-1] == ' ' || p[-1] == '\t')) {
			*p = '\0';
			return;
		}
	}
}

/* Split v on blanks into list; returns count or -1 if too many / too long. */
static int split(char *v, char list[][RL_STR], int max)
{
	char *save = NULL, *tok;
	int n = 0;

	for (tok = strtok_r(v, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
		if (n == max || rl_copy(list[n], RL_STR, tok))
			return -1;
		n++;
	}
	return n;
}

int rl_host_load(const char *key, struct rl_host *h)
{
	char path[RL_PATH], line[1024];
	char *k, *v, *eq;
	FILE *f;
	int lineno = 0, bad = 0, i;
	long port;
	char *end;

	memset(h, 0, sizeof *h);
	if (!rl_valid_key(key)) {
		rl_err("'%s' is not a valid host name (letters, digits, . _ -)", key);
		return RL_ECONFIG;
	}
	if (rl_host_path(key, path, sizeof path))
		return RL_ECONFIG;
	f = fopen(path, "r");
	if (!f) {
		rl_err("no host '%s' (%s: %s). Create it with: rdp-launcher add %s",
		       key, path, strerror(errno), key);
		return RL_ECONFIG;
	}
	rl_copy(h->key, sizeof h->key, key);
	rl_copy(h->name, sizeof h->name, key);
	h->port = 3389;
	h->client = RL_CLIENT_FREERDP;

	while (fgets(line, sizeof line, f)) {
		lineno++;
		if (!strchr(line, '\n') && !feof(f)) {
			rl_err("%s:%d: line too long", path, lineno);
			bad = 1;
			break;
		}
		strip_comment(line);
		k = trim(line);
		if (!*k)
			continue;
		eq = strchr(k, '=');
		if (!eq) {
			rl_err("%s:%d: expected 'key = value'", path, lineno);
			bad = 1;
			continue;
		}
		*eq = '\0';
		k = trim(k);
		v = trim(eq + 1);

		if (!strcmp(k, "name")) {
			if (*v && rl_copy(h->name, sizeof h->name, v))
				bad = 1, rl_err("%s:%d: name too long", path, lineno);
		} else if (!strcmp(k, "addresses")) {
			h->naddrs = split(v, h->addrs, RL_MAX_ADDRS);
			if (h->naddrs < 0)
				bad = 1, rl_err("%s:%d: at most %d addresses", path, lineno, RL_MAX_ADDRS);
		} else if (!strcmp(k, "port")) {
			errno = 0;
			port = strtol(v, &end, 10);
			if (errno || *end || end == v || port < 1 || port > 65535)
				bad = 1, rl_err("%s:%d: port must be 1-65535", path, lineno);
			else
				h->port = (int)port;
		} else if (!strcmp(k, "user")) {
			if (rl_copy(h->user, sizeof h->user, v))
				bad = 1, rl_err("%s:%d: user too long", path, lineno);
		} else if (!strcmp(k, "client")) {
			if (!strcmp(v, "freerdp"))
				h->client = RL_CLIENT_FREERDP;
			else if (!strcmp(v, "windows-app"))
				h->client = RL_CLIENT_WINDOWS_APP;
			else
				bad = 1, rl_err("%s:%d: client must be freerdp or windows-app", path, lineno);
		} else if (!strcmp(k, "args")) {
			h->nargs = split(v, h->args, RL_MAX_ARGS);
			if (h->nargs < 0)
				bad = 1, rl_err("%s:%d: at most %d args", path, lineno, RL_MAX_ARGS);
		} else if (!strcmp(k, "env")) {
			h->nenv = split(v, h->env, RL_MAX_ENV);
			if (h->nenv < 0)
				bad = 1, rl_err("%s:%d: at most %d env entries", path, lineno, RL_MAX_ENV);
			for (i = 0; i < h->nenv; i++)
				if (h->env[i][0] == '=' || !strchr(h->env[i], '=')) {
					bad = 1, rl_err("%s:%d: env entries must be NAME=value", path, lineno);
					break;
				}
		} else if (!strcmp(k, "rdpfile")) {
			if (rl_copy(h->rdpfile, sizeof h->rdpfile, v))
				bad = 1, rl_err("%s:%d: rdpfile path too long", path, lineno);
		} else {
			rl_note("%s:%d: unknown key '%s' ignored", path, lineno, k);
		}
	}
	fclose(f);
	if (!bad && h->naddrs <= 0) {
		rl_err("%s: no addresses", path);
		bad = 1;
	}
	return bad ? RL_ECONFIG : RL_OK;
}

/* Key for a typed-in address: file-name-safe, port appended if not 3389. */
void rl_make_key(const char *addr, int port, char *out, size_t n)
{
	char tmp[RL_KEY];
	size_t i, j = 0;
	char c;

	for (i = 0; addr[i] && j < sizeof tmp - 1; i++) {
		c = addr[i];
		if (c == '[' || c == ']')
			continue;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			c = '-';
		if (j == 0 && (c == '.' || c == '_' || c == '-'))
			continue;
		tmp[j++] = c;
	}
	tmp[j] = '\0';
	if (j == 0)
		rl_copy(tmp, sizeof tmp, "host");
	if (port != 3389)
		snprintf(out, n, "%.48s-%d", tmp, port);
	else
		rl_copy(out, n, tmp);
}

int rl_host_exists(const char *key)
{
	char path[RL_PATH];
	struct stat st;

	return rl_valid_key(key) && rl_host_path(key, path, sizeof path) == 0 &&
	       stat(path, &st) == 0;
}

/* Rewrite a host file from the struct (temp file + rename). */
int rl_host_save(const struct rl_host *h)
{
	char path[RL_PATH], tmp[RL_PATH + 8];
	FILE *f;
	int i;

	if (!rl_valid_key(h->key) || rl_host_path(h->key, path, sizeof path))
		return RL_ECONFIG;
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	f = fopen(tmp, "w");
	if (!f) {
		rl_err("cannot write %s: %s", tmp, strerror(errno));
		return RL_ECONFIG;
	}
	fprintf(f, "# Saved by rdp-launcher; safe to edit (see `man rdp-launcher`).\n");
	fprintf(f, "name      = %s\n", h->name);
	fprintf(f, "addresses =");
	for (i = 0; i < h->naddrs; i++)
		fprintf(f, " %s", h->addrs[i]);
	fprintf(f, "\nport      = %d\n", h->port);
	fprintf(f, "user      = %s\n", h->user);
	fprintf(f, "client    = %s\n",
		h->client == RL_CLIENT_WINDOWS_APP ? "windows-app" : "freerdp");
	fprintf(f, "args      =");
	for (i = 0; i < h->nargs; i++)
		fprintf(f, " %s", h->args[i]);
	fprintf(f, "\n");
	if (h->nenv > 0) {
		fprintf(f, "env       =");
		for (i = 0; i < h->nenv; i++)
			fprintf(f, " %s", h->env[i]);
		fprintf(f, "\n");
	}
	if (h->rdpfile[0])
		fprintf(f, "rdpfile   = %s\n", h->rdpfile);
	if (fclose(f) || rename(tmp, path)) {
		rl_err("cannot write %s: %s", path, strerror(errno));
		remove(tmp);
		return RL_ECONFIG;
	}
	return RL_OK;
}

int rl_host_remove(const char *key)
{
	char path[RL_PATH];

	if (!rl_valid_key(key) || rl_host_path(key, path, sizeof path))
		return RL_ECONFIG;
	if (remove(path)) {
		rl_err("cannot remove %s: %s", path, strerror(errno));
		return RL_ECONFIG;
	}
	plat_secret_forget(key);
	return RL_OK;
}

void rl_host_touch(const char *key)
{
	char path[RL_PATH];

	if (rl_host_path(key, path, sizeof path) == 0)
		utimensat(AT_FDCWD, path, NULL, 0);
}

struct entry {
	char key[RL_KEY];
	time_t mtime;
};

static int cmp_name(const void *a, const void *b)
{
	return strcmp(((const struct entry *)a)->key, ((const struct entry *)b)->key);
}

static int cmp_recent(const void *a, const void *b)
{
	const struct entry *x = a, *y = b;

	if (x->mtime != y->mtime)
		return x->mtime > y->mtime ? -1 : 1;
	return strcmp(x->key, y->key);
}

int rl_host_list(char keys[][RL_KEY], int max, int by_recent)
{
	char dir[RL_PATH], path[RL_PATH + RL_KEY + 8];
	struct entry list[RL_MAX_HOSTS];
	struct dirent *e;
	struct stat st;
	DIR *d;
	size_t len;
	int n = 0, i;

	if (max > RL_MAX_HOSTS)
		max = RL_MAX_HOSTS;
	if (rl_hosts_dir(dir, sizeof dir, 1))
		return -1;
	d = opendir(dir);
	if (!d) {
		rl_err("cannot read %s: %s", dir, strerror(errno));
		return -1;
	}
	while ((e = readdir(d)) && n < max) {
		len = strlen(e->d_name);
		if (len <= 5 || len - 5 >= RL_KEY || strcmp(e->d_name + len - 5, ".conf"))
			continue;
		memcpy(list[n].key, e->d_name, len - 5);
		list[n].key[len - 5] = '\0';
		if (!rl_valid_key(list[n].key))
			continue;
		snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
		list[n].mtime = stat(path, &st) == 0 ? st.st_mtime : 0;
		n++;
	}
	closedir(d);
	qsort(list, (size_t)n, sizeof list[0], by_recent ? cmp_recent : cmp_name);
	for (i = 0; i < n; i++)
		rl_copy(keys[i], RL_KEY, list[i].key);
	return n;
}

int rl_host_create(const char *key)
{
	char path[RL_PATH];
	struct stat st;
	FILE *f;

	if (!rl_valid_key(key)) {
		rl_err("'%s' is not a valid host name (letters, digits, . _ -)", key);
		return RL_USAGE;
	}
	if (rl_host_path(key, path, sizeof path))
		return RL_ECONFIG;
	if (stat(path, &st) == 0) {
		rl_err("%s already exists", path);
		return RL_ECONFIG;
	}
	f = fopen(path, "w");
	if (!f) {
		rl_err("cannot write %s: %s", path, strerror(errno));
		return RL_ECONFIG;
	}
	fprintf(f, host_template, key, key);
	if (fclose(f)) {
		rl_err("cannot write %s: %s", path, strerror(errno));
		return RL_ECONFIG;
	}
	printf("%s\n", path);
	return RL_OK;
}

/* "host", "host:port", "[v6]:port" or a bare IPv6 literal. */
int rl_parse_target(const char *in, char *host, size_t n, int *port)
{
	const char *colon, *end;
	char *e;
	long p;

	*port = 3389;
	if (in[0] == '[') {
		end = strchr(in, ']');
		if (!end || (size_t)(end - in - 1) >= n || end == in + 1)
			return -1;
		memcpy(host, in + 1, (size_t)(end - in - 1));
		host[end - in - 1] = '\0';
		colon = end[1] == ':' ? end + 1 : NULL;
		if (end[1] && !colon)
			return -1;
	} else {
		colon = strchr(in, ':');
		if (colon && strchr(colon + 1, ':'))
			colon = NULL; /* several colons: IPv6 literal without port */
		if (rl_copy(host, n, in))
			return -1;
		if (colon)
			host[colon - in] = '\0';
		if (!host[0] || strpbrk(host, " \t"))
			return -1;
	}
	if (colon) {
		p = strtol(colon + 1, &e, 10);
		if (*e || e == colon + 1 || p < 1 || p > 65535)
			return -1;
		*port = (int)p;
	}
	return 0;
}

/* Read an .rdp file (Microsoft's "key:type:value" format) into a host.
 * Windows saves these as UTF-16LE with a BOM; other tools write UTF-8.
 * Only what the launcher itself needs is read (address, port, username);
 * every other setting reaches FreeRDP because h->rdpfile is passed to it. */
int rl_rdp_import(const char *path, struct rl_host *h)
{
	static unsigned char raw[64 * 1024];
	static char text[64 * 1024];
	char host[RL_STR], *line, *save = NULL, *base, *abs;
	size_t len, i, j = 0;
	int port = 3389, file_port = 0, have_addr = 0;
	FILE *f;
	long v;

	memset(h, 0, sizeof *h);
	f = fopen(path, "rb");
	if (!f) {
		rl_err("cannot open %s: %s", path, strerror(errno));
		return RL_ECONFIG;
	}
	len = fread(raw, 1, sizeof raw - 1, f);
	fclose(f);

	if (len >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {        /* UTF-16LE */
		for (i = 2; i + 1 < len && j < sizeof text - 1; i += 2) {
			unsigned c = raw[i] | (unsigned)raw[i + 1] << 8;
			text[j++] = c < 128 ? (char)c : '?';
		}
	} else {
		i = (len >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
		for (; i < len && j < sizeof text - 1; i++)
			text[j++] = raw[i] ? (char)raw[i] : ' ';
	}
	text[j] = '\0';

	for (line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
		if (!strncasecmp(line, "full address:s:", 15)) {
			if (rl_parse_target(trim(line + 15), host, sizeof host, &port) == 0)
				have_addr = 1;
		} else if (!strncasecmp(line, "server port:i:", 14)) {
			v = strtol(line + 14, NULL, 10);
			if (v > 0 && v <= 65535)
				file_port = (int)v;
		} else if (!strncasecmp(line, "username:s:", 11)) {
			rl_copy(h->user, sizeof h->user, trim(line + 11));
		}
	}
	if (!have_addr) {
		rl_err("%s has no usable \"full address\" line", path);
		return RL_ECONFIG;
	}
	if (file_port && port == 3389) /* "server port" only applies without :port */
		port = file_port;

	rl_make_key(host, port, h->key, sizeof h->key);
	base = strrchr(path, '/');
	base = base ? base + 1 : (char *)path;
	rl_copy(h->name, sizeof h->name, base);
	len = strlen(h->name);
	if (len > 4 && !strcasecmp(h->name + len - 4, ".rdp"))
		h->name[len - 4] = '\0';
	rl_copy(h->addrs[0], RL_STR, host);
	h->naddrs = 1;
	h->port = port;
	h->client = RL_CLIENT_FREERDP;
	abs = realpath(path, NULL);
	rl_copy(h->rdpfile, sizeof h->rdpfile, abs ? abs : path);
	free(abs);
	return RL_OK;
}
