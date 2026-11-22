/* rdp-launcher: open a remote desktop to a named machine.
 * Hosts live in ~/.config/rdp-launcher/hosts/<name>.conf (see `add`).
 */
#include "rl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PROBE_MS 2000

static void usage(FILE *f)
{
	fprintf(f,
"usage: rdp-launcher <host>                 connect\n"
"       rdp-launcher pick                   dialog: new connection or a saved one\n"
"       rdp-launcher new                    dialog: type an address, connect, save it\n"
"       rdp-launcher open <file.rdp>        connect using an .rdp file (also: rdp-launcher x.rdp)\n"
"       rdp-launcher list                   list hosts and whether they answer\n"
"       rdp-launcher test <host>            probe each address of a host\n"
"       rdp-launcher add <host>             create a host file and open it in $EDITOR\n"
"       rdp-launcher remove <host>          delete a saved host and its password\n"
"       rdp-launcher set-password <host>    store the RDP password (asks for it)\n"
"       rdp-launcher forget-password <host>\n"
"       rdp-launcher doctor                 check this machine's setup\n"
"       rdp-launcher connect <host>         connect (for a host named like a command)\n"
"       rdp-launcher --version | --help\n"
"\n"
"Host files: $XDG_CONFIG_HOME/rdp-launcher/hosts/<host>.conf\n"
"            (default ~/.config/rdp-launcher/hosts/)\n");
}

/* Pseudo-entries in the picker; '+'/'-' can never start a real host key. */
#define PICK_NEW    "+new"
#define PICK_REMOVE "-remove"

static int cmd_connect_host(struct rl_host *hp);

/* First reachable address. In the GUI, keep retrying for a while if nothing
 * answers: macOS shows its Local Network prompt on an app's first LAN
 * connection and fails that connection while the prompt is up. */
static int pick_addr_patient(const struct rl_host *h)
{
	int wait_s = rl_gui ? plat_permission_wait_s() : 0;
	time_t end = time(NULL) + wait_s;
	int ai = rl_pick_addr(h, PROBE_MS);

	while (ai < 0 && time(NULL) < end) {
		sleep(1);
		ai = rl_pick_addr(h, PROBE_MS);
	}
	return ai;
}

static int cmd_connect(const char *key)
{
	struct rl_host h;
	int r;

	if ((r = rl_host_load(key, &h)) != RL_OK)
		return r;
	return cmd_connect_host(&h);
}

static int cmd_connect_host(struct rl_host *hp)
{
	struct rl_host h = *hp;
	char tried[RL_MAX_ADDRS * (RL_STR + 2)];
	int ai, i;

	ai = pick_addr_patient(&h);
	if (ai < 0) {
		tried[0] = '\0';
		for (i = 0; i < h.naddrs; i++) {
			if (i)
				strcat(tried, ", ");
			strcat(tried, h.addrs[i]);
		}
		rl_err("Can't reach %s on port %d (tried: %s).%s", h.name, h.port, tried,
		       rl_gui ? plat_unreach_hint() : "");
		return RL_EUNREACH;
	}
	rl_host_touch(h.key);
	rl_log("connect %s (%s) via %s:%d", h.name, h.key, h.addrs[ai], h.port);
	return rl_connect(&h, ai);
}

/* "New connection…": ask for the computer, save it to history, connect. */
static int cmd_new(void)
{
	char input[RL_STR], last[RL_STR] = "", host[RL_STR], key[RL_KEY];
	struct rl_host h;
	int port, r;

	rl_gui = 1;
	for (;;) {
		r = plat_ask("Computer to connect to (name or IP address; add :port if not 3389):",
			     last, input, sizeof input);
		if (r > 0)
			return RL_OK;
		if (r < 0) {
			rl_gui = 0;
			rl_err("no dialog program found (macOS: osascript; Linux: zenity or kdialog)");
			return RL_ERR;
		}
		if (rl_parse_target(input, host, sizeof host, &port) == 0)
			break;
		rl_err("'%s' isn't a valid address. Examples: mypc.local, 192.168.1.20, "
		       "192.168.1.20:3390", input);
		rl_copy(last, sizeof last, input);
	}

	rl_make_key(host, port, key, sizeof key);
	if (rl_host_exists(key)) /* already in history: reuse its saved user etc. */
		return cmd_connect(key);

	memset(&h, 0, sizeof h);
	rl_copy(h.key, sizeof h.key, key);
	rl_copy(h.name, sizeof h.name, input);
	rl_copy(h.addrs[0], RL_STR, host);
	h.naddrs = 1;
	h.port = port;
	h.client = RL_CLIENT_FREERDP;
	rl_copy(h.args[0], RL_STR, "/dynamic-resolution");
	h.nargs = 1;
	if (pick_addr_patient(&h) < 0) {
		rl_err("Can't reach %s on port %d. Check the address and that remote "
		       "desktop is on there.%s", host, port, plat_unreach_hint());
		return RL_EUNREACH;
	}
	if (rl_host_save(&h) != RL_OK)
		rl_note("could not save %s to history (connecting anyway)", key);
	return cmd_connect_host(&h);
}

/* Connect using an .rdp file. The computer joins the saved list; if it's
 * already there, its saved user and extra addresses are kept and the entry
 * just remembers this file. */
static int cmd_open(const char *path)
{
	struct rl_host h, old;
	int r;

	if ((r = rl_rdp_import(path, &h)) != RL_OK)
		return r;
	if (rl_host_exists(h.key) && rl_host_load(h.key, &old) == RL_OK) {
		rl_copy(old.rdpfile, sizeof old.rdpfile, h.rdpfile);
		if (h.user[0])
			rl_copy(old.user, sizeof old.user, h.user);
		h = old;
	}
	if (rl_host_save(&h) != RL_OK)
		rl_note("could not save %s to history (connecting anyway)", h.key);
	rl_log("open %s -> %s", path, h.key);
	return cmd_connect_host(&h);
}

static int has_rdp_suffix(const char *s)
{
	size_t len = strlen(s);

	return len > 4 && (!strcmp(s + len - 4, ".rdp") || !strcmp(s + len - 4, ".RDP"));
}

static int cmd_remove(const char *key)
{
	if (!rl_host_exists(key)) {
		rl_err("no saved connection '%s'", key);
		return RL_ECONFIG;
	}
	return rl_host_remove(key);
}

static int confirm_remove(const char *key, char keys[][RL_KEY], char names[][RL_STR], int n)
{
	char msg[RL_STR + 96];
	int i;

	for (i = 0; i < n && strcmp(keys[i], key); i++)
		;
	snprintf(msg, sizeof msg, "Remove \"%s\" and its saved password?",
		 i < n ? names[i] : key);
	if (plat_confirm(msg, "Remove", "Cancel") != 1)
		return RL_OK;
	return rl_host_remove(key);
}

static int pick_remove(char keys[][RL_KEY], char names[][RL_STR], int n)
{
	char chosen[RL_KEY];
	int r;

	r = plat_pick("Remove which saved connection?", "Remove", keys, names, n,
		      chosen, sizeof chosen);
	if (r)
		return r < 0 ? RL_ERR : RL_OK;
	return confirm_remove(chosen, keys, names, n);
}

static int cmd_test(const char *key)
{
	static const char *const what[] = { "answers", "no answer", "name not found" };
	struct rl_host h;
	int i, r, any = 0;

	if ((r = rl_host_load(key, &h)) != RL_OK)
		return r;
	printf("%s (%s), port %d\n", h.name, h.key, h.port);
	for (i = 0; i < h.naddrs; i++) {
		r = rl_probe(h.addrs[i], h.port, PROBE_MS);
		any |= (r == 0);
		printf("  %-32s %s\n", h.addrs[i], what[r]);
	}
	return any ? RL_OK : RL_EUNREACH;
}

static int cmd_list(void)
{
	char keys[RL_MAX_HOSTS][RL_KEY], dir[RL_PATH];
	struct rl_host h;
	int n, i, ai, saved = rl_gui;

	n = rl_host_list(keys, RL_MAX_HOSTS, 0);
	if (n < 0)
		return RL_ECONFIG;
	if (n == 0) {
		rl_hosts_dir(dir, sizeof dir, 0);
		printf("no hosts yet in %s; create one with: rdp-launcher add <name>\n", dir);
		return RL_OK;
	}
	for (i = 0; i < n; i++) {
		rl_gui = 0;
		if (rl_host_load(keys[i], &h) != RL_OK) {
			printf("  %-16s (invalid file, see above)\n", keys[i]);
			continue;
		}
		ai = rl_pick_addr(&h, PROBE_MS);
		if (ai >= 0)
			printf("  %-16s %-24s up    via %s:%d\n", h.key, h.name, h.addrs[ai], h.port);
		else
			printf("  %-16s %-24s down\n", h.key, h.name);
	}
	rl_gui = saved;
	return RL_OK;
}

/* Picker: "New connection…", saved hosts (most recently used first),
 * then "Remove a saved connection…". */
static int cmd_pick(void)
{
	char keys[RL_MAX_HOSTS][RL_KEY], names[RL_MAX_HOSTS][RL_STR];
	char hkeys[RL_MAX_HOSTS][RL_KEY], hnames[RL_MAX_HOSTS][RL_STR];
	char chosen[RL_KEY];
	struct rl_host h;
	int n, m = 0, i, r;

	rl_gui = 1;
	if (!plat_picker()) {
		rl_gui = 0;
		rl_err("no dialog program found (macOS: osascript; Linux: zenity or kdialog)");
		return RL_ERR;
	}
	n = rl_host_list(hkeys, RL_MAX_HOSTS - 2, 1);
	if (n < 0)
		return RL_ECONFIG;
	for (i = 0; i < n; i++) {
		rl_gui = 0; /* a broken file shouldn't pop a dialog per entry */
		if (rl_host_load(hkeys[i], &h) == RL_OK)
			rl_copy(hnames[i], RL_STR, h.name);
		else
			snprintf(hnames[i], RL_STR, "%.60s (invalid file)", hkeys[i]);
		rl_gui = 1;
	}

	/* Preferred: dropdown/list of hosts with Connect / New… / Remove… buttons. */
	r = plat_pick_main(hkeys, hnames, n, chosen, sizeof chosen);
	switch (r) {
	case RL_PICK_CONNECT:
		return cmd_connect(chosen);
	case RL_PICK_NEW:
		return cmd_new();
	case RL_PICK_REMOVE:
		if (chosen[0])
			return confirm_remove(chosen, hkeys, hnames, n);
		return pick_remove(hkeys, hnames, n);
	case RL_PICK_OPEN: {
		char path[RL_PATH];

		r = plat_choose_rdp_file(path, sizeof path);
		return r == 0 ? cmd_open(path) : (r < 0 ? RL_ERR : RL_OK);
	}
	case RL_PICK_CANCEL:
		return RL_OK;
	default:
		break; /* this dialog tool can't: fall back to one combined list */
	}
	if (n == 0)
		return cmd_new();

	rl_copy(keys[m], RL_KEY, PICK_NEW);
	rl_copy(names[m++], RL_STR, "New connection…");
	for (i = 0; i < n; i++, m++) {
		rl_copy(keys[m], RL_KEY, hkeys[i]);
		rl_copy(names[m], RL_STR, hnames[i]);
	}
	rl_copy(keys[m], RL_KEY, PICK_REMOVE);
	rl_copy(names[m++], RL_STR, "Remove a saved connection…");

	r = plat_pick("Connect to:", "Connect", keys, names, m, chosen, sizeof chosen);
	if (r)
		return r < 0 ? RL_ERR : RL_OK; /* error or cancelled */
	if (!strcmp(chosen, PICK_NEW))
		return cmd_new();
	if (!strcmp(chosen, PICK_REMOVE))
		return pick_remove(hkeys, hnames, n);
	return cmd_connect(chosen);
}

static int cmd_add(const char *key)
{
	char path[RL_PATH];
	const char *ed = getenv("VISUAL");
	int r;

	if ((r = rl_host_create(key)) != RL_OK)
		return r;
	if (!isatty(STDIN_FILENO) || rl_host_path(key, path, sizeof path))
		return RL_OK;
	if (!ed || !*ed)
		ed = getenv("EDITOR");
	if (!ed || !*ed)
		ed = "vi";
	{
		/* $EDITOR may carry options ("code -w"), so let the shell split it. */
		char *argv[] = { "/bin/sh", "-c", "$RL_EDITOR \"$1\"", "sh", path, NULL };
		if (setenv("RL_EDITOR", ed, 1) == 0)
			rl_run(argv);
	}
	printf("next: rdp-launcher set-password %s && rdp-launcher test %s\n", key, key);
	return RL_OK;
}

static int cmd_set_password(const char *key)
{
	if (!rl_valid_key(key)) {
		rl_err("'%s' is not a valid host name", key);
		return RL_USAGE;
	}
	if (!plat_secret_tool()) {
		rl_err("no password store tool. %s", plat_secret_hint());
		return RL_ESECRET;
	}
	printf("Storing the RDP password for '%s' in the system password store.\n", key);
	if (plat_secret_set(key)) {
		rl_err("password not stored");
		return RL_ESECRET;
	}
	printf("stored\n");
	return RL_OK;
}

static int cmd_forget_password(const char *key)
{
	int r;

	if (!rl_valid_key(key)) {
		rl_err("'%s' is not a valid host name", key);
		return RL_USAGE;
	}
	r = plat_secret_forget(key);
	if (r < 0) {
		rl_err("no password store tool. %s", plat_secret_hint());
		return RL_ESECRET;
	}
	printf(r == 0 ? "removed\n" : "no stored password for '%s'\n", key);
	return RL_OK;
}

static int cmd_doctor(void)
{
	char path[RL_PATH], keys[RL_MAX_HOSTS][RL_KEY];
	const char *s;
	int n, i, ok = 1;

	printf("rdp-launcher %s (%s)\n", RL_VERSION, plat_name());
	if (rl_hosts_dir(path, sizeof path, 1) == 0) {
		n = rl_host_list(keys, RL_MAX_HOSTS, 0);
		printf("  hosts dir     %s\n  hosts         %d", path, n < 0 ? 0 : n);
		for (i = 0; i < n; i++)
			printf("%s%s", i ? ", " : " (", keys[i]);
		printf("%s\n", n > 0 ? ")" : "");
	} else {
		printf("  hosts dir     UNAVAILABLE (is $HOME set?)\n");
		ok = 0;
	}
	if (rl_find_freerdp(path, sizeof path) == 0) {
		printf("  freerdp       %s\n", path);
	} else {
		printf("  freerdp       NOT FOUND - %s\n", plat_freerdp_hint());
		ok = 0;
	}
	s = plat_secret_tool();
	if (s)
		printf("  passwords     %s\n", s);
	else
		printf("  passwords     NOT FOUND - %s (connect will prompt instead)\n", plat_secret_hint());
	s = plat_picker();
	printf("  picker        %s\n", s ? s : "none (only needed for 'pick')");
	return ok ? RL_OK : RL_ERR;
}

int main(int argc, char **argv)
{
	const char *cmd;

	/* Opened as the macOS app (binary inside *.app/Contents/MacOS/): show the
	 * picker. Old Finder versions add a "-psn_..." argument. */
	if (strstr(argv[0], ".app/Contents/MacOS/") &&
	    (argc == 1 || (argc == 2 && !strncmp(argv[1], "-psn_", 5))))
		return cmd_pick();

	if (argc < 2) {
		usage(stderr);
		return RL_USAGE;
	}
	cmd = argv[1];

	if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
		usage(stdout);
		return RL_OK;
	}
	if (!strcmp(cmd, "-V") || !strcmp(cmd, "--version")) {
		printf("rdp-launcher %s\n", RL_VERSION);
		return RL_OK;
	}
	if (argc == 2) {
		if (!strcmp(cmd, "list"))   return cmd_list();
		if (!strcmp(cmd, "pick"))   return cmd_pick();
		if (!strcmp(cmd, "new"))    return cmd_new();
		if (!strcmp(cmd, "doctor")) return cmd_doctor();
	}
	if (argc == 3) {
		if (!strcmp(cmd, "open"))            return cmd_open(argv[2]);
		if (!strcmp(cmd, "pick")) {          /* from a file manager: pick FILE.rdp */
			rl_gui = 1;
			return cmd_open(argv[2]);
		}
		if (!strcmp(cmd, "connect"))         return cmd_connect(argv[2]);
		if (!strcmp(cmd, "test"))            return cmd_test(argv[2]);
		if (!strcmp(cmd, "add"))             return cmd_add(argv[2]);
		if (!strcmp(cmd, "remove"))          return cmd_remove(argv[2]);
		if (!strcmp(cmd, "set-password"))    return cmd_set_password(argv[2]);
		if (!strcmp(cmd, "forget-password")) return cmd_forget_password(argv[2]);
	}
	if (argc == 2 && has_rdp_suffix(cmd))
		return cmd_open(cmd);
	if (argc == 2 && cmd[0] != '-' &&
	    strcmp(cmd, "connect") && strcmp(cmd, "test") && strcmp(cmd, "add") &&
	    strcmp(cmd, "remove") && strcmp(cmd, "set-password") &&
	    strcmp(cmd, "forget-password"))
		return cmd_connect(cmd);

	usage(stderr);
	return RL_USAGE;
}
