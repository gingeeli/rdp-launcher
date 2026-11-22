/* rdp-launcher: start the RDP client for a host.
 *
 * FreeRDP gets every option through a pipe (/args-from:fd:N, which FreeRDP
 * only accepts as its sole argument), so the password never appears in argv
 * or the environment, and `ps` can't show it.
 */
#include "rl.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* FreeRDP 3 ships under different names per OS/package. */
static const char *const freerdp_names[] = {
	"sdl-freerdp3", "sdl-freerdp", "xfreerdp3", "xfreerdp", NULL
};

/* FreeRDP logs this when the remote screen is set up, i.e. the session really
 * started. Its exit code can't tell us: closing the window with ✕ exits
 * nonzero too (e.g. 131 after a GNOME Remote Login redirect). */
#define RL_SESSION_STARTED "gdi_init_ex"

int rl_find_freerdp(char *out, size_t n)
{
	const char *over = getenv("RDP_LAUNCHER_FREERDP");
	int i;

	if (over && *over)
		return rl_find_exe(over, plat_exe_dirs(), out, n);
	for (i = 0; freerdp_names[i]; i++)
		if (rl_find_exe(freerdp_names[i], plat_exe_dirs(), out, n) == 0)
			return 0;
	return -1;
}

static int log_path(const char *key, char *out, size_t n)
{
	char base[RL_PATH], dir[RL_PATH];
	int r;

	if (rl_xdg_dir("XDG_STATE_HOME", ".local/state", base, sizeof base))
		return -1;
	r = snprintf(dir, sizeof dir, "%s/rdp-launcher", base);
	if (r < 0 || (size_t)r >= sizeof dir || rl_mkdir_p(dir))
		return -1;
	r = snprintf(out, n, "%s/%s.log", dir, key);
	return (r < 0 || (size_t)r >= n) ? -1 : 0;
}

/* Scan a FreeRDP log: did the session start, and what's the most telling
 * error? Prefers the last ERRCONNECT_* cause (not the follow-on CANCELLED or
 * FreeRDP's "TODO:" notes), else the last [ERROR] line. */
static int scan_log(const char *path, char *why, size_t n)
{
	char line[1024], cause[512] = "", any[512] = "", *msg;
	FILE *f = fopen(path, "r");
	int started = 0;

	why[0] = '\0';
	if (!f)
		return 0;
	while (fgets(line, sizeof line, f)) {
		if (strstr(line, RL_SESSION_STARTED))
			started = 1;
		if (!strstr(line, "[ERROR]") || strstr(line, "TODO:"))
			continue;
		msg = strstr(line, "]: ");
		msg = msg ? msg + 3 : line;
		msg[strcspn(msg, "\r\n")] = '\0';
		rl_copy(any, sizeof any, msg);
		if (strstr(msg, "ERRCONNECT_") && !strstr(msg, "ERRCONNECT_CONNECT_CANCELLED"))
			rl_copy(cause, sizeof cause, msg);
	}
	fclose(f);
	rl_copy(why, n, cause[0] ? cause : any);
	return started;
}

/* Write FreeRDP's option list (one per line) into the pipe. */
static int write_args(int fd, const struct rl_host *h, int ai, const char *pw)
{
	const char *addr = h->addrs[ai];
	int i, bad = 0;

	/* An .rdp file first: FreeRDP loads its settings (screen size, etc.) and
	 * the options after it (address that answered, login) take precedence. */
	if (h->rdpfile[0]) {
		if (access(h->rdpfile, R_OK) == 0)
			bad |= dprintf(fd, "%s\n", h->rdpfile) < 0;
		else
			rl_note("%s is gone; connecting with the saved settings only", h->rdpfile);
	}

	/* An IPv6 literal must be bracketed in /v: */
	if (strchr(addr, ':') && addr[0] != '[')
		bad |= dprintf(fd, "/v:[%s]\n", addr) < 0;
	else
		bad |= dprintf(fd, "/v:%s\n", addr) < 0;
	bad |= dprintf(fd, "/port:%d\n", h->port) < 0;
	if (h->user[0])
		bad |= dprintf(fd, "/u:%s\n", h->user) < 0;
	if (pw)
		bad |= dprintf(fd, "/p:%s\n", pw) < 0;
	else
		bad |= dprintf(fd, "/from-stdin:force\n") < 0;
	bad |= dprintf(fd, "/cert:tofu\n") < 0;
	for (i = 0; i < h->nargs; i++)
		bad |= dprintf(fd, "%s\n", h->args[i]) < 0;
	return bad ? -1 : 0;
}

/* GUI launch: run FreeRDP with output to a log; report it only if the
 * session never started. If the password came from the store, offer to
 * forget it on failure. */
static int run_logged(const struct rl_host *h, const char *exe, char *const argv[], int argfd,
		      int pw_from_store)
{
	char msg[2048];
	char log[RL_PATH], why[512];
	time_t start = time(NULL);
	int st, code, lfd, dn, started;
	pid_t pid;

	if (log_path(h->key, log, sizeof log))
		rl_copy(log, sizeof log, "/dev/null");
	pid = fork();
	if (pid < 0) {
		rl_err("fork: %s", strerror(errno));
		return RL_ERR;
	}
	if (pid == 0) {
		lfd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		if (lfd >= 0) {
			dup2(lfd, STDOUT_FILENO);
			dup2(lfd, STDERR_FILENO);
		}
		dn = open("/dev/null", O_RDONLY);
		if (dn >= 0)
			dup2(dn, STDIN_FILENO);
		execv(exe, argv);
		_exit(127);
	}
	close(argfd);
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			return RL_ERR;
	code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
	started = scan_log(log, why, sizeof why);
	rl_log("FreeRDP for %s exited %d after %lds, session %s (log: %s)", h->key, code,
	       (long)(time(NULL) - start), started ? "started" : "never started", log);
	if (code != 0 && !started) {
		snprintf(msg, sizeof msg, "Couldn't connect to %s (FreeRDP exit %d).\n\n%s\n\nFull log: %s",
			 h->name, code, why[0] ? why : "(no error line in log)", log);
		if (pw_from_store) {
			rl_note("%s", msg);
			strncat(msg, "\n\nIf the saved password is wrong, forget it so you're asked next time.",
				sizeof msg - strlen(msg) - 1);
			if (plat_confirm(msg, "Forget Password", "OK") == 1)
				plat_secret_forget(h->key);
		} else {
			rl_err("%s", msg);
		}
		return RL_ERR;
	}
	return RL_OK;
}

/* Fill in a missing username (dialog or terminal) and remember it. */
static int ensure_user(struct rl_host *h)
{
	char prompt[RL_STR + 64];
	int r;

	if (h->user[0])
		return RL_OK;
	snprintf(prompt, sizeof prompt, "Username for %s: ", h->name);
	if (rl_gui) {
		r = plat_ask(prompt, "", h->user, sizeof h->user);
		if (r > 0)
			return -1; /* cancelled */
		if (r < 0) {
			rl_err("no dialog program to ask for the username");
			return RL_ERR;
		}
	} else if (rl_tty_line(prompt, h->user, sizeof h->user)) {
		rl_err("'user' is empty in %s.conf; set the RDP login name there", h->key);
		return RL_ECONFIG;
	}
	if (rl_host_save(h) != RL_OK)
		rl_note("could not remember the username (continuing)");
	return RL_OK;
}

/* Password: store first, else a dialog (GUI) or FreeRDP's own prompt (tty).
 * Returns 0 with pw filled, 1 = let FreeRDP prompt, -1 cancelled, >1 error. */
static int ensure_password(const struct rl_host *h, char *pw, size_t n, int *from_store)
{
	char prompt[2 * RL_STR + 32];
	int s, r, save;

	*from_store = 0;
	s = plat_secret_get(h->key, pw, n);
	if (s == 0 && pw[0]) {
		if (strpbrk(pw, "\r\n")) {
			rl_err("the stored password for %s contains a line break; set it again", h->key);
			return RL_ESECRET;
		}
		*from_store = 1;
		return 0;
	}
	if (!rl_gui) {
		if (isatty(STDIN_FILENO))
			return 1;
		rl_err("No password stored for %s. In a terminal, run:\n  rdp-launcher set-password %s",
		       h->name, h->key);
		return RL_ESECRET;
	}
	snprintf(prompt, sizeof prompt, "Password for %s on %s:", h->user, h->name);
	r = plat_ask_password(prompt, plat_secret_tool() != NULL, &save, pw, n);
	if (r > 0)
		return -1;
	if (r < 0) {
		rl_err("no dialog program to ask for the password");
		return RL_ERR;
	}
	if (save && plat_secret_put(h->key, pw))
		rl_err("Couldn't save the password (connecting anyway).");
	return 0;
}

/* GUI with neither a username nor a stored password: ask for both in one box.
 * Returns 0 done (h->user and pw filled, user remembered), 1 cancelled,
 * -1 not applicable (fall back to asking one thing at a time). */
static int ask_login_together(struct rl_host *h, char *pw, size_t n)
{
	char prompt[RL_STR + 32];
	int s, r, save;

	if (!rl_gui || h->user[0])
		return -1;
	s = plat_secret_get(h->key, pw, n);
	rl_wipe(pw, n);
	if (s == 0)
		return -1; /* a password is stored: only the username is missing */
	snprintf(prompt, sizeof prompt, "Log in to %s", h->name);
	r = plat_ask_login(prompt, plat_secret_tool() != NULL, &save,
			   h->user, sizeof h->user, pw, n);
	if (r != 0) {
		h->user[0] = '\0';
		return r;
	}
	if (rl_host_save(h) != RL_OK)
		rl_note("could not remember the username (continuing)");
	if (save && plat_secret_put(h->key, pw))
		rl_err("Couldn't save the password (connecting anyway).");
	return 0;
}

int rl_connect(const struct rl_host *hin, int ai)
{
	struct rl_host hc = *hin, *h = &hc;
	char exe[RL_PATH], pw[512], fdarg[32];
	char *argv[3];
	int pfd[2], r, from_store, prompt_tty;

	if (h->client == RL_CLIENT_WINDOWS_APP)
		return plat_open_windows_app(h, ai);

	if (rl_find_freerdp(exe, sizeof exe)) {
		rl_err("FreeRDP not found. %s", plat_freerdp_hint());
		return RL_ENOCLIENT;
	}
	r = ask_login_together(h, pw, sizeof pw);
	if (r > 0) {
		rl_wipe(pw, sizeof pw);
		return RL_OK; /* cancelled */
	}
	if (r == 0) {
		from_store = 0;
		prompt_tty = 0;
	} else {
		r = ensure_user(h);
		if (r)
			return r < 0 ? RL_OK : r;
		r = ensure_password(h, pw, sizeof pw, &from_store);
		if (r < 0 || r > 1) {
			rl_wipe(pw, sizeof pw);
			return r < 0 ? RL_OK : r;
		}
		prompt_tty = (r == 1);
	}

	if (pipe(pfd)) {
		rl_wipe(pw, sizeof pw);
		rl_err("pipe: %s", strerror(errno));
		return RL_ERR;
	}
	r = write_args(pfd[1], h, ai, prompt_tty ? NULL : pw);
	rl_wipe(pw, sizeof pw);
	close(pfd[1]);
	if (r) {
		close(pfd[0]);
		rl_err("could not pass options to FreeRDP");
		return RL_ERR;
	}
	snprintf(fdarg, sizeof fdarg, "/args-from:fd:%d", pfd[0]);
	argv[0] = exe;
	argv[1] = fdarg;
	argv[2] = NULL;

	if (rl_gui)
		return run_logged(h, exe, argv, pfd[0], from_store);

	rl_note("connecting to %s via %s:%d (%s)", h->name, h->addrs[ai], h->port, exe);
	execv(exe, argv);
	rl_err("cannot run %s: %s", exe, strerror(errno));
	return RL_ENOCLIENT;
}
