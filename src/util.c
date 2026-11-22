/* rdp-launcher: small helpers (messages, strings, paths, running programs). */
#include "rl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int rl_gui = 0;

#define RL_LOG_MAX (256 * 1024) /* rotate to .old beyond this */

/* Append a timestamped line to $XDG_STATE_HOME/rdp-launcher/rdp-launcher.log.
 * Messages never contain passwords, so the log is safe to keep. */
static void log_line(const char *kind, const char *msg)
{
	char base[RL_PATH], dir[RL_PATH + 16], path[RL_PATH + 40], old[RL_PATH + 48], when[32];
	struct stat st;
	struct tm tm;
	time_t now = time(NULL);
	FILE *f;
	int fd, r;

	if (rl_xdg_dir("XDG_STATE_HOME", ".local/state", base, sizeof base))
		return;
	r = snprintf(dir, sizeof dir, "%s/rdp-launcher", base);
	if (r < 0 || (size_t)r >= sizeof dir || rl_mkdir_p(dir))
		return;
	snprintf(path, sizeof path, "%s/rdp-launcher.log", dir);
	if (stat(path, &st) == 0 && st.st_size > RL_LOG_MAX) {
		snprintf(old, sizeof old, "%s.old", path);
		rename(path, old);
	}
	fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
	f = fd >= 0 ? fdopen(fd, "a") : NULL;
	if (!f) {
		if (fd >= 0)
			close(fd);
		return;
	}
	localtime_r(&now, &tm);
	strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
	fprintf(f, "%s %-5s %s%s\n", when, kind, rl_gui ? "[app] " : "", msg);
	fclose(f);
}

static void vmsg(int dialog, const char *fmt, va_list ap)
{
	char buf[2048], *p;

	vsnprintf(buf, sizeof buf, fmt, ap);
	fprintf(stderr, "rdp-launcher: %s\n", buf);
	if (dialog || rl_gui) {
		char flat[2048];
		rl_copy(flat, sizeof flat, buf);
		for (p = flat; *p; p++)  /* one log line per message */
			if (*p == '\n')
				*p = ' ';
		log_line(dialog ? "error" : "note", flat);
	}
	if (dialog && rl_gui)
		plat_dialog_error(buf);
}

void rl_log(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	log_line("info", buf);
}

void rl_err(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vmsg(1, fmt, ap);
	va_end(ap);
}

void rl_note(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vmsg(0, fmt, ap);
	va_end(ap);
}

int rl_copy(char *dst, size_t n, const char *src)
{
	size_t len = strlen(src);

	if (n == 0)
		return -1;
	if (len >= n) {
		memcpy(dst, src, n - 1);
		dst[n - 1] = '\0';
		return -1;
	}
	memcpy(dst, src, len + 1);
	return 0;
}

/* Overwrite secrets; volatile stops the compiler dropping the stores. */
void rl_wipe(void *p, size_t n)
{
	volatile unsigned char *v = p;

	while (n--)
		*v++ = 0;
}

int rl_mkdir_p(const char *path)
{
	char tmp[RL_PATH];
	char *p;

	if (rl_copy(tmp, sizeof tmp, path))
		return -1;
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0700) && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0700) && errno != EEXIST)
		return -1;
	return 0;
}

/* $env if set and absolute, else $HOME/fallback (XDG Base Directory rules). */
int rl_xdg_dir(const char *env, const char *fallback, char *out, size_t n)
{
	const char *v = getenv(env);
	const char *home;
	int r;

	if (v && v[0] == '/')
		return rl_copy(out, n, v);
	home = getenv("HOME");
	if (!home || home[0] != '/')
		return -1;
	r = snprintf(out, n, "%s/%s", home, fallback);
	return (r < 0 || (size_t)r >= n) ? -1 : 0;
}

static int is_exe(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

/* Search $PATH, then extra_dirs (GUI launches often get a minimal PATH). */
int rl_find_exe(const char *name, const char *const extra_dirs[], char *out, size_t n)
{
	const char *path = getenv("PATH");
	char dirs[4096];
	char *save = NULL, *d;
	int i, r;

	if (strchr(name, '/'))
		return is_exe(name) ? rl_copy(out, n, name) : -1;
	if (path && rl_copy(dirs, sizeof dirs, path) == 0) {
		for (d = strtok_r(dirs, ":", &save); d; d = strtok_r(NULL, ":", &save)) {
			r = snprintf(out, n, "%s/%s", d[0] ? d : ".", name);
			if (r > 0 && (size_t)r < n && is_exe(out))
				return 0;
		}
	}
	for (i = 0; extra_dirs && extra_dirs[i]; i++) {
		r = snprintf(out, n, "%s/%s", extra_dirs[i], name);
		if (r > 0 && (size_t)r < n && is_exe(out))
			return 0;
	}
	return -1;
}

static int wait_status(pid_t pid)
{
	int st;

	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int rl_run(char *const argv[])
{
	pid_t pid = fork();

	if (pid < 0)
		return -1;
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	return wait_status(pid);
}

/* Run argv with `in` on stdin, stdout/stderr on /dev/null. Exit status or -1. */
int rl_run_input(char *const argv[], const char *in, size_t len)
{
	int pfd[2], dn;
	ssize_t w;
	size_t off = 0;
	pid_t pid;

	if (pipe(pfd))
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pfd[0], STDIN_FILENO);
		close(pfd[0]);
		close(pfd[1]);
		dn = open("/dev/null", O_WRONLY);
		if (dn >= 0) {
			dup2(dn, STDOUT_FILENO);
			dup2(dn, STDERR_FILENO);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pfd[0]);
	while (off < len) {
		w = write(pfd[1], in + off, len - off);
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			break;
		off += (size_t)w;
	}
	close(pfd[1]);
	return wait_status(pid);
}

/* One line from the terminal (echo on). */
int rl_tty_line(const char *prompt, char *out, size_t n)
{
	if (!isatty(STDIN_FILENO))
		return -1;
	fprintf(stderr, "%s", prompt);
	fflush(stderr);
	if (!fgets(out, (int)n, stdin))
		return -1;
	out[strcspn(out, "\r\n")] = '\0';
	return out[0] ? 0 : -1;
}

/* Run argv with stdin/stderr on /dev/null; capture stdout (NUL-terminated). */
int rl_run_capture(char *const argv[], char *out, size_t outsz)
{
	int pfd[2], dn;
	size_t len = 0;
	ssize_t r;
	char junk[256];
	pid_t pid;

	if (outsz == 0 || pipe(pfd))
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pfd[1], STDOUT_FILENO);
		close(pfd[0]);
		close(pfd[1]);
		dn = open("/dev/null", O_RDWR);
		if (dn >= 0) {
			dup2(dn, STDIN_FILENO);
			dup2(dn, STDERR_FILENO);
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	close(pfd[1]);
	for (;;) {
		r = read(pfd[0], out + len, outsz - 1 - len);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		len += (size_t)r;
		if (len >= outsz - 1)
			break;
	}
	while ((r = read(pfd[0], junk, sizeof junk)) > 0 || (r < 0 && errno == EINTR))
		;
	rl_wipe(junk, sizeof junk);
	close(pfd[0]);
	out[len] = '\0';
	return wait_status(pid);
}
