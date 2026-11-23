#!/bin/sh
# Smoke test: runs against a throwaway config dir; no network login, no password store.
# usage: sh test/smoke.sh ./rdp-launcher
set -u
BIN=${1:-./rdp-launcher}
T=$(mktemp -d "${TMPDIR:-/tmp}/rl-test.XXXXXX")
trap 'rm -rf "$T"' EXIT INT TERM
export XDG_CONFIG_HOME="$T/config" XDG_STATE_HOME="$T/state"
fail=0

expect() { # expect <exit-code> <description> <command...>
	want=$1 desc=$2; shift 2
	"$@" >"$T/out" 2>&1; got=$?
	if [ "$got" -eq "$want" ]; then echo "ok   $desc"
	else echo "FAIL $desc (exit $got, want $want)"; sed 's/^/     /' "$T/out"; fail=1; fi
}

expect 0 "--version"                 "$BIN" --version
expect 2 "no arguments is usage"     "$BIN"
expect 0 "list with no hosts"        "$BIN" list
expect 3 "unknown host"              "$BIN" test nosuch
expect 2 "bad host name rejected"    "$BIN" add ../evil
expect 0 "add creates a host file"   "$BIN" add demo </dev/null
expect 3 "duplicate add refused"     "$BIN" add demo </dev/null

cat >"$XDG_CONFIG_HOME/rdp-launcher/hosts/closed.conf" <<'EOF'
name      = Closed Port   # inline comment
addresses = 127.0.0.1 localhost
port      = 1
user      = nobody
EOF
expect 4 "closed port is unreachable" "$BIN" test closed
expect 4 "connect to unreachable"     "$BIN" closed </dev/null

cat >"$XDG_CONFIG_HOME/rdp-launcher/hosts/broken.conf" <<'EOF'
addresses = 127.0.0.1
port      = 99999
EOF
expect 3 "port out of range"          "$BIN" test broken

cat >"$XDG_CONFIG_HOME/rdp-launcher/hosts/envok.conf" <<'EOF'
addresses = 127.0.0.1
port      = 1
env       = SDL_RENDER_VSYNC=0 FOO=a=b
EOF
expect 4 "env line accepted"          "$BIN" test envok

cat >"$XDG_CONFIG_HOME/rdp-launcher/hosts/envbad.conf" <<'EOF'
addresses = 127.0.0.1
port      = 1
env       = NOEQUALS
EOF
expect 3 "env without = rejected"     "$BIN" test envbad

cat >"$XDG_CONFIG_HOME/rdp-launcher/hosts/noaddr.conf" <<'EOF'
name = Nothing
EOF
expect 3 "missing addresses"          "$BIN" test noaddr

# A listening local port must show as reachable.
if command -v python3 >/dev/null 2>&1; then
	python3 -c 'import socket,time;s=socket.socket();s.bind(("127.0.0.1",0));s.listen();print(s.getsockname()[1],flush=True);time.sleep(5)' >"$T/port" &
	sleep 1
	port=$(cat "$T/port")
	printf 'addresses = 127.0.0.1\nport = %s\nuser = x\n' "$port" >"$XDG_CONFIG_HOME/rdp-launcher/hosts/open.conf"
	expect 0 "listening port is reachable" "$BIN" test open
	wait
fi

# .rdp files: imported into the saved list even when the host doesn't answer.
H="$XDG_CONFIG_HOME/rdp-launcher/hosts"
printf 'screen mode id:i:1\r\nfull address:s:127.0.0.1:1\r\nusername:s:bob\r\n' >"$T/utf8.rdp"
expect 4 ".rdp (UTF-8) imported, host down" "$BIN" open "$T/utf8.rdp" </dev/null
if grep -q '^user *= bob$' "$H/127.0.0.1-1.conf" 2>/dev/null &&
   grep -q '^rdpfile *= .*utf8.rdp$' "$H/127.0.0.1-1.conf"; then
	echo "ok   .rdp saved user + rdpfile"
else
	echo "FAIL .rdp saved user + rdpfile"; cat "$H/127.0.0.1-1.conf" 2>&1 | sed 's/^/     /'; fail=1
fi
printf 'full address:s:127.0.0.1\r\nserver port:i:2\r\n' >"$T/port.rdp"
expect 4 ".rdp server port line"        "$BIN" "$T/port.rdp" </dev/null
[ -f "$H/127.0.0.1-2.conf" ] && echo "ok   .rdp port 2 -> key 127.0.0.1-2" ||
	{ echo "FAIL .rdp port 2 -> key 127.0.0.1-2"; fail=1; }
if command -v python3 >/dev/null 2>&1; then
	python3 -c 'import sys; open(sys.argv[1],"wb").write(b"\xff\xfe" + "full address:s:127.0.0.1:3\r\nusername:s:win\r\n".encode("utf-16-le"))' "$T/win.rdp"
	expect 4 ".rdp (UTF-16 from Windows)" "$BIN" open "$T/win.rdp" </dev/null
	grep -q '^user *= win$' "$H/127.0.0.1-3.conf" 2>/dev/null && echo "ok   UTF-16 user read" ||
		{ echo "FAIL UTF-16 user read"; fail=1; }
fi
printf 'username:s:nobody\r\n' >"$T/noaddr.rdp"
expect 3 ".rdp without an address"      "$BIN" open "$T/noaddr.rdp"

expect 0 "list shows hosts"           "$BIN" list
expect 0 "remove a saved host"        "$BIN" remove closed
expect 3 "remove it again fails"      "$BIN" remove closed
[ $fail -eq 0 ] && echo "all passed" || echo "SOME TESTS FAILED"
exit $fail
