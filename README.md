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
the page. Text-only paste is left on WebKit's native path, which works fine. When a clipboard publishes an image together with auxiliary text or HTML formats, the image is preferred so WhatsApp receives a real media attachment that can be sent.

**WhatsApp thought it was talking to Safari on a Mac**, offered a Mac download
and registered the device as "Safari (Mac OS)". WebKitGTK ships site-specific
quirks that rewrite the user agent for a list of hosts including whatsapp.com,
and the quirk beats whatever the application sets — so setting a Chrome user
agent is not enough on its own, `enable-site-specific-quirks` has to go off too.

**Notifications were unreliable in every direction** — silent for some
messages, stuck on screen for others, and inventing a few that never happened.
There are two halves to getting them right, split on where the focus is.

*While the window is away*, WhatsApp Web raises its own notification, and it is
by far the better judge: it knows the sender, the text, the mute state, and that
what arrived is a message rather than a typing indicator or something sent from
the phone. What it cannot do is dress one — WebKit's default handler shows a
silent banner that lingers. So the client intercepts every notification the page
raises and sends it on itself, with the tone, the sender's picture, and a click
that opens the conversation it came from.

*While the window is in front*, WhatsApp stays silent, so the client watches the
chat list instead — one banner per message, and none at all for the conversation
already on screen. The document title cannot do that job: its number counts
unread *chats*, so a second and third message from the same person leave
`(1) WhatsApp` exactly as it was.

And **the client must be sure which chat is on screen before it goes quiet about
one**, because being wrong there is silence, not noise. It went silent for whole
bursts of messages: ten landed in a conversation sitting at ten unread and every
one of them was answered "the message is in the chat on screen". Three things
decide it now, all measured on a live session rather than assumed -- WhatsApp
draws `#main` only while a conversation is open, the `aria-selected` marker
always resolves to exactly one row, and a row still wearing an unread pill cannot
be the chat on screen, since WhatsApp clears that pill the moment it draws a chat
in a window that has focus.

Two things had to be unlearned there. **Unread is not the same as new**: the app
asks the page what arrived more often than messages actually land -- the document
title asks on its own count, off its own clock -- and every ask the watcher could
not answer used to fall through to "the topmost unread row", which announced one
chat's last message over and over while the user sat reading another. An ask now
goes unanswered unless some row has just moved, carries the time it is now, and
says something this client has not already announced. An arrival also goes stale
in five seconds rather than thirty: the nudge is dropped whenever the window is
not active in the moment a message lands -- the page raises its own notification
then -- and the entry it was for used to wait in the queue until something else
asked, minutes of alt-tabbing later, and be announced as though it had just
arrived. And **a group says it
differently**: a direct chat leaves `typing...` in the list, a group leaves
`Mega is typing...`, and only the first shape was recognised -- so a group
announced somebody starting to write as though they had said something.

The page is told the truth about focus, pushed in from GTK, because WebKit gets
it wrong: a view in a window hidden in the tray still reports itself focused.
Pinning the answer to `false` was tried and is a trap — it does produce
notifications, and it also convinces WhatsApp nobody is looking, so chats the
user reads never get their receipts.

**Banners came down on the client's own clock, in the end.** GNOME reads the
`expire_timeout` of a notification and throws it away: a banner leaves the screen
when the user has been active *and* the pointer is not resting on it. Since the
shell shows one banner at a time, queues three behind it and drops the rest, a
single banner parked under an idle mouse pointer silently swallows every message
that follows. Measured live: with one stuck, six notifications produced no banner
and no sound between them, including one sent at `CRITICAL` urgency — and the
moment it went away, the next one rang. Each banner is now closed after twelve
seconds and the message posted again at `LOW` urgency, which the shell files in
the notification centre without a banner and without a sound. Nothing is lost and
nothing blocks.

**Emoji arrived blank, every single launch.** They are drawn from 152 sprite
sheets of about 30 KB each, and nothing on the machine was keeping them:
WhatsApp's service worker caches its JavaScript but hands the sheets to the
network, and WebKit's disk cache stored none of them — 55 records on disk and not
one an image. So every start pulled the same 4.7 MB down again, all 152 at once
over six connections, and the emoji panel sat full of blank squares until it
finished. The client keeps them itself now, in CacheStorage, which does survive a
restart. Both of the ways a sheet is asked for are served from the stored copy:
the generated `background-image` rules are overridden with a `blob:` URL, and
WhatsApp's own preload — one XHR per sheet — is pointed at the same URL. Measured
across a restart: **328 requests and 9.3 MB became 2 requests and 0.05 MB**. The
first run still downloads them once; every run after it draws emoji offline.

A note on the softness that came with it: WhatsApp picks 1x or 2x sheets from the
display resolution, and a Wayland session at scale 1 gets the 40px tiles however
far the view is zoomed. The client answers resolution queries for the page with
the scale it is really drawn at, and now does so at any zoom above 1 rather than
above 1.25 — squeezing a 64px tile is sharp where stretching a 40px one is not,
and the sheets cost nothing after the first run.

## What it does

- Starts hidden at login and lives in the tray; the page still loads and
  notifications still arrive with no window on screen
- Tray icon marks itself unread, driven by WhatsApp's own `(3) WhatsApp`
  document title. No number is drawn anywhere
- One notification per message, with the sender, the text and the sender's
  picture on it. Messages sent from the phone raise nothing, and neither does the
  `typing...` the other side leaves in the chat list while writing, in a group
  or a direct chat and in either language
- Every message stays in the notification centre after its banner has gone,
  including each one of a burst from the same conversation
- A notification for a message that lands in the chat already on screen is
  skipped -- the user is reading it as it arrives, and WhatsApp plays its own
  tone for it
- Nothing is announced that the client cannot name. A row the chat list rewrote
  during a sync is not a message: an arrival has to carry the current clock, so
  the backlog that syncs in over the first half-minute after a launch stays quiet
- Pictures are stored under a name taken from the picture itself, because a
  notification holds the *path* of its icon and the shell reads it lazily -- a
  rotating name meant a later message could rewrite the file under a banner that
  was still on screen, and put one sender's face on another sender's message
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
make test-inject    # replay a chat list past src/inject.js, no browser needed
```

`make install` honours `DESTDIR` and `PREFIX`, so the tree packages cleanly.

`make test-inject` is where the notification logic is exercised. Every bug it has
had was found by hand on a live session -- which means waiting for somebody to
write to you, and being unable to reproduce what you just saw. The watcher only
reaches the page through a handful of selectors, so a mock chat list is enough to
move a row the way WhatsApp moves one and ask the page what arrived. It needs
`node`, and nothing else.

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
| `tools/test-inject.js` | replays a chat list past `src/inject.js` — `make test-inject` |

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
