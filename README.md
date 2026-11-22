# RDP Launcher

<img src="desktop/icon/rdp-launcher-256.png" width="96" align="right" alt="RDP Launcher icon">

A small remote desktop launcher for macOS and Linux. Pick a saved computer, or type a new address, and it opens an RDP session with [FreeRDP](https://www.freerdp.com/). It remembers your connections and keeps passwords in the system keychain.

It's a few hundred lines of portable C (C11 + POSIX), linking only against libc. Everything platform-specific is done by calling programs the OS already has.

> **Status:** 0.0.x, in development. Works day to day on macOS (Apple Silicon) and Ubuntu (x86_64).

## Why this exists

I needed remote access to a new Ubuntu desktop, and the ready-made apps I downloaded didn't work with it:

- **Microsoft Windows App** reached GNOME's own remote desktop but stayed on a black screen. It doesn't complete the reconnect step that GNOME's headless Remote Login uses.
- **FreeRDP** worked, but only as a long terminal command that asks for the password every time.

RDP Launcher is the missing piece: FreeRDP's compatibility with a simple app around it.

It was built with the help of **Claude** (Anthropic's AI assistant): design, C code, testing on macOS and Linux, and these docs, working alongside me.

## Features

- **Saved connections.** Recently used ones are listed first. Connect by double-click, the Connect button or Return.
- **New connection.** Type `mypc.local`, `192.168.1.20` or `192.168.1.20:3390`. Username and password go in one box, and you can save the password.
- **`.rdp` files.** Opens standard Remote Desktop files, UTF-8 or Windows' UTF-16. Their screen and display settings are kept.
- **Several addresses per computer.** For example a `.local` name plus an IP, or a VPN name. The first one that answers is used.
- **Passwords never touch the command line.** They're read from the macOS Keychain or the freedesktop Secret Service (GNOME Keyring / KWallet) and passed to FreeRDP through a pipe.
- **Clear errors.** A failed connection shows FreeRDP's actual reason. Closing a session normally shows nothing.
- **Works from a terminal too:** `rdp-launcher mypc`, `rdp-launcher list`, `rdp-launcher open file.rdp`.

## Requirements

| | macOS | Ubuntu / Debian | Fedora |
|---|---|---|---|
| build | Xcode Command Line Tools | `build-essential` | `gcc make` |
| RDP client | `brew install freerdp` | `freerdp3-sdl` | `freerdp` |
| password store | built in (Keychain) | `libsecret-tools` | `libsecret` |
| dialogs | built in | `zenity` | `zenity` (or `kdialog`) |

`rdp-launcher doctor` reports what a machine is missing.

## Install

```bash
make
make check            # self-test, no network or GUI needed
make install          # → ~/.local/bin/rdp-launcher and its man page
make install-desktop  # the clickable app: ~/Applications/RDP Launcher.app on macOS,
                      # an app-grid entry + .rdp file association on Linux
```

`make install PREFIX=/usr/local` installs system-wide. `DESTDIR`, `CC`, `CFLAGS` and `LDFLAGS` are respected. `make uninstall` and `make uninstall-desktop` remove everything.

## Use

Open **RDP Launcher**. The window lists your saved computers, with buttons:

- **Connect:** connects to the selected computer (or double-click it). Only what isn't saved yet is asked for.
- **New…:** opens a new connection by address. It's saved to the list afterwards.
- **Open .rdp…:** opens a Remote Desktop file. On Linux you can also double-click `.rdp` files.
- **Remove…:** deletes the selected connection and its saved password.

The same from a terminal:

```bash
rdp-launcher list                  # saved computers, and which answer right now
rdp-launcher mypc                  # connect
rdp-launcher new                   # the New… dialog
rdp-launcher open work.rdp         # connect using an .rdp file (or: rdp-launcher work.rdp)
rdp-launcher test mypc             # check each address, without logging in
rdp-launcher add mypc              # create a saved computer by hand ($EDITOR)
rdp-launcher set-password mypc     # store / replace the password
rdp-launcher forget-password mypc
rdp-launcher remove mypc
```

See `man rdp-launcher` for details and exit codes.

## Saved computers

Each one is a small text file in `~/.config/rdp-launcher/hosts/<name>.conf` (or under `$XDG_CONFIG_HOME`). The app writes them for you, and they're safe to edit:

```ini
name      = My PC
addresses = mypc.local 192.168.1.20    # tried in order; the first that answers wins
port      = 3389
user      = me
client    = freerdp                    # or windows-app (macOS only)
args      = /dynamic-resolution        # extra FreeRDP options
rdpfile   = /path/to/mypc.rdp          # optional: FreeRDP also loads its settings
```

Logs live in `~/.local/state/rdp-launcher/`: `rdp-launcher.log` (timestamped events, never passwords) and one FreeRDP log per computer.

## Setting up an Ubuntu (GNOME) computer to connect to

Do this once on the computer you want to reach. GNOME's built-in **Remote Login** works headless: no monitor, nobody logged in.

1. Settings → System → Remote Desktop → **Remote Login** → on.
2. `sudo grdctl --system rdp set-credentials`. The toggle alone leaves the login empty, and connections are refused with `Credentials are not set`.
3. Its address is `<hostname>.local` on most home networks.

You'll be asked twice: first for the Remote Login credentials, then for your normal user on the GNOME login screen. Use `client = freerdp` (the default). Microsoft's Windows App doesn't complete GNOME's reconnect step and shows a black screen.

## Platform notes

**macOS**
- The app asks once for **Local Network** permission ("find devices on your local network"). It keeps retrying for 15 s while the prompt is up. The app stays running during a session, because macOS ties the permission to the running app.
- Rebuilding the app may make macOS ask again.
- `.rdp` files open through the app's **Open .rdp…** button. Finder double-click isn't supported, because Finder delivers files through Apple Events, which a plain C program doesn't receive.

**Linux**
- `make install-desktop` registers an `application/x-rdp` type (Ubuntu otherwise treats `.rdp` as text) and makes RDP Launcher its default app.
- Saving passwords needs an unlocked keyring, which a normal desktop session has.

## How it works

`rdp-launcher` finds a FreeRDP binary (`sdl-freerdp3`, `sdl-freerdp`, `xfreerdp3` or `xfreerdp`). It checks the saved addresses with a short non-blocking connect, then starts FreeRDP with every option, including the password, written into a pipe (`/args-from:fd:N`). Nothing sensitive appears in `ps` or the environment. The dialogs are `osascript` (AppleScript, plus JavaScript with the Cocoa bridge for the custom windows) on macOS, and `zenity` or `kdialog` on Linux. All OS-specific code is in `src/platform.c`.

## License

[MIT](LICENSE).

RDP Launcher runs FreeRDP but contains none of its code. FreeRDP is a separate project under the Apache License 2.0. RDP Launcher isn't affiliated with or endorsed by the FreeRDP project.
