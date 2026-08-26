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
  /* The document title counts unread CHATS -- "(2) WhatsApp" for two conversations
     holding five messages between them -- so the badge built from it was never the
     number the user sees inside the app. The per-chat pills hold the real figure.
     WhatsApp labels each one for screen readers ("3 unread messages"), which is
     both language-tagged and stable; the visible pill is the fallback. */
  window.__whatsappUnreadCount = () => {
    const pane = document.querySelector('#pane-side');
    if (!pane) return '0 0';

    let messages = 0, chats = 0;
    for (const row of pane.querySelectorAll('[role="row"]')) {
      const labels = [...row.querySelectorAll('[aria-label]')]
        .map(e => e.getAttribute('aria-label') || '');

      /* A muted chat keeps its pill but is left out of WhatsApp's own count and
         raises no notification, so counting it made the badge disagree with both
         the title and the app -- one arriving message showed as two. */
      if (labels.some(a => /muted|مكتوم|كتم/i.test(a))) continue;

      const label = [...row.querySelectorAll('[aria-label]')]
        .map(e => e.getAttribute('aria-label') || '')
        .find(a => /unread|غير مقروء/i.test(a));

      let n = 0;
      if (label) {
        const m = label.match(/(\d+)/);
        n = m ? parseInt(m[1], 10) : 1;
      } else {
        /* The pill is a leaf element holding only digits. Timestamps carry a
           colon and previews are prose, so neither collides. */
        const pill = [...row.querySelectorAll('span')].find(e =>
          e.children.length === 0 && /^\d{1,3}\+?$/.test((e.textContent || '').trim()) &&
          e.closest('[role="gridcell"]'));
        if (pill) n = parseInt(pill.textContent, 10) || 0;
      }
      if (n > 0) { messages += n; chats++; }
    }
    return messages + ' ' + chats;
  };

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

  const unreadRow = () => {
    const pane = document.querySelector('#pane-side');
    if (!pane) return null;
    const rows = [...pane.querySelectorAll('[role="row"]')];
    const marked = rows.find(r => r.querySelector('[aria-label*="unread"], [aria-label*="غير مقروء"]'));
    return marked || rows[0] || null;
  };

  /* Chat names and message previews arrive wrapped in bidi control characters,
     which have to come off before anything is compared or displayed. */
  const strip = t => (t || '').replace(/[\u200e\u200f\u202a-\u202e\u2066-\u2069]/g, '').trim();

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

  window.__whatsappDescribeUnread = async () => {
    const row = unreadRow();
    if (!row) return '';

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

    // Unit separator: it cannot occur in a chat name or a message.
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
