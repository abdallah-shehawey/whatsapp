# wa-lite

A small WhatsApp Web client for GTK4 + WebKitGTK 6. The binary is around 55 KB.
It loads `web.whatsapp.com`, so it is the same client WhatsApp serves to a
browser — no reverse-engineered protocol, and nothing that puts an account at
risk.

Built because the packaged clients had three problems that could not be fixed
from the outside.

## What it fixes

**Ctrl+V pasted nothing.** WebKitGTK hands the page an empty `clipboardData`
for images — a real Ctrl+V fires `paste` with `types=[] items=[] files=[]`,
measured on WebKitGTK 2.52.5 — so WhatsApp's handler finds nothing and drops it.
WebKit's usual fallback of inserting `<img src="blob:">` does not happen either,
because WhatsApp's composer is plaintext-only, so there is nothing to scrape
back out of the DOM. wa-lite never asks WebKit for the clipboard: it reads the
image on the GTK side, where the data is plainly there, and hands the bytes to
the page. Text paste is left on WebKit's native path, which works fine.

**WhatsApp thought it was talking to Safari on a Mac**, offered a Mac download
and registered the device as "Safari (Mac OS)". WebKitGTK ships site-specific
quirks that rewrite the user agent for a list of hosts including whatsapp.com,
and the quirk beats whatever the application sets — so setting a Chrome user
agent is not enough on its own, `enable-site-specific-quirks` has to go off too.

**Notifications went missing while the window was open.** WhatsApp Web
suppresses them when it believes the window is focused. wa-lite reports the
document as unfocused so they arrive either way, and deliberately leaves
`visibilityState` alone so read receipts and sync keep behaving.

## What it does

- Starts hidden at login and lives in the tray; the page still loads and
  notifications still arrive with no window on screen
- Tray icon switches to an unread badge, driven by WhatsApp's own `(3) WhatsApp`
  document title
- Follows the desktop's dark/light preference and interface font
- Downloads land in `~/Downloads`
- WebKit's in-process memory pressure handler is switched on, so it sheds caches
  and runs GC as it approaches a ceiling. That is very different from capping
  the process with a cgroup, which can only evict pages blindly — a
  `MemoryHigh=1100M` cap on the packaged client drove `memory.pressure` to
  `full avg10=63`, frozen roughly two thirds of the time, while `cpu.pressure`
  stayed at zero.

## Install

From the [shinux repository](https://shehawey.github.io/shinux):

```sh
sudo dnf install wa-lite     # Fedora and friends
sudo apt install wa-lite     # Debian, Ubuntu 24.04+
```

## Build

Needs GTK4 and WebKitGTK 6 development files.

```sh
sudo dnf install gtk4-devel webkitgtk6.0-devel          # Fedora
sudo apt install libgtk-4-dev libwebkitgtk-6.0-dev      # Debian/Ubuntu

make
make install        # ~/.local/bin plus a desktop entry and icons
make autostart      # also start hidden at login
make no-autostart   # undo just the autostart part
```

`make install` honours `DESTDIR` and `PREFIX`, so the tree packages cleanly.

## Keys

| | |
|---|---|
| `Ctrl+V` | paste an image |
| `Ctrl` `+` / `-` / `0` | zoom in, out, reset |
| `Ctrl+Q` | quit for real |
| window close | hides to the tray, stays connected |

## Layout

| | |
|---|---|
| `src/main.c` | window, WebKit setup, clipboard paste, config |
| `src/tray.c` | StatusNotifierItem over raw GDBus — GTK4 has no tray, and libayatana-appindicator is packaged for GTK2/GTK3 only, neither linkable into a GTK4 process |
| `src/inject.js` | page-side helpers, compiled into the binary |
| `tools/make-icons.py` | regenerates `data/icons` — `make icons`, never hand-edit the PNGs |

State lives in `~/.local/share/wa-lite`, config in `~/.config/wa-lite`. The
config file takes `[view] font` and `[window] width`/`height`.

## Licence

GPL-3.0. Icon origins are recorded in `data/icons/NOTICE`.
