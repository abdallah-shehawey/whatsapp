/*
 * Page-side helpers for whatsapp.
 *
 * The paste entry point exists because WebKitGTK hands the page an empty
 * clipboardData for images: a real Ctrl+V fires `paste` with
 * types=[] items=[] files=[], so WhatsApp's own handler finds nothing and drops
 * it. Rather than fight WebKit's clipboard, whatsapp reads the image on the GTK
 * side and calls in here with the bytes already decoded.
 */
(() => {
  'use strict';

  const log = m => {
    try { window.webkit.messageHandlers.whatsapp.postMessage(String(m)); } catch (e) {}
  };

  const findComposer = () => {
    const active = document.activeElement;
    if (active && active.closest && active.closest('[contenteditable="true"]')) return active;
    // Fall back to the last visible editable box, which is the message composer;
    // the first one on the page is the chat search field.
    const boxes = Array.from(document.querySelectorAll('[contenteditable="true"]'))
                       .filter(el => el.offsetParent !== null);
    return boxes.length ? boxes[boxes.length - 1] : document.body;
  };

  /* Called from C with base64 image bytes lifted straight off the GTK clipboard. */
  window.__whatsappPasteImage = (b64, mime) => {
    let bytes;
    try {
      const bin = atob(b64);
      bytes = new Uint8Array(bin.length);
      for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
    } catch (e) {
      log('paste: could not decode base64: ' + e.message);
      return 'decode-failed';
    }

    const type = mime || 'image/png';
    const file = new File([bytes], 'pasted-image.' + (type.split('/')[1] || 'png'),
                          { type, lastModified: Date.now() });

    const carrier = new DataTransfer();
    carrier.items.add(file);

    // WebKit refuses clipboardData through the ClipboardEvent constructor, so
    // define it on a plain event instead.
    const ev = new Event('paste', { bubbles: true, cancelable: true });
    Object.defineProperty(ev, 'clipboardData', { value: carrier });
    ev.__whatsappSynthetic = true;

    const target = findComposer();
    target.dispatchEvent(ev);

    const label = target.tagName + (target.className ? '.' + String(target.className).slice(0, 24) : '');
    log('paste: dispatched ' + file.size + 'B ' + type + ' to ' + label);
    return label;
  };

  addEventListener('DOMContentLoaded', () => {
    const style = getComputedStyle(document.body);
    log('appearance: scheme=' +
        (matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light') +
        ' font=' + style.fontFamily.slice(0, 48) + ' size=' + style.fontSize);
  });

  /* WebKitGTK hands the page an empty clipboardData for images. Report those
     pastes so the app can supply the bytes from the GTK clipboard instead. */
  document.addEventListener('paste', ev => {
    if (ev.__whatsappSynthetic) return;
    const dt = ev.clipboardData;
    if (dt && (dt.files.length || dt.types.length)) return;   // nothing wrong here
    ev.preventDefault();
    ev.stopImmediatePropagation();
    log('paste: clipboardData empty, asking the app');
    try { window.webkit.messageHandlers.whatsappPaste.postMessage('image'); } catch (e) {}
  }, true);

  /* Instrumentation: WhatsApp may raise its notification from the page or from
     its service worker, and it plays its own tone through an <audio> element.
     Both are logged so the app can tell which paths are live rather than guess. */
  try {
    const RealNotification = window.Notification;
    const Shim = function (title, options) {
      log('page notification: ' + String(title).slice(0, 40));
      return new RealNotification(title, options);
    };
    Shim.prototype = RealNotification.prototype;
    Object.defineProperty(Shim, 'permission', { get: () => RealNotification.permission });
    Shim.requestPermission = (...a) => RealNotification.requestPermission(...a);
    window.Notification = Shim;

    const realPlay = HTMLMediaElement.prototype.play;
    HTMLMediaElement.prototype.play = function (...args) {
      log('page sound: ' + String(this.currentSrc || this.src).slice(-28));
      return realPlay.apply(this, args);
    };
  } catch (err) {
    log('could not instrument notifications: ' + err.message);
  }

  /* ------------------------------------------------------------------ focus */

  /* Focus is pushed in by the app, because WebKit gets it wrong here: a WebView
     in a window hidden in the tray still reports itself focused. WhatsApp reads
     document.hasFocus() both to decide whether to raise a notification and to
     decide whether the chat on screen has been read, so the answer has to be the
     truth -- pinning it to false once produced notifications and cost the user
     their read receipts at the same time.

     It is also the line the whole notification story is divided along. While the
     page believes itself unfocused it raises its own notifications, which the app
     intercepts and dresses; the watcher below then has nothing to do, and does
     nothing, because two paths reporting one message is two banners. */
  let focused = false;
  try {
    Object.defineProperty(document, 'hasFocus', { configurable: true, value: () => focused });
  } catch (err) {
    log('could not override document.hasFocus: ' + err.message);
  }

  window.__whatsappSetFocus = state => {
    state = !!state;
    if (state === focused) return 'unchanged';
    focused = state;
    // WhatsApp acts on the events, not on a poll of hasFocus().
    window.dispatchEvent(new Event(state ? 'focus' : 'blur'));
    document.dispatchEvent(new Event(state ? 'focus' : 'blur'));
    return state ? 'focused' : 'blurred';
  };

  /* ------------------------------------------------------- what just arrived */

  /* Everything below only matters while the window is in front. WhatsApp Web
     stays silent then -- it can see it has the user's attention -- so a message
     landing in a conversation the user is not looking at would pass unannounced.
     The chat list is watched for it: WhatsApp rewrites a row the moment a message
     lands there, so the row that changed is the chat the message went to. */

  /* The chat on screen. WhatsApp marks its row aria-selected="true", which beats
     reading the conversation header: the header of a community announcement group
     carries title="Announcements", not the name of the group. */
  const openRow = () => {
    const pane = document.querySelector('#pane-side');
    const selected = pane && pane.querySelector('[aria-selected="true"]');
    return selected ? selected.closest('[role="row"]') || selected : null;
  };

  /* Chat names and message previews arrive wrapped in bidi control characters,
     which have to come off before anything is compared or displayed. */
  const strip = t => (t || '').replace(/[\u200e\u200f\u202a-\u202e\u2066-\u2069]/g, '').trim();

  const titlesIn = row => [...row.querySelectorAll('span[title]')];
  const nameOf = row => {
    const first = titlesIn(row)[0];
    return strip(first && first.getAttribute('title'));
  };

  /* A message of our own moves a chat to the top of the list and rewrites its
     preview exactly the way an incoming one does, so without this a message sent
     from the phone raised a banner on the desktop. The delivery tick on the row
     is what tells them apart -- WhatsApp draws one only for what we sent, under
     its design-system name (wds-ic-read and friends) or the older msg- names.
     Class names could not be used: they are obfuscated and rotate every build. */
  const OUTGOING_ICON = /^(wds-)?(ic-)?(msg-)?(status-)?(read|delivered|sent|check|dblcheck|clock|time)$/;
  /* The name is not always in the same place. WhatsApp's current build gives the
     tick an <svg> whose only marking is a <title> child reading "wds-ic-read";
     older ones put data-icon on the element. Both are read, which is what this
     costs -- looking only at data-icon found nothing at all and every message
     sent from the phone raised a banner on the desktop. */
  const iconNames = el => {
    const names = [...el.querySelectorAll('[data-icon]')]
        .map(i => i.getAttribute('data-icon') || '');
    for (const svg of el.querySelectorAll('svg')) {
      names.push(svg.getAttribute('title') || '');
      const inner = svg.querySelector('title');
      if (inner) names.push(inner.textContent || '');
    }
    return names;
  };
  const isOutgoing = el => iconNames(el).some(n => OUTGOING_ICON.test(n));

  /* WhatsApp leaves muted chats out of its own notifications, so this client
     does too. */
  const MUTED_LABEL = /muted|\u0645\u0643\u062a\u0648\u0645|\u0643\u062a\u0645/i;
  const isMuted = row => [...row.querySelectorAll('[aria-label]')]
      .some(e => MUTED_LABEL.test(e.getAttribute('aria-label') || ''));

  /* How many messages the row says are waiting. The pill carries the number in
     its label ("3 unread messages"); a row with no pill is caught up. */
  const UNREAD_LABEL = /unread|\u063a\u064a\u0631 \u0645\u0642\u0631\u0648\u0621/i;
  const unreadCount = row => {
    for (const el of row.querySelectorAll('[aria-label]')) {
      const label = el.getAttribute('aria-label') || '';
      if (!UNREAD_LABEL.test(label)) continue;
      const digits = label.match(/\d+/);
      return digits ? parseInt(digits[0], 10) : 1;
    }
    return 0;
  };

  /* The app is told that something landed; it then asks what. Only the nudge is
     pushed -- pushing the description is the race that used to make every banner
     read "You have a new message". */
  const ping = () => {
    try { window.webkit.messageHandlers.whatsappEvent.postMessage('arrival'); } catch (e) {}
  };

  /* Keyed on the row itself, never on the chat name: two chats can carry the
     same name -- this account has four such pairs, and keying by name made each
     scan read one row's preview as the other's, so every single pass reported an
     arrival that had not happened. */
  const rowState = new WeakMap();
  const ARRIVAL_TTL_MS = 30000;
  /* The list does not arrive in one piece: rows appear, and then their previews,
     their badges and their timestamps fill in behind them. Every one of those is
     a change to a row we have already seen, which is the shape of an arrival --
     and on the first launch after this watcher was written, two chats that had
     been sitting there for hours were announced as new. Nothing counts until the
     list has stood still for a moment. */
  const SETTLE_MS = 2500;
  let arrivals = [];
  let seeded = false;
  let seededAt = 0;

  /* The preview WhatsApp shows while the other side is writing, in the languages
     this client is likely to be run in. A group row carries the name in front of
     it ("Ahmed: typing..."), hence the optional prefix. Anchored at both ends on
     purpose: a message that merely begins with the word "typing" is a message,
     and swallowing it would cost a banner. \b cannot do that job here -- it is
     defined on ASCII word characters, so it never matches after Arabic. */
  const TYPING_PREVIEW =
      /^(?:[^:]{1,40}:\s*)?(typing|recording(?: audio)?|\u064a\u0643\u062a\u0628|\u064a\u0633\u062c\u0644)\s*(?:\.{1,3}|\u2026)?$/i;

  /* What is read off a row on every pass. Three things move when a message
     lands, and it takes all three to catch every one: the preview, because that
     is the message; the timestamp, because a second "tamam" under the first
     leaves the preview identical and that message went unannounced; and the
     unread count, because two identical messages inside the same minute move
     nothing else at all. */
  const readRow = row => {
    const titles = titlesIn(row);
    return {
      name:    strip(titles[0] && titles[0].getAttribute('title')),
      preview: strip(titles[1] && titles[1].getAttribute('title')),
      badge:   unreadCount(row),
      when:    ((row.innerText || '').match(/\b\d{1,2}:\d{2}(?:\s*[AP]M)?\b/) || [''])[0],
    };
  };

  /* Whether the time a row shows is the time it is now. WhatsApp stamps a row
     with the time of its last message, so a row rewritten by a sync -- which is
     what the whole chat list does for half a minute after the client starts, and
     again after the network comes back -- carries an old one. Four conversations
     were announced fifteen seconds into a launch this way, and that is what "it
     shows me phantom notifications when I open it" was.

     Returns null rather than false when the format is not one this can read, so
     a locale that writes its clock in digits the regex above cannot match falls
     back to the unread count instead of going silent. */
  const FRESH_MS = 3 * 60 * 1000;
  const freshness = when => {
    const m = /^(\d{1,2}):(\d{2})(?:\s*([AP])\.?M\.?)?$/i.exec(when || '');
    if (!m) return null;

    let hour = parseInt(m[1], 10);
    if (m[3]) hour = (hour % 12) + (/p/i.test(m[3]) ? 12 : 0);

    const now = new Date();
    const stamp = new Date(now);
    stamp.setHours(hour, parseInt(m[2], 10), 0, 0);

    let age = now - stamp;
    if (age < -FRESH_MS) age += 24 * 60 * 60 * 1000;   // the clock has just passed midnight
    return age >= -FRESH_MS && age <= FRESH_MS;
  };

  /* Whether the difference between two readings of one row is a message landing.
     Comparing the readings wholesale is what put phantom banners on screen: the
     badge clears when a chat is read, so every conversation the user opened --
     and the whole backlog clearing when the window came back from the tray --
     looked exactly like an arrival. An unread count going DOWN is the user
     catching up, and is never news; a row whose clock says half an hour ago is
     WhatsApp rewriting it, not somebody writing to it. */
  const isArrival = (before, now) => {
    const changed = now.preview !== before.preview ||
                    now.when !== before.when ||
                    now.badge > before.badge;
    if (!changed) return false;

    const fresh = freshness(now.when);
    return fresh === null ? now.badge > before.badge : fresh;
  };

  const scanList = () => {
    const pane = document.querySelector('#pane-side');
    if (!pane) return;

    for (const row of pane.querySelectorAll('[role="row"]')) {
      const now = readRow(row);
      if (!now.name) continue;

      const before = rowState.get(row);

      /* Neither of these is a message, and both are skipped before the row's
         state is recorded, so the text that replaces them matches what was there
         before and does not read as an arrival of its own. "typing..." is what
         WhatsApp writes in the preview while the other side is still writing --
         it announced "Mega -- typing..." as though it were something somebody
         had said -- and an empty preview is a row mid-render. */
      if (TYPING_PREVIEW.test(now.preview)) continue;
      if (!now.preview && before !== undefined) continue;

      rowState.set(row, now);

      /* A row we are seeing for the first time is not news -- only one we
         already knew, whose message has since changed. */
      if (!seeded || before === undefined) continue;
      if (Date.now() - seededAt < SETTLE_MS) continue;
      if (!isArrival(before, now)) continue;
      if (isMuted(row)) continue;
      if (isOutgoing(row)) continue;

      /* Nothing is queued while the window is away: WhatsApp raises its own
         notification then, and the app dresses that one instead. A queue built
         up in the background used to be handed over the moment the window came
         back, and every message in it was announced a second time. */
      if (!focused) continue;

      /* Queued per message rather than per chat: the app asks once for each one,
         and collapsing them here is what swallowed the second and third message
         of a burst from the same person. */
      arrivals.push({ row, name: now.name, preview: now.preview, at: Date.now() });
      ping();
    }

    const cutoff = Date.now() - ARRIVAL_TTL_MS;
    arrivals = arrivals.filter(a => a.at > cutoff);
    /* Deep enough for a burst the app has not caught up with yet; it asks once
       per message, so this is a backstop, not a queue depth. */
    if (arrivals.length > 16) arrivals = arrivals.slice(-16);
    if (!seeded) { seeded = true; seededAt = Date.now(); }
  };

  const watchList = () => {
    const pane = document.querySelector('#pane-side');
    if (!pane || pane.__whatsappWatched) return;
    pane.__whatsappWatched = true;

    scanList();                       // seed first, so the opening pass is silent
    let timer = 0;
    new MutationObserver(() => {
      clearTimeout(timer);
      timer = setTimeout(scanList, 150);
    }).observe(pane, { childList: true, subtree: true, characterData: true,
                       attributes: true, attributeFilter: ['title'] });
    log('watching the chat list for arrivals');
  };

  /* #pane-side is rebuilt when the client re-renders, taking the observer with
     it, so the watch is re-established rather than set up once. */
  setInterval(watchList, 4000);
  addEventListener('load', watchList);

  /* The row for a chat WhatsApp has re-rendered since. Rows are recycled freely,
     and an arrival whose element was thrown away in the 250ms before the app
     asked about it used to fall through to "nothing identified" -- which is what
     raised a banner reading "You have a new message" over a conversation the user
     was already reading. The message rides along with the name, so a chat that
     shares its name with another is still told apart. */
  const findRow = (name, preview) => {
    const pane = document.querySelector('#pane-side');
    for (const row of (pane ? pane.querySelectorAll('[role="row"]') : [])) {
      const titles = titlesIn(row);
      if (strip(titles[0] && titles[0].getAttribute('title')) !== name) continue;
      if (preview && strip(titles[1] && titles[1].getAttribute('title')) !== preview) continue;
      return row;
    }
    return null;
  };

  /* The text of the last message drawn in the conversation on screen. */
  const lastOnScreen = () => {
    const main = document.querySelector('#main');
    const rows = main ? main.querySelectorAll('[role="row"]') : [];
    const last = rows[rows.length - 1];
    return last ? strip(last.innerText) : '';
  };

  /* Whether this row is the conversation the user is looking at. Element
     identity answers it whenever the row survived; when WhatsApp recycled it the
     name has to, and the name alone is not enough -- this account has four pairs
     of chats that share one -- so the message has to be on screen as well. */
  const isOpen = (row, preview) => {
    const open = openRow();
    if (!open) return false;
    if (row === open || open.contains(row) || row.contains(open)) return true;

    const name = nameOf(row);
    if (!name || name !== nameOf(open)) return false;
    const text = strip(preview).replace(/\u2026$/, '');
    return !!text && lastOnScreen().indexOf(text) >= 0;
  };

  /* Pictures are fetched once per URL and kept. The same face comes back for
     every message of a burst, and a network round trip in front of every banner
     is a banner that arrives late. */
  const avatars = new Map();
  const AVATAR_MAX_BYTES = 200000;
  const AVATAR_TIMEOUT_MS = 1200;

  const bytesToBase64 = bytes => {
    let binary = '';
    /* In chunks: fromCharCode.apply over the whole array blows the argument
       limit, and a character at a time over 200 KB is slow enough to be felt as
       a stutter, since this runs on the page's own thread. */
    for (let i = 0; i < bytes.length; i += 8192)
      binary += String.fromCharCode.apply(null, bytes.subarray(i, i + 8192));
    return btoa(binary);
  };

  /* The <img> is already on screen but its canvas is tainted, so the bytes are
     re-fetched instead -- the CDN answers a plain fetch with CORS, verified
     against a live avatar (200 image/jpeg). */
  const fetchAvatar = async src => {
    if (avatars.has(src)) return avatars.get(src);

    let encoded = '';
    try {
      const response = await fetch(src);
      if (response.ok) {
        const bytes = new Uint8Array(await response.arrayBuffer());
        if (bytes.length && bytes.length <= AVATAR_MAX_BYTES) encoded = bytesToBase64(bytes);
      }
    } catch (e) { /* offline, or the URL expired: the app icon will do */ }

    if (avatars.size > 64) avatars.clear();
    avatars.set(src, encoded);
    return encoded;
  };

  /* A picture must never hold a notification up. Better plain than late. */
  const avatarOf = async row => {
    const img = row && row.querySelector('img[src^="http"], img[src^="blob:"]');
    if (!img || !img.src) return '';
    return Promise.race([
      fetchAvatar(img.src),
      new Promise(resolve => setTimeout(() => resolve(''), AVATAR_TIMEOUT_MS)),
    ]);
  };

  /* Asked by name when the notification is one the page raised: a
     WebKitNotification carries text and nothing else, and the app still wants the
     sender's face on the banner. WhatsApp titles a group notification with the
     group name and a direct one with the contact, so an exact match is tried
     first and a containing one after it. */
  window.__whatsappAvatarFor = async name => {
    const wanted = strip(name);
    if (!wanted) return '';

    const pane = document.querySelector('#pane-side');
    const rows = [...(pane ? pane.querySelectorAll('[role="row"]') : [])];
    let match = rows.find(row => nameOf(row) === wanted);

    if (!match)
      match = rows.find(row => {
        const rowName = nameOf(row);
        return rowName.length > 2 &&
               (wanted.indexOf(rowName) >= 0 || rowName.indexOf(wanted) >= 0);
      });

    return match ? avatarOf(match) : '';
  };

  /* Answers the app's one question at notification time: what just arrived, and
     was it the conversation already on screen? The reply is the chat, the sender,
     the message and the avatar joined by unit separators -- or the single word
     "open", which means stay quiet, or an empty string, which means there is
     nothing to say and the app should say nothing. There is deliberately no
     third answer: a banner whose text the app had to invent is the phantom this
     client kept raising. */
  window.__whatsappDescribeUnread = async () => {
    scanList();                       // collect whatever the debounce still owes us

    /* Oldest first, one per call. The app raises a banner for every message, so
       draining the queue for a single description would announce the newest
       arrival and quietly discard the rest -- messages the user never saw. */
    const cutoff = Date.now() - ARRIVAL_TTL_MS;
    arrivals = arrivals.filter(a => a.at > cutoff);

    let row = null, queued = null;
    while (arrivals.length && !row) {
      queued = arrivals.shift();
      row = queued.row.isConnected ? queued.row : findRow(queued.name, queued.preview);
    }
    /* The message landed in the chat on screen: the user is reading it as it
       arrives and WhatsApp plays its own tone, so a banner over the top of the
       very conversation it came from is noise. */
    if (row && isOpen(row, queued.preview)) return 'open';

    /* Nothing queued and the app still asked, which means the document title saw
       a chat go unread that the watcher never did: the list only renders the
       rows near the top, and a message to a chat below them arrives on an
       element we have no previous reading for. The topmost unread row is the one
       WhatsApp just moved up there. This is a guess, and it is confined to the
       case where there is nothing better -- the queue is what answers every
       message from a chat already on the list. */
    if (!row) {
      const pane = document.querySelector('#pane-side');
      for (const candidate of (pane ? pane.querySelectorAll('[role="row"]') : [])) {
        if (!unreadCount(candidate)) continue;
        if (isOpen(candidate, '') || isMuted(candidate) || isOutgoing(candidate)) continue;
        row = candidate;
        break;
      }
    }

    /* Nothing changed and nothing unread: there is genuinely nothing to say. */
    if (!row) return '';

    const state = readRow(row);
    if (!state.name || !state.preview) return '';

    /* In a group row the sender is its own element followed by a bare ":"
       element; a one-to-one row has neither. Verified against live rows: groups
       yield "You", "+20 11 18856364", "@eng_mahmoudmajed", and direct chats
       correctly yield nothing. Reading the position of that ":" beats matching
       WhatsApp's class names, which are obfuscated and rotate every build. */
    const lines = (row.innerText || '').split('\n').map(strip);
    const colon = lines.indexOf(':');
    const sender = colon > 0 ? lines[colon - 1] : '';

    // The separator below cannot occur in a chat name or in a message.
    return [state.name, sender, state.preview, await avatarOf(row)].join('\u001f');
  };

  /* Emoji are sprite sheets referenced from generated CSS, and each one is only
     fetched when an emoji that lives on it first has to be drawn -- which is why
     emoji show up as blank gaps for the first moments after a cold start, and
     why they all went blank at once when the app switched to the 2x sheets. The
     sheets are ~15 KB each and never change, so warming them while the browser
     is idle costs nothing and removes the gap entirely. */
  const warmEmojiSheets = () => {
    const urls = new Set();
    for (const sheet of document.styleSheets) {
      try {
        for (const rule of sheet.cssRules) {
          const m = rule.cssText && rule.cssText.match(/url\("([^"]*\/sprite\/[^"]*)"\)/);
          if (m) urls.add(m[1]);
        }
      } catch (e) { /* a cross-origin sheet cannot be walked; skip it */ }
    }
    urls.forEach(u => { const i = new Image(); i.src = u; });
    if (urls.size) log('warmed ' + urls.size + ' emoji sheets');
  };

  addEventListener('load', () => {
    const idle = window.requestIdleCallback || (fn => setTimeout(fn, 3000));
    idle(warmEmojiSheets, { timeout: 8000 });
    // WhatsApp generates most sprite rules only once the chat list is drawn.
    setTimeout(() => idle(warmEmojiSheets, { timeout: 8000 }), 12000);
  });

  log('UA: ' + navigator.userAgent);
  log('inject.js ready on ' + location.host);
})();
