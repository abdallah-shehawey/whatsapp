# whatsapp

> **There is a better one now.**
> [**whatsapp-desktop**](https://github.com/abdallah-shehawey/whatsapp-desktop)
> is the successor to this client, built on Electron and Chromium. It does
> everything below and adds what WebKitGTK could not give this one: **voice and
> video calls with screen sharing**, and notifications that are read from
> WhatsApp Web's own store rather than inferred from its chat list — so reading
> a message on your phone takes its notification down at once, a deleted message
> takes its own with it, and a mention gets through a muted group without letting
> the rest of the group through behind it. It is packaged in the same repository,
> as `whatsapp-desktop`, and the two can be installed side by side.
>
> This one is still maintained and still works. It is 120 KB against 245 MB, so
> if that trade is the one you want, it is here.

A small WhatsApp Web client for GTK4 + WebKitGTK 6. The binary is around 120 KB.
It loads `web.whatsapp.com`, so it is the same client WhatsApp serves to a
browser — no reverse-engineered protocol, and nothing that puts an account at
risk.

## What it does

- Lives in the tray and can start hidden at login — the page stays loaded and
  notifications keep arriving with no window on screen.
- One notification per message, with the sender, the text and the sender's
  picture, and a click that opens that conversation. Nothing is raised for a
  message you sent from your phone, for `typing...`, or for the chat already on
  screen.
- Every message stays in the notification centre after its banner is gone.
- Draws the whole page in your desktop's font, and follows dark/light live.
- Working `Ctrl+V` for images, links open in your browser, downloads land in
  `~/Downloads`, and the tray marks itself unread from WhatsApp's own title.
- Arabic keeps its descenders: the clip is widened rather than the line, so rows
  stay the height WhatsApp gives them.

## Install

From the [shinux repository](https://abdallah-shehawey.github.io/shinux-repo/):

```sh
curl -fsSL https://abdallah-shehawey.github.io/shinux-repo/install.sh | sudo sh
sudo dnf install whatsapp     # Fedora and friends
sudo apt install whatsapp     # Debian, Ubuntu 24.04+
sudo pacman -S whatsapp       # Arch
```

Packages are also on every [release](../../releases). The package installs a
system-wide autostart entry, so do not add `make autostart` on top of it.

## Build

```sh
sudo dnf install gtk4-devel webkitgtk6.0-devel          # Fedora
sudo apt install libgtk-4-dev libwebkitgtk-6.0-dev      # Debian/Ubuntu

make
make install        # ~/.local/bin plus a desktop entry and icons
make autostart      # also start hidden at login
make test-inject    # replay a chat list past src/inject.js, no browser needed
```

`make install` honours `DESTDIR` and `PREFIX`.

## Keys

| | |
|---|---|
| `Ctrl+V` | paste an image |
| `Ctrl` `+` / `-` / `0` | zoom in, out, reset |
| `Ctrl+Q` | quit for real |
| window close | hides to the tray, stays connected |

## Configuration

`~/.config/whatsapp/whatsapp.conf`, every key optional:

| Key | Default | What it does |
|---|---|---|
| `[view] font` | the GNOME interface font | family for everything the client draws |
| `[view] font-size` | `16` | root font size in pixels — WhatsApp sizes in rem |
| `[view] zoom` | `1.0` | also set with `Ctrl` `+`/`-` |
| `[window] width`, `height` | `1200x800` | remembered on exit |

State lives in `~/.local/share/whatsapp`.

## Layout

| | |
|---|---|
| `src/main.c` | window, WebKit setup, clipboard, notifications, config |
| `src/tray.c` | `StatusNotifierItem` over raw GDBus — GTK4 has no tray, and libayatana-appindicator is GTK2/GTK3 only |
| `src/inject.js` | page-side helpers, compiled into the binary |
| `tools/make-icons.py` | regenerates `data/icons` — `make icons`, never hand-edit the PNGs |
| `tools/test-inject.js` | replays a chat list past `src/inject.js` |

## Diagnosing it

```sh
WHATSAPP_DEBUG_EVAL=/tmp/eval.js whatsapp
```

Whatever lands in that file is evaluated in the live page and the result logged;
`#snapshot` writes a PNG of the window instead. Unset by default — it is a way
into a live WhatsApp session, not a feature.

## Licence

GPL-3.0. Icon origins are recorded in `data/icons/NOTICE`.
