/* rdp-launcher: everything that differs between macOS and other Unix.
 *
 * macOS uses built-in tools: security (Keychain), osascript (dialogs), open.
 * Everything else is treated as a freedesktop system (Linux, BSD):
 * secret-tool (Secret Service: GNOME Keyring / KWallet), zenity or kdialog.
 * Nothing is linked: each job runs an existing program, so the binary needs
 * only libc. Text for dialogs is always passed as a separate argv entry, so
 * it never needs quoting for AppleScript or a shell.
 */
#include "rl.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define SERVICE "rdp-launcher"
#define TITLE   "RDP Launcher"

static int have(const char *exe)
{
	char path[RL_PATH];

	return rl_find_exe(exe, plat_exe_dirs(), path, sizeof path) == 0;
}

static void chomp(char *s)
{
	s[strcspn(s, "\r\n")] = '\0';
}

#ifdef __APPLE__ /* ---------------------------------------------- macOS */

#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>

static const char *const exe_dirs[] = {
	"/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin", NULL
};

const char *plat_name(void) { return "macOS"; }
const char *const *plat_exe_dirs(void) { return exe_dirs; }
const char *plat_freerdp_hint(void) { return "Install it with: brew install freerdp"; }
const char *plat_secret_tool(void) { return have("security") ? "security (Keychain)" : NULL; }
const char *plat_secret_hint(void) { return "macOS's 'security' tool should always exist"; }
const char *plat_unreach_hint(void)
{
	return "\n\nIf the same host works from Terminal (rdp-launcher test <host>), macOS is "
	       "blocking this app: System Settings > Privacy & Security > Local Network > "
	       "turn on RDP Launcher.";
}
/* Long enough to read and answer the "find devices on your local network" prompt. */
int plat_permission_wait_s(void) { return 15; }

/* SDL draws FreeRDP's window with Metal and by default hands each frame to the
 * display as soon as it is ready (vsync off). On macOS 15 with a 240 Hz external
 * display that has been followed by kernel panics in Apple's display driver
 * ("mismatched swapID's"). Presenting in step with the display is the mitigation.
 * A value already in the environment, or a host's env line, takes precedence. */
void plat_client_env(void) { setenv("SDL_RENDER_VSYNC", "1", 0); }
const char *plat_picker(void) { return have("osascript") ? "osascript" : NULL; }

int plat_secret_get(const char *key, char *out, size_t n)
{
	char *argv[] = { "security", "find-generic-password", "-s", SERVICE,
			 "-a", (char *)key, "-w", NULL };
	int r = rl_run_capture(argv, out, n);

	if (r == 127 || r < 0)
		return -1;
	if (r != 0) {
		rl_wipe(out, n);
		return 1;
	}
	chomp(out);
	return 0;
}

/* "-w" last makes security prompt twice on the tty itself, no echo. */
int plat_secret_set(const char *key)
{
	char label[RL_STR];
	char *argv[] = { "security", "add-generic-password", "-U", "-s", SERVICE,
			 "-a", (char *)key, "-l", label, "-w", NULL };

	snprintf(label, sizeof label, "rdp-launcher: %s", key);
	return rl_run(argv) == 0 ? 0 : -1;
}

/* The command goes to `security -i` on stdin, password as hex (-X):
 * never in argv, and no quoting problems whatever the password holds. */
int plat_secret_put(const char *key, const char *pw)
{
	static const char hexd[] = "0123456789abcdef";
	char cmd[2 * 512 + RL_KEY * 2 + 128], check[512];
	char *argv[] = { "security", "-i", NULL };
	size_t i, len = strlen(pw);
	int off, ok;

	if (len >= 512)
		return -1;
	off = snprintf(cmd, sizeof cmd,
		       "add-generic-password -U -s " SERVICE " -a %s -l \"rdp-launcher: %s\" -X ",
		       key, key);
	for (i = 0; i < len; i++) {
		cmd[off++] = hexd[(unsigned char)pw[i] >> 4];
		cmd[off++] = hexd[(unsigned char)pw[i] & 15];
	}
	cmd[off++] = '\n';
	rl_run_input(argv, cmd, (size_t)off);
	rl_wipe(cmd, sizeof cmd);
	/* security -i exits 0 even when a command fails: read it back. */
	ok = plat_secret_get(key, check, sizeof check) == 0 && strcmp(check, pw) == 0;
	rl_wipe(check, sizeof check);
	return ok ? 0 : -1;
}

int plat_secret_forget(const char *key)
{
	char out[64];
	char *argv[] = { "security", "delete-generic-password", "-s", SERVICE,
			 "-a", (char *)key, NULL };
	int r = rl_run_capture(argv, out, sizeof out);

	return r == 0 ? 0 : (r == 127 || r < 0) ? -1 : 1;
}

/* Run argv, keep output minus one trailing newline: 0 ok, 1 failed, -1 missing. */
static int osa_like_capture(char *const argv[], char *out, size_t n)
{
	size_t len;
	int r = rl_run_capture(argv, out, n);

	if (r == 127 || r < 0)
		return -1;
	len = strlen(out);
	if (len > 0 && out[len - 1] == '\n')
		out[len - 1] = '\0';
	return r == 0 ? 0 : 1;
}

/* Run an AppleScript `on run argv` handler; capture its result. */
static int osa(const char *body, char *const args[], int nargs, char *out, size_t n)
{
	char *argv[RL_MAX_HOSTS + 8];
	char dummy[64];
	int a = 0, i, r;

	argv[a++] = "osascript";
	argv[a++] = "-e";
	argv[a++] = (char *)body;
	for (i = 0; i < nargs && a < (int)(sizeof argv / sizeof argv[0]) - 1; i++)
		argv[a++] = args[i];
	argv[a] = NULL;
	if (!out) {
		out = dummy;
		n = sizeof dummy;
	}
	r = rl_run_capture(argv, out, n);
	if (r == 127 || r < 0)
		return -1;
	/* Drop only the trailing newline osascript adds; results may be multi-line. */
	i = (int)strlen(out);
	if (i > 0 && out[i - 1] == '\n')
		out[i - 1] = '\0';
	return r == 0 ? 0 : 1; /* nonzero: user cancelled (error -128) */
}

int plat_pick(const char *prompt, const char *ok_label, char keys[][RL_KEY],
	      char names[][RL_STR], int n, char *chosen, size_t csz)
{
	static const char body[] =
		"on run argv\n"
		"  activate\n"
		"  set r to choose from list (items 3 thru -1 of argv) with title \"" TITLE "\""
		" with prompt (item 1 of argv) OK button name (item 2 of argv)\n"
		"  if r is false then error number -128\n"
		"  return item 1 of r\n"
		"end run";
	char labels[RL_MAX_HOSTS][RL_STR + RL_KEY + 4];
	char *args[RL_MAX_HOSTS + 2];
	char out[RL_STR + RL_KEY + 8];
	int i, r;

	if (n > RL_MAX_HOSTS)
		n = RL_MAX_HOSTS;
	args[0] = (char *)prompt;
	args[1] = (char *)ok_label;
	for (i = 0; i < n; i++) {
		/* Show the key only when it adds information. */
		if (!rl_valid_key(keys[i]) || !strcmp(names[i], keys[i]))
			snprintf(labels[i], sizeof labels[i], "%s", names[i]);
		else
			snprintf(labels[i], sizeof labels[i], "%s  (%s)", names[i], keys[i]);
		args[i + 2] = labels[i];
	}
	r = osa(body, args, n + 2, out, sizeof out);
	if (r)
		return r;
	for (i = 0; i < n; i++)
		if (!strcmp(out, labels[i]))
			return rl_copy(chosen, csz, keys[i]) ? -1 : 0;
	return 1;
}

int plat_ask(const char *prompt, const char *def, char *out, size_t n)
{
	static const char body[] =
		"on run argv\n"
		"  activate\n"
		"  set d to display dialog (item 1 of argv) default answer (item 2 of argv)"
		" with title \"" TITLE "\" buttons {\"Cancel\", \"OK\"}"
		" default button \"OK\" cancel button \"Cancel\"\n"
		"  return text returned of d\n"
		"end run";
	char *args[] = { (char *)prompt, (char *)def };
	int r = osa(body, args, 2, out, n);

	return (r == 0 && !out[0]) ? 1 : r;
}

/* Returns "<button>\n<password>" from one dialog with hidden input. */
int plat_ask_password(const char *prompt, int offer_save, int *save, char *out, size_t n)
{
	static const char body_save[] =
		"on run argv\n"
		"  activate\n"
		"  set d to display dialog (item 1 of argv) default answer \"\" with hidden answer"
		" with title \"" TITLE "\" buttons {\"Cancel\", \"Save & Connect\", \"Connect\"}"
		" default button \"Connect\" cancel button \"Cancel\"\n"
		"  return (button returned of d) & linefeed & (text returned of d)\n"
		"end run";
	static const char body_plain[] =
		"on run argv\n"
		"  activate\n"
		"  set d to display dialog (item 1 of argv) default answer \"\" with hidden answer"
		" with title \"" TITLE "\" buttons {\"Cancel\", \"Connect\"}"
		" default button \"Connect\" cancel button \"Cancel\"\n"
		"  return (button returned of d) & linefeed & (text returned of d)\n"
		"end run";
	char buf[640], *nl;
	char *args[] = { (char *)prompt };
	int r;

	*save = 0;
	r = osa(offer_save ? body_save : body_plain, args, 1, buf, sizeof buf);
	nl = strchr(buf, '\n');
	if (r || !nl || !nl[1]) {        /* cancelled, or empty password */
		rl_wipe(buf, sizeof buf);
		return r ? r : 1;
	}
	*nl = '\0';
	*save = !strcmp(buf, "Save & Connect");
	r = rl_copy(out, n, nl + 1) ? -1 : 0;
	rl_wipe(buf, sizeof buf);
	return r;
}

/* NSOpenPanel limited to .rdp files (osascript JavaScript + Cocoa bridge). */
int plat_choose_rdp_file(char *out, size_t n)
{
	static const char js[] =
		"ObjC.import('Cocoa');\n"
		"function run() {\n"
		"  var app = $.NSApplication.sharedApplication;\n"
		"  app.setActivationPolicy($.NSApplicationActivationPolicyAccessory);\n"
		"  app.activateIgnoringOtherApps(true);\n"
		"  var p = $.NSOpenPanel.openPanel;\n"
		"  p.title = 'Open an .rdp file';\n"
		"  p.allowedFileTypes = $(['rdp', 'RDP']);\n"
		"  p.canChooseDirectories = false;\n"
		"  p.allowsMultipleSelection = false;\n"
		"  if (p.runModal != $.NSModalResponseOK) return '';\n"
		"  return p.URL.path.js;\n"
		"}\n";
	char *argv[] = { "osascript", "-l", "JavaScript", "-e", (char *)js, NULL };
	int r = osa_like_capture(argv, out, n);

	return r ? r : (out[0] ? 0 : 1);
}

/* Small native window: list of saved hosts (double-click connects) +
 * Remove… / New… / Cancel / Connect. A stock NSAlert stacks four buttons
 * vertically, so this builds the window itself (osascript JavaScript + Cocoa
 * bridge, nothing to install).
 * Prints "connect\n<row>", "remove\n<row>", "new", "open" or "cancel". */
int plat_pick_main(char keys[][RL_KEY], char names[][RL_STR], int n, char *chosen, size_t csz)
{
	static const char js[] =
		"ObjC.import('Cocoa');\n"
		"var picked = 'cancel', rows = [];\n"
		"ObjC.registerSubclass({ name: 'RLPickHandler', methods: {\n"
		"  'click:': { types: ['void', ['id']], implementation: function (s) {\n"
		"    picked = ['connect', 'new', 'remove', 'cancel', 'open'][s.tag];\n"
		"    $.NSApp.stopModal; } },\n"
		"  'dbl:': { types: ['void', ['id']], implementation: function (t) {\n"
		"    if (t.clickedRow >= 0) { picked = 'connect'; $.NSApp.stopModal; } } },\n"
		"  'numberOfRowsInTableView:': { types: ['long', ['id']],\n"
		"    implementation: function (t) { return rows.length; } },\n"
		"  'tableView:objectValueForTableColumn:row:': { types: ['id', ['id', 'id', 'long']],\n"
		"    implementation: function (t, c, r) { return $(rows[r]); } } } });\n"
		"function button(win, h, title, tag, x, w, key) {\n"
		"  var b = $.NSButton.alloc.initWithFrame($.NSMakeRect(x, 14, w, 32));\n"
		"  b.title = title; b.bezelStyle = $.NSBezelStyleRounded; b.tag = tag;\n"
		"  b.target = h; b.action = 'click:';\n"
		"  if (key) b.keyEquivalent = key;\n"
		"  win.contentView.addSubview(b); return b;\n"
		"}\n"
		"function run(argv) {\n"
		"  for (var i = 0; i < argv.length; i++) rows.push(argv[i]);\n"
		"  var app = $.NSApplication.sharedApplication;\n"
		"  app.setActivationPolicy($.NSApplicationActivationPolicyAccessory);\n"
		"  var win = $.NSPanel.alloc.initWithContentRectStyleMaskBackingDefer(\n"
		"    $.NSMakeRect(0, 0, 544, 280), $.NSWindowStyleMaskTitled,\n"
		"    $.NSBackingStoreBuffered, false);\n"
		"  win.title = '" TITLE "';\n"
		"  var label = $.NSTextField.labelWithString('Connect to:');\n"
		"  label.frame = $.NSMakeRect(20, 248, 504, 20);\n"
		"  win.contentView.addSubview(label);\n"
		"  var h = $.RLPickHandler.alloc.init;\n"
		"  var sv = $.NSScrollView.alloc.initWithFrame($.NSMakeRect(20, 60, 504, 180));\n"
		"  sv.hasVerticalScroller = true; sv.borderType = $.NSBezelBorder;\n"
		"  var tbl = $.NSTableView.alloc.initWithFrame($.NSMakeRect(0, 0, 504, 180));\n"
		"  var col = $.NSTableColumn.alloc.initWithIdentifier('host');\n"
		"  col.width = 484; tbl.addTableColumn(col);\n"
		"  tbl.headerView = $(); tbl.rowHeight = 22;\n"
		"  tbl.usesAlternatingRowBackgroundColors = true;\n"
		"  tbl.allowsEmptySelection = false;\n"
		"  tbl.dataSource = h; tbl.target = h; tbl.doubleAction = 'dbl:';\n"
		"  sv.documentView = tbl;\n"
		"  win.contentView.addSubview(sv);\n"
		"  tbl.reloadData;\n"
		"  tbl.selectRowIndexesByExtendingSelection($.NSIndexSet.indexSetWithIndex(0), false);\n"
		"  button(win, h, 'Remove\\u2026', 2, 14, 100, '');\n"
		"  button(win, h, 'New\\u2026', 1, 116, 90, '');\n"
		"  button(win, h, 'Open .rdp\\u2026', 4, 208, 116, '');\n"
		"  button(win, h, 'Cancel', 3, 338, 90, '\\u001b');\n"
		"  button(win, h, 'Connect', 0, 430, 100, '\\r');\n"
		"  win.center;\n"
		"  win.initialFirstResponder = tbl;\n"
		"  app.activateIgnoringOtherApps(true);\n"
		"  win.makeKeyAndOrderFront(null);\n"
		"  app.runModalForWindow(win);\n"
		"  var idx = tbl.selectedRow;\n"
		"  win.orderOut(null);\n"
		"  return (picked === 'connect' || picked === 'remove') ? picked + '\\n' + idx : picked;\n"
		"}\n";
	char labels[RL_MAX_HOSTS][RL_STR + RL_KEY + 4];
	char *argv[RL_MAX_HOSTS + 8], out[64], *nl;
	int a = 0, i, r;
	long idx;

	if (n > RL_MAX_HOSTS)
		n = RL_MAX_HOSTS;
	argv[a++] = "osascript";
	argv[a++] = "-l";
	argv[a++] = "JavaScript";
	argv[a++] = "-e";
	argv[a++] = (char *)js;
	for (i = 0; i < n; i++) {
		if (!strcmp(names[i], keys[i]))
			snprintf(labels[i], sizeof labels[i], "%s", names[i]);
		else
			snprintf(labels[i], sizeof labels[i], "%s  (%s)", names[i], keys[i]);
		argv[a++] = labels[i];
	}
	argv[a] = NULL;

	chosen[0] = '\0';
	r = rl_run_capture(argv, out, sizeof out);
	if (r == 127 || r < 0)
		return RL_PICK_UNSUPPORTED;
	if (r != 0)
		return RL_PICK_UNSUPPORTED; /* script failed: fall back to the list */
	out[strcspn(out, "\r")] = '\0';
	nl = strchr(out, '\n');
	if (nl)
		*nl++ = '\0';
	if (!strcmp(out, "new"))
		return RL_PICK_NEW;
	if (!strcmp(out, "open"))
		return RL_PICK_OPEN;
	if (strcmp(out, "connect") && strcmp(out, "remove"))
		return RL_PICK_CANCEL;
	idx = nl ? strtol(nl, NULL, 10) : -1;
	if (idx < 0 || idx >= n)
		return RL_PICK_CANCEL;
	rl_copy(chosen, csz, keys[idx]);
	return !strcmp(out, "connect") ? RL_PICK_CONNECT : RL_PICK_REMOVE;
}

/* Split "<button>\n<user>\n<password>"; 0 ok, 1 cancelled/empty, -1 too long. */
static int split_login(char *buf, int *save, char *user, size_t usz, char *pw, size_t psz)
{
	char *u, *p;

	u = strchr(buf, '\n');
	p = u ? strchr(u + 1, '\n') : NULL;
	if (!p)
		return 1;
	*u++ = '\0';
	*p++ = '\0';
	if (!strcmp(buf, "Cancel") || !*u || !*p)
		return 1;
	*save = !strcmp(buf, "Save & Connect");
	if (rl_copy(user, usz, u) || rl_copy(pw, psz, p))
		return -1;
	return 0;
}

/* One native alert with a username field and a secure password field.
 * AppleScript's dialog has only one field, so this uses osascript's
 * JavaScript mode with the Cocoa bridge (still nothing to install). */
int plat_ask_login(const char *prompt, int offer_save, int *save,
		   char *user, size_t usz, char *pw, size_t psz)
{
	static const char js[] =
		"ObjC.import('Cocoa');\n"
		"function run(argv) {\n"
		"  var app = $.NSApplication.sharedApplication;\n"
		"  app.setActivationPolicy($.NSApplicationActivationPolicyAccessory);\n"
		"  app.activateIgnoringOtherApps(true);\n"
		"  var a = $.NSAlert.alloc.init;\n"
		"  a.messageText = '" TITLE "';\n"
		"  a.informativeText = argv[0];\n"
		"  a.addButtonWithTitle('Connect');\n"
		"  if (argv[1] === '1') a.addButtonWithTitle('Save & Connect');\n"
		"  var c = a.addButtonWithTitle('Cancel');\n"
		"  c.keyEquivalent = '\\u001b';\n"
		"  var v = $.NSView.alloc.initWithFrame($.NSMakeRect(0, 0, 260, 56));\n"
		"  var u = $.NSTextField.alloc.initWithFrame($.NSMakeRect(0, 32, 260, 24));\n"
		"  u.placeholderString = 'Username';\n"
		"  var p = $.NSSecureTextField.alloc.initWithFrame($.NSMakeRect(0, 0, 260, 24));\n"
		"  p.placeholderString = 'Password';\n"
		"  v.addSubview(u); v.addSubview(p);\n"
		"  u.nextKeyView = p; p.nextKeyView = u;\n"
		"  a.accessoryView = v;\n"
		"  a.layout;\n"
		"  a.window.initialFirstResponder = u;\n"
		"  var r = a.runModal;\n"
		"  var btn = r == 1000 ? 'Connect' : (r == 1001 && argv[1] === '1') ? 'Save & Connect' : 'Cancel';\n"
		"  return btn + '\\n' + u.stringValue.js + '\\n' + p.stringValue.js;\n"
		"}\n";
	char buf[1024];
	char *argv[] = { "osascript", "-l", "JavaScript", "-e", (char *)js,
			 (char *)prompt, offer_save ? "1" : "0", NULL };
	int r, len;

	*save = 0;
	r = rl_run_capture(argv, buf, sizeof buf);
	if (r == 127 || r < 0) {
		rl_wipe(buf, sizeof buf);
		return -1;
	}
	len = (int)strlen(buf);
	if (len > 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';
	r = r == 0 ? split_login(buf, save, user, usz, pw, psz) : -1;
	rl_wipe(buf, sizeof buf);
	return r;
}

int plat_confirm(const char *msg, const char *yes, const char *no)
{
	static const char body[] =
		"on run argv\n"
		"  activate\n"
		"  set d to display dialog (item 1 of argv) with title \"" TITLE "\""
		" buttons {(item 3 of argv), (item 2 of argv)} default button (item 2 of argv)"
		" with icon caution\n"
		"  return button returned of d\n"
		"end run";
	char out[RL_STR];
	char *args[] = { (char *)msg, (char *)yes, (char *)no };
	int r = osa(body, args, 3, out, sizeof out);

	if (r < 0)
		return -1;
	return (r == 0 && !strcmp(out, yes)) ? 1 : 0;
}

void plat_dialog_error(const char *msg)
{
	static const char body[] =
		"on run argv\n"
		"  activate\n"
		"  display dialog (item 1 of argv) with title \"" TITLE "\""
		" buttons {\"OK\"} default button 1 with icon caution\n"
		"end run";
	char *args[] = { (char *)msg };

	osa(body, args, 1, NULL, 0);
}

/* Windows App: hand it a .rdp file. It keeps its own saved passwords. */
int plat_open_windows_app(const struct rl_host *h, int ai)
{
	const char *tmp = getenv("TMPDIR");
	char path[RL_PATH];
	char *argv[] = { "open", path, NULL };
	FILE *f;
	int fd;

	snprintf(path, sizeof path, "%s/rdp-launcher-%s.rdp",
		 (tmp && *tmp) ? tmp : "/tmp", h->key);
	unlink(path);
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	f = fd >= 0 ? fdopen(fd, "w") : NULL;
	if (!f) {
		rl_err("cannot write %s: %s", path, strerror(errno));
		return RL_ERR;
	}
	fprintf(f, "full address:s:%s:%d\r\n", h->addrs[ai], h->port);
	if (h->user[0])
		fprintf(f, "username:s:%s\r\n", h->user);
	fprintf(f, "prompt for credentials:i:0\r\n");
	fclose(f);
	if (rl_run(argv) != 0) {
		rl_err("could not open %s (is Windows App installed?)", path);
		return RL_ENOCLIENT;
	}
	return RL_OK;
}

#else /* ------------------------------------------- Linux, BSD, other */

static const char *const exe_dirs[] = {
	"/usr/local/bin", "/usr/bin", "/bin", "/snap/bin", NULL
};

const char *plat_name(void) { return "freedesktop Unix"; }
const char *const *plat_exe_dirs(void) { return exe_dirs; }
const char *plat_freerdp_hint(void)
{
	return "Install it, e.g. Ubuntu/Debian: sudo apt install freerdp3-sdl; "
	       "Fedora: sudo dnf install freerdp";
}
const char *plat_secret_tool(void) { return have("secret-tool") ? "secret-tool" : NULL; }
const char *plat_secret_hint(void)
{
	return "Install secret-tool, e.g. Ubuntu/Debian: sudo apt install libsecret-tools; "
	       "Fedora: sudo dnf install libsecret";
}
const char *plat_unreach_hint(void) { return ""; }
int plat_permission_wait_s(void) { return 0; }
void plat_client_env(void) { }
const char *plat_picker(void)
{
	if (have("zenity"))
		return "zenity";
	if (have("kdialog"))
		return "kdialog";
	return NULL;
}

static int is_zenity(void)
{
	const char *p = plat_picker();

	return p && !strcmp(p, "zenity");
}

int plat_secret_get(const char *key, char *out, size_t n)
{
	char *argv[] = { "secret-tool", "lookup", "service", SERVICE, "host", (char *)key, NULL };
	int r;

	if (!have("secret-tool"))
		return -1;
	r = rl_run_capture(argv, out, n);
	if (r != 0 || !out[0]) {
		rl_wipe(out, n);
		return 1;
	}
	chomp(out);
	return 0;
}

/* secret-tool reads the password from the tty itself (no echo). */
int plat_secret_set(const char *key)
{
	char label[RL_STR];
	char *argv[] = { "secret-tool", "store", label, "service", SERVICE,
			 "host", (char *)key, NULL };

	if (!have("secret-tool"))
		return -1;
	snprintf(label, sizeof label, "--label=rdp-launcher: %s", key);
	return rl_run(argv) == 0 ? 0 : -1;
}

/* Without a tty, secret-tool reads the password from stdin until EOF. */
int plat_secret_put(const char *key, const char *pw)
{
	char label[RL_STR];
	char *argv[] = { "secret-tool", "store", label, "service", SERVICE,
			 "host", (char *)key, NULL };

	if (!have("secret-tool"))
		return -1;
	snprintf(label, sizeof label, "--label=rdp-launcher: %s", key);
	return rl_run_input(argv, pw, strlen(pw)) == 0 ? 0 : -1;
}

int plat_secret_forget(const char *key)
{
	char out[64];
	char *argv[] = { "secret-tool", "clear", "service", SERVICE, "host", (char *)key, NULL };

	if (!have("secret-tool"))
		return -1;
	return rl_run_capture(argv, out, sizeof out) == 0 ? 0 : 1;
}

/* Run a dialog program; 0 ok (output captured), 1 cancelled, -1 missing. */
static int dlg(char *const argv[], char *out, size_t n)
{
	char dummy[64];
	int r;

	if (!out) {
		out = dummy;
		n = sizeof dummy;
	}
	r = rl_run_capture(argv, out, n);
	if (r == 127 || r < 0)
		return -1;
	chomp(out);
	return r == 0 ? 0 : 1;
}

int plat_pick(const char *prompt, const char *ok_label, char keys[][RL_KEY],
	      char names[][RL_STR], int n, char *chosen, size_t csz)
{
	char *argv[2 * RL_MAX_HOSTS + 16];
	char text[RL_STR], ok[RL_STR], out[RL_KEY + 8];
	int i, a = 0, r;

	if (!plat_picker())
		return -1;
	if (n > RL_MAX_HOSTS)
		n = RL_MAX_HOSTS;
	if (is_zenity()) {
		snprintf(text, sizeof text, "--text=%s", prompt);
		snprintf(ok, sizeof ok, "--ok-label=%s", ok_label);
		argv[a++] = "zenity";
		argv[a++] = "--list";
		argv[a++] = "--title=" TITLE;
		argv[a++] = text;
		argv[a++] = ok;
		argv[a++] = "--column=Key";
		argv[a++] = "--column=Connection";
		argv[a++] = "--hide-column=1";
		argv[a++] = "--print-column=1";
	} else {
		argv[a++] = "kdialog";
		argv[a++] = "--title";
		argv[a++] = TITLE;
		argv[a++] = "--menu";
		argv[a++] = (char *)prompt;
	}
	for (i = 0; i < n; i++) {
		argv[a++] = keys[i];
		argv[a++] = names[i];
	}
	argv[a] = NULL;

	r = dlg(argv, out, sizeof out);
	if (r)
		return r;
	for (i = 0; i < n; i++)
		if (!strcmp(out, keys[i]))
			return rl_copy(chosen, csz, keys[i]) ? -1 : 0;
	return 1;
}

int plat_ask(const char *prompt, const char *def, char *out, size_t n)
{
	char text[RL_STR], entry[RL_STR];
	int r;

	if (!plat_picker())
		return -1;
	if (is_zenity()) {
		snprintf(text, sizeof text, "--text=%s", prompt);
		snprintf(entry, sizeof entry, "--entry-text=%s", def);
		{
			char *argv[] = { "zenity", "--entry", "--title=" TITLE, text, entry, NULL };
			r = dlg(argv, out, n);
		}
	} else {
		char *argv[] = { "kdialog", "--title", TITLE, "--inputbox",
				 (char *)prompt, (char *)def, NULL };
		r = dlg(argv, out, n);
	}
	return (r == 0 && !out[0]) ? 1 : r;
}

int plat_ask_password(const char *prompt, int offer_save, int *save, char *out, size_t n)
{
	char text[RL_STR];
	int r;

	*save = 0;
	if (!plat_picker())
		return -1;
	if (is_zenity()) {
		snprintf(text, sizeof text, "--text=%s", prompt);
		{
			/* zenity --password ignores --text; --entry --hide-text shows it. */
			char *argv[] = { "zenity", "--entry", "--hide-text", "--title=" TITLE,
					 text, NULL };
			r = dlg(argv, out, n);
		}
	} else {
		char *argv[] = { "kdialog", "--title", TITLE, "--password", (char *)prompt, NULL };
		r = dlg(argv, out, n);
	}
	if (r) {
		rl_wipe(out, n);
		return r;
	}
	if (offer_save)
		*save = plat_confirm("Save this password in the keyring for next time?",
				     "Save", "Don't Save") == 1;
	return 0;
}

int plat_choose_rdp_file(char *out, size_t n)
{
	int r;

	if (!plat_picker())
		return -1;
	if (is_zenity()) {
		char *argv[] = { "zenity", "--file-selection", "--title=Open an .rdp file",
				 "--file-filter=RDP files | *.rdp *.RDP",
				 "--file-filter=All files | *", NULL };
		r = dlg(argv, out, n);
	} else {
		char *argv[] = { "kdialog", "--title", "Open an .rdp file",
				 "--getopenfilename", ".", "*.rdp *.RDP", NULL };
		r = dlg(argv, out, n);
	}
	return r ? r : (out[0] ? 0 : 1);
}

/* zenity list of hosts with Connect (OK) plus New… / Remove… extra buttons.
 * zenity prints only an extra button's label, not the selected row, so
 * REMOVE comes back with no host and the caller asks which one. kdialog
 * has no extra buttons: unsupported, the caller uses the plain list. */
int plat_pick_main(char keys[][RL_KEY], char names[][RL_STR], int n, char *chosen, size_t csz)
{
	char *argv[2 * RL_MAX_HOSTS + 16], out[RL_KEY + 16];
	int a = 0, i, r;

	chosen[0] = '\0';
	if (!is_zenity())
		return RL_PICK_UNSUPPORTED;
	if (n > RL_MAX_HOSTS)
		n = RL_MAX_HOSTS;
	argv[a++] = "zenity";
	argv[a++] = "--list";
	argv[a++] = "--title=" TITLE;
	argv[a++] = "--text=Connect to:";
	argv[a++] = "--ok-label=Connect";
	argv[a++] = "--extra-button=New…";
	argv[a++] = "--extra-button=Remove…";
	argv[a++] = "--extra-button=Open .rdp…";
	argv[a++] = "--column=Key";
	argv[a++] = "--column=Computer";
	argv[a++] = "--hide-column=1";
	argv[a++] = "--print-column=1";
	for (i = 0; i < n; i++) {
		argv[a++] = keys[i];
		argv[a++] = names[i];
	}
	argv[a] = NULL;

	r = rl_run_capture(argv, out, sizeof out);
	if (r == 127 || r < 0)
		return RL_PICK_UNSUPPORTED;
	chomp(out);
	if (!strcmp(out, "New…"))
		return RL_PICK_NEW;
	if (!strcmp(out, "Remove…"))
		return RL_PICK_REMOVE;
	if (!strcmp(out, "Open .rdp…"))
		return RL_PICK_OPEN;
	if (r != 0 || !out[0])
		return RL_PICK_CANCEL;
	for (i = 0; i < n; i++)
		if (!strcmp(out, keys[i]))
			return rl_copy(chosen, csz, keys[i]) ? RL_PICK_CANCEL : RL_PICK_CONNECT;
	return RL_PICK_CANCEL;
}

/* zenity --forms: both fields (and the save choice) in one box. kdialog has
 * no multi-field form, so it reports -1 and the caller asks one at a time. */
int plat_ask_login(const char *prompt, int offer_save, int *save,
		   char *user, size_t usz, char *pw, size_t psz)
{
	char text[RL_STR], buf[1024], *u, *p, *s;
	char *argv[12];
	int a = 0, r;

	*save = 0;
	if (!is_zenity())
		return -1;
	snprintf(text, sizeof text, "--text=%s", prompt);
	argv[a++] = "zenity";
	argv[a++] = "--forms";
	argv[a++] = "--title=" TITLE;
	argv[a++] = text;
	argv[a++] = "--add-entry=Username";
	argv[a++] = "--add-password=Password";
	if (offer_save) {
		argv[a++] = "--add-combo=Save password";
		argv[a++] = "--combo-values=No|Yes";
	}
	argv[a++] = "--separator=\n";
	argv[a] = NULL;

	/* Output is "user\npassword[\nYes|No]\n": capture it whole (dlg() would
	 * cut at the first newline). */
	r = rl_run_capture(argv, buf, sizeof buf);
	if (r == 127 || r < 0) {
		rl_wipe(buf, sizeof buf);
		return -1;
	}
	if (r != 0) {
		rl_wipe(buf, sizeof buf);
		return 1;
	}
	u = buf;
	p = strchr(u, '\n');
	r = 1;
	if (p) {
		*p++ = '\0';
		s = strchr(p, '\n');
		if (s)
			*s++ = '\0';
		if (*u && *p) {
			*save = s && !strncmp(s, "Yes", 3);
			r = (rl_copy(user, usz, u) || rl_copy(pw, psz, p)) ? -1 : 0;
		}
	}
	rl_wipe(buf, sizeof buf);
	return r;
}

int plat_confirm(const char *msg, const char *yes, const char *no)
{
	char ok[RL_STR], cancel[RL_STR];

	if (!plat_picker())
		return -1;
	if (is_zenity()) {
		snprintf(ok, sizeof ok, "--ok-label=%s", yes);
		snprintf(cancel, sizeof cancel, "--cancel-label=%s", no);
		{
			char *argv[] = { "zenity", "--question", "--no-markup", "--title=" TITLE,
					 ok, cancel, "--text", (char *)msg, NULL };
			return dlg(argv, NULL, 0) == 0;
		}
	} else {
		char *argv[] = { "kdialog", "--title", TITLE, "--yes-label", (char *)yes,
				 "--no-label", (char *)no, "--yesno", (char *)msg, NULL };
		return dlg(argv, NULL, 0) == 0;
	}
}

void plat_dialog_error(const char *msg)
{
	if (!plat_picker())
		return;
	if (is_zenity()) {
		char *argv[] = { "zenity", "--error", "--no-markup", "--title=" TITLE,
				 "--text", (char *)msg, NULL };
		dlg(argv, NULL, 0);
	} else {
		char *argv[] = { "kdialog", "--title", TITLE, "--error", (char *)msg, NULL };
		dlg(argv, NULL, 0);
	}
}

int plat_open_windows_app(const struct rl_host *h, int ai)
{
	(void)ai;
	rl_err("%s uses client = windows-app, which exists only on macOS; "
	       "use client = freerdp here", h->key);
	return RL_ENOCLIENT;
}

#endif
