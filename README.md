# whatsapp

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
back out of the DOM. whatsapp never asks WebKit for the clipboard: it reads the
image on the GTK side, where the data is plainly there, and hands the bytes to
the page. Text paste is left on WebKit's native path, which works fine.

**WhatsApp thought it was talking to Safari on a Mac**, offered a Mac download
and registered the device as "Safari (Mac OS)". WebKitGTK ships site-specific
quirks that rewrite the user agent for a list of hosts including whatsapp.com,
and the quirk beats whatever the application sets — so setting a Chrome user
agent is not enough on its own, `enable-site-specific-quirks` has to go off too.

**Notifications went missing while the window was open.** WhatsApp Web
suppresses them when it believes the window is focused, so the client raises its
own — one per message, driven by a watch on the chat list rather than by the
document title. The title only counts unread *chats*: a second and a third
message from the same person leave `(1) WhatsApp` exactly as it was, and every
one of them used to arrive in silence behind the first one's banner.

The page is told the truth about focus, pushed in from GTK, because WebKit gets
it wrong: a view in a window hidden in the tray still reports itself focused.
Pinning the answer to `false` was tried and is a trap — it does produce
notifications, and it also convinces WhatsApp nobody is looking, so chats the
user reads never get their receipts.

## What it does

- Starts hidden at login and lives in the tray; the page still loads and
  notifications still arrive with no window on screen
- Tray icon marks itself unread, driven by WhatsApp's own `(3) WhatsApp`
  document title. No number is drawn anywhere
- One notification per message, with the sender, the text and the sender's
  picture on it. Messages sent from the phone raise nothing, and neither does the
  `typing...` the other side leaves in the chat list while writing
- A notification for a message that lands in the chat already on screen is
  skipped -- the user is reading it as it arrives, and WhatsApp plays its own
  tone for it
- Links open in the desktop's browser rather than inside the client
- Follows the desktop's dark/light preference and interface font
- Downloads land in `~/Downloads`
- WebKit's in-process memory pressure handler is switched on, so it sheds caches
  and runs GC as it approaches a ceiling. That is very different from capping
  the process with a cgroup, which can only evict pages blindly — a
  `MemoryHigh=1100M` cap on the packaged client drove `memory.pressure` to
  `full avg10=63`, frozen roughly two thirds of the time, while `cpu.pressure`
  stayed at zero.

## Install

From the [shinux repository](https://abdallah-shehawey.github.io/shinux/), which
carries signed builds for both families:

```sh
sudo dnf install whatsapp     # Fedora and friends
sudo apt install whatsapp     # Debian, Ubuntu 24.04+
```

The package installs a system-wide autostart entry, so it starts hidden in the
tray at login without writing into a home directory. Do not add `make autostart`
on top of it: login would then run `whatsapp --hidden` twice. The second launch
exits instead of raising the first one's window, but the per-user entry is
redundant.

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

State lives in `~/.local/share/whatsapp`, config in `~/.config/whatsapp`.

## Configuration

`~/.config/whatsapp/whatsapp.conf`, every key optional:

| Key | Default | What it does |
|---|---|---|
| `[view] font` | the GNOME interface font | family for everything the client draws |
| `[view] font-size` | `16` | root font size in pixels — WhatsApp sizes in rem, so this scales the client |
| `[view] zoom` | `1.0` | WebKit zoom level, also set with `Ctrl` `+`/`-` |
| `[window] width`, `[window] height` | `1200x800` | remembered on exit |

One family draws everything, chat text included. A separate reading face was
tried on the theory that a display font is a poor choice for a chat, and it was
the wrong target: a browser set to ignore page fonts draws bubbles, previews and
controls alike in the one family, and matching the browser is the point.

WhatsApp's own line boxes are left exactly as they are. A display face carrying
no Arabic sends Arabic to a fallback whose descenders reach further than the
0.31em WhatsApp leaves under the baseline, so the tails of ج ح خ and of a final ي
were shaved off — "يعني" could read as "يعن ،". The client answers that with a
taller *clip*, not a taller line: padding grows the box `overflow: hidden` cuts
against and a negative margin hands the space straight back, so every row keeps
the height WhatsApp Web gives it. Raising `line-height` instead fixes the tails
and moves every Arabic line off the rhythm the page was designed on.

## Diagnosing it

```sh
WHATSAPP_DEBUG_EVAL=/tmp/eval.js whatsapp
```

Whatever lands in that file is evaluated in the live page and the result logged;
writing `#snapshot` into it instead writes a PNG of the window to
`/tmp/whatsapp-snapshot.png`. Both exist because every question worth asking is
about a signed-in session: WebKitGTK 2.52's remote inspector does not answer on
its HTTP port, and a GNOME Wayland session will not hand a screenshot to a
process like this one. Unset by default, and deliberately so — it is a way into
a live WhatsApp session, not a feature.

## Licence

GPL-3.0. Icon origins are recorded in `data/icons/NOTICE`.
