/* rdp-launcher: shared declarations. Include this first in every .c file. */
#ifndef RL_H
#define RL_H

#define _POSIX_C_SOURCE 200809L

#include <stddef.h>
#include <sys/types.h>

#ifndef RL_VERSION
#define RL_VERSION "dev"
#endif

#define RL_MAX_ADDRS 8
#define RL_MAX_ARGS  16
#define RL_MAX_ENV   8
#define RL_STR       256
#define RL_KEY       64
#define RL_PATH      1024
#define RL_MAX_HOSTS 128

enum rl_exit {
	RL_OK        = 0,
	RL_ERR       = 1, /* generic failure */
	RL_USAGE     = 2,
	RL_ECONFIG   = 3, /* bad or missing host file */
	RL_EUNREACH  = 4, /* no address answered */
	RL_ENOCLIENT = 5, /* FreeRDP / Windows App not available */
	RL_ESECRET   = 6  /* password store problem */
};

enum rl_client { RL_CLIENT_FREERDP, RL_CLIENT_WINDOWS_APP };

struct rl_host {
	char key[RL_KEY];             /* file name without ".conf" */
	char name[RL_STR];            /* display name */
	char addrs[RL_MAX_ADDRS][RL_STR];
	int  naddrs;
	int  port;
	char user[RL_STR];
	enum rl_client client;
	char args[RL_MAX_ARGS][RL_STR];
	int  nargs;
	char rdpfile[RL_PATH];        /* optional .rdp file whose settings FreeRDP also loads */
	char env[RL_MAX_ENV][RL_STR]; /* NAME=value pairs added to FreeRDP's environment */
	int  nenv;
};

/* util.c */
extern int rl_gui;                       /* 1: errors are also shown as dialogs */
void rl_err(const char *fmt, ...);       /* "rdp-launcher: ..." to stderr (+ dialog) */
void rl_note(const char *fmt, ...);      /* "rdp-launcher: ..." to stderr only */
void rl_log(const char *fmt, ...);       /* log file only (connect attempts etc.) */
int  rl_copy(char *dst, size_t n, const char *src);  /* 0 ok, -1 truncated */
void rl_wipe(void *p, size_t n);
int  rl_mkdir_p(const char *path);       /* mode 0700 for new dirs */
int  rl_xdg_dir(const char *env, const char *fallback, char *out, size_t n);
int  rl_find_exe(const char *name, const char *const extra_dirs[], char *out, size_t n);
int  rl_run(char *const argv[]);         /* inherit stdio, wait, exit status or -1 */
int  rl_run_capture(char *const argv[], char *out, size_t outsz);
int  rl_run_input(char *const argv[], const char *in, size_t len); /* feed stdin */
int  rl_tty_line(const char *prompt, char *out, size_t n);  /* 0 ok, -1 none */

/* config.c */
int  rl_valid_key(const char *key);
void rl_make_key(const char *addr, int port, char *out, size_t n);
int  rl_config_dir(char *out, size_t n);
int  rl_hosts_dir(char *out, size_t n, int create);
int  rl_host_path(const char *key, char *out, size_t n);
int  rl_host_exists(const char *key);
int  rl_host_load(const char *key, struct rl_host *h);   /* RL_OK or RL_ECONFIG */
int  rl_host_save(const struct rl_host *h);             /* RL_OK or RL_ECONFIG */
int  rl_host_remove(const char *key);                   /* file + stored password */
void rl_host_touch(const char *key);                    /* mark as recently used */
int  rl_host_list(char keys[][RL_KEY], int max, int by_recent); /* count, or -1 */
int  rl_host_create(const char *key);                   /* write template */
int  rl_parse_target(const char *in, char *host, size_t n, int *port); /* host[:port] */
int  rl_rdp_import(const char *path, struct rl_host *h); /* .rdp file -> host */

/* probe.c */
int  rl_probe(const char *addr, int port, int timeout_ms); /* 0 open, 1 no answer, 2 no such name */
int  rl_pick_addr(const struct rl_host *h, int timeout_ms); /* index of first open, or -1 */

/* client.c */
int  rl_find_freerdp(char *out, size_t n);
int  rl_connect(const struct rl_host *h, int addr_idx);

/* platform.c: the only file with OS-specific code */
const char *plat_name(void);
const char *const *plat_exe_dirs(void);    /* extra dirs to search beyond $PATH */
const char *plat_freerdp_hint(void);
const char *plat_secret_tool(void);        /* NULL if none */
const char *plat_secret_hint(void);
const char *plat_unreach_hint(void);       /* extra text for "can't reach" in the app */
int  plat_permission_wait_s(void);         /* GUI retry window for a first LAN connect */
void plat_client_env(void);                /* OS defaults for FreeRDP's environment */
int  plat_secret_get(const char *key, char *out, size_t n); /* 0 found, 1 none, -1 no tool */
int  plat_secret_set(const char *key);     /* interactive; 0 ok */
int  plat_secret_put(const char *key, const char *pw); /* non-interactive; 0 ok */
int  plat_secret_forget(const char *key);  /* 0 ok, 1 none, -1 no tool */
const char *plat_picker(void);             /* NULL if none */

/* Main picker: saved hosts plus Connect / New… / Remove… / Cancel buttons.
 * Returns an rl_pick_action; chosen gets the host key for CONNECT/REMOVE
 * (empty for REMOVE when the dialog can't tell which host was selected). */
enum rl_pick_action { RL_PICK_UNSUPPORTED = -1, RL_PICK_CONNECT = 0, RL_PICK_CANCEL = 1,
		      RL_PICK_NEW = 2, RL_PICK_REMOVE = 3, RL_PICK_OPEN = 4 };
int  plat_pick_main(char keys[][RL_KEY], char names[][RL_STR], int n, char *chosen, size_t csz);
int  plat_choose_rdp_file(char *out, size_t n);  /* file chooser: 0 chosen, 1 cancel, -1 */

/* Dialogs: 0 ok, 1 cancelled, -1 no dialog program. */
int  plat_pick(const char *prompt, const char *ok_label, char keys[][RL_KEY],
	       char names[][RL_STR], int n, char *chosen, size_t csz);
int  plat_ask(const char *prompt, const char *def, char *out, size_t n);
int  plat_ask_password(const char *prompt, int offer_save, int *save, char *out, size_t n);
/* Username + password in one dialog; -1 if this platform's dialog tool can't. */
int  plat_ask_login(const char *prompt, int offer_save, int *save,
		    char *user, size_t usz, char *pw, size_t psz);
int  plat_confirm(const char *msg, const char *yes, const char *no); /* 1 yes, 0 no, -1 */
void plat_dialog_error(const char *msg);
int  plat_open_windows_app(const struct rl_host *h, int addr_idx);

#endif
