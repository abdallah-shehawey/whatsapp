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

  /* Report the chat behind the newest unread badge, so the app can name it in a
     notification. Nothing here lies to the page: an earlier attempt forced
     document.hasFocus() to false, which did make WhatsApp raise notifications
     while the window was focused -- and also convinced it the user was not
     looking, so opening a chat never marked it read. */
  const unreadChat = () => {
    const badge = document.querySelector('[aria-label*="unread"], [aria-label*="غير مقروءة"]');
    const row = badge && badge.closest('[role="listitem"], [role="row"]');
    const name = row && row.querySelector('span[title]');
    return name ? name.getAttribute('title') : '';
  };

  const reportUnread = () => {
    try {
      window.webkit.messageHandlers.whatsappUnread.postMessage(unreadChat());
    } catch (e) { /* handler only exists in the host app */ }
  };

  const title = document.querySelector('title');
  if (title) new MutationObserver(reportUnread).observe(title, { childList: true });
  addEventListener('DOMContentLoaded', reportUnread);

  log('UA: ' + navigator.userAgent);
  log('inject.js ready on ' + location.host);
})();
