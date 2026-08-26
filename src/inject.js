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

  /* Describe the chat behind the newest message, so a notification can say who
     sent it and what they said. Two facts from the live DOM drive this: a chat
     list row is [role="row"] under #pane-side, and it carries two span[title]
     elements -- the chat name first, the message preview second. A new message
     moves its chat to the top of the list, so the top row is the fallback when
     no row carries an unread badge.

     Nothing here lies to the page: an earlier attempt forced document.hasFocus()
     to false, which did make WhatsApp raise notifications while the window was
     focused -- and also convinced it the user was not looking, so opening a chat
     never marked it read. */
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

  /* Which chat a message actually landed in.

     The document title only says how many chats are unread, so a notification
     built on it alone had to guess at the rest, and the guess -- the row wearing
     an unread badge -- was wrong in the one case that matters: a message arriving
     in the chat already on screen is read instantly and never wears a badge, so
     the app described some other conversation and put an old message on screen
     under a fresh banner.

     WhatsApp rewrites a chat's preview the moment a message lands there, so
     watching the list says exactly which row changed. Arrivals are queued and the
     app collects them when it raises a notification -- pushing them would race the
     title, which is the bug this replaces. */
  /* Keyed on the row itself, never on the chat name: two chats can carry the
     same name -- this account has four such pairs, and keying by name made each
     scan read one row's preview as the other's, so every single pass reported an
     arrival that had not happened. */
  const previews = new WeakMap();
  const ARRIVAL_TTL_MS = 30000;
  let arrivals = [];
  let seeded = false;

  const scanList = () => {
    const pane = document.querySelector('#pane-side');
    if (!pane) return;

    for (const row of pane.querySelectorAll('[role="row"]')) {
      const titles = [...row.querySelectorAll('span[title]')];
      if (!strip(titles[0] && titles[0].getAttribute('title'))) continue;
      const preview = strip(titles[1] && titles[1].getAttribute('title'));

      const before = previews.get(row);
      previews.set(row, preview);

      /* A row we are seeing for the first time is not news -- only one we
         already knew, whose preview has since changed. */
      if (!seeded || before === undefined || before === preview) continue;
      /* WhatsApp leaves muted chats out of its own notifications. */
      if ([...row.querySelectorAll('[aria-label]')]
            .some(e => /muted|مكتوم|كتم/i.test(e.getAttribute('aria-label') || ''))) continue;

      arrivals = arrivals.filter(a => a.row !== row);
      arrivals.push({ row, at: Date.now() });
    }

    /* Anything the app never came to collect -- it only asks while its window is
       in front -- goes stale rather than waiting to be reported as news. */
    const cutoff = Date.now() - ARRIVAL_TTL_MS;
    arrivals = arrivals.filter(a => a.row.isConnected && a.at > cutoff);
    if (arrivals.length > 8) arrivals = arrivals.slice(-8);
    seeded = true;
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

  /* The contact's picture, as bytes the app can turn into a notification icon.
     The <img> is already on screen but its canvas is tainted, so the bytes are
     re-fetched instead -- the CDN answers a plain fetch with CORS, verified
     against a live avatar (200 image/jpeg). */
  const avatarOf = async row => {
    const img = row.querySelector('img[src^="http"], img[src^="blob:"]');
    if (!img || !img.src) return '';
    try {
      const r = await fetch(img.src);
      if (!r.ok) return '';
      const bytes = new Uint8Array(await r.arrayBuffer());
      if (bytes.length > 400000) return '';
      let bin = '';
      for (let i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);
      return btoa(bin);
    } catch (e) {
      return '';
    }
  };

  /* Answers the app's one question at notification time: what just arrived, and
     was it the conversation already on screen? The reply is the chat, the sender,
     the message and the avatar joined by unit separators -- or the single word
     "open", which means stay quiet. The user is looking straight at the message
     and WhatsApp plays its own arrival tone for it, so a banner over the top of
     the very chat it came from is noise. */
  window.__whatsappDescribeUnread = async () => {
    scanList();                       // collect whatever the debounce still owes us

    const open = openRow();

    /* Newest first, and a chat other than the one on screen wins: two messages
       can land together, one in the open chat and one elsewhere, and the one
       elsewhere is still worth a banner. */
    const cutoff = Date.now() - ARRIVAL_TTL_MS;
    const queued = arrivals.filter(a => a.row.isConnected && a.at > cutoff)
                           .map(a => a.row).reverse();
    arrivals = [];

    const row = queued.find(r => r !== open) || queued[0] || null;

    /* Nothing seen changing means nothing to describe. The app falls back to a
       plain "you have unread chats" rather than being handed the row that
       happens to wear an unread badge -- that guess is what used to put a
       message from an hour ago under a banner announcing a new one. */
    if (!row) return '';
    if (row === open) return 'open';

    const titles = [...row.querySelectorAll('span[title]')];
    const name = strip(titles[0] && titles[0].getAttribute('title'));
    const message = strip(titles[1] && titles[1].getAttribute('title'));
    if (!name) return '';

    /* In a group row the sender is its own element followed by a bare ":"
       element; a one-to-one row has neither. Verified against live rows: groups
       yield "You", "+20 11 18856364", "@eng_mahmoudmajed", and direct chats
       correctly yield nothing. Reading the position of that ":" beats matching
       WhatsApp's class names, which are obfuscated and rotate every build. */
    const lines = (row.innerText || '').split('\n').map(strip);
    const colon = lines.indexOf(':');
    const sender = colon > 0 ? lines[colon - 1] : '';

    // The separator below cannot occur in a chat name or in a message.
    return [name, sender, message, await avatarOf(row)].join('\u001f');
  };

  /* Focus is pushed in by the app, because WebKit gets it wrong here: a WebView
     in a window hidden in the tray still reports itself focused. WhatsApp reads
     document.hasFocus() both to decide whether to raise a notification and to
     decide whether the chat on screen has been read, so the answer has to be the
     truth -- pinning it to false once produced notifications and cost the user
     their read receipts at the same time. */
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
