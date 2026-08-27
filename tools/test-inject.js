/*
 * Replays the chat list past src/inject.js, without WhatsApp and without a
 * browser.
 *
 * Every notification bug this client has had lived in the page-side watcher, and
 * every one of them was found by hand on a live session -- which means waiting
 * for somebody to write to you, and being unable to reproduce what you just saw.
 * The watcher only ever touches the DOM through a handful of selectors, so a few
 * hundred lines of mock element are enough to drive it: build a chat list, move
 * a row the way WhatsApp moves one, and ask the page what arrived.
 *
 * Run it with `make test-inject`. No dependencies, no browser, no account.
 */
'use strict';

const fs   = require('fs');
const path = require('path');
const vm   = require('vm');

const SRC = process.env.WA_INJECT || path.join(__dirname, '..', 'src', 'inject.js');
const US  = String.fromCharCode(31);      // the unit separator the page answers with

/* ------------------------------------------------------------ the mock DOM */

/* Only the selector forms inject.js actually uses: a tag, an #id, [attr],
   [attr="value"], [attr^="value"], and comma-separated lists of those. */
const parseSel = sel => sel.split(',').map(part => {
  const whole = /^([a-zA-Z]*)((?:#[\w-]+|\[[^\]]+\])*)$/.exec(part.trim());
  if (!whole) throw new Error('unsupported selector: ' + part);
  const tests = [];
  if (whole[1]) tests.push(el => el.tagName.toLowerCase() === whole[1].toLowerCase());
  for (const cond of whole[2].match(/#[\w-]+|\[[^\]]+\]/g) || []) {
    if (cond[0] === '#') {
      const id = cond.slice(1);
      tests.push(el => el.attrs.id === id);
      continue;
    }
    const attr = /^\[([\w-]+)(?:(\^?=)"([^"]*)")?\]$/.exec(cond);
    if (!attr) throw new Error('unsupported condition: ' + cond);
    const [, name, op, want] = attr;
    if (!op) tests.push(el => el.attrs[name] !== undefined);
    else if (op === '=') tests.push(el => el.attrs[name] === want);
    else tests.push(el => String(el.attrs[name] || '').startsWith(want));
  }
  return el => tests.every(test => test(el));
});

class El {
  constructor(tag, attrs = {}, text = '') {
    this.tagName = tag.toUpperCase();
    this.attrs = attrs;
    this.text = text;
    this.children = [];
    this.parentNode = null;
  }
  append(...kids) {
    for (const kid of kids) { kid.parentNode = this; this.children.push(kid); }
    return this;
  }
  remove() {
    const parent = this.parentNode;
    if (!parent) return;
    parent.children.splice(parent.children.indexOf(this), 1);
    this.parentNode = null;
  }
  /* WhatsApp recycles rows constantly and the watcher checks for it, so the
     mock has to answer honestly: connected means reachable from the root. */
  get isConnected() {
    let node = this;
    while (node.parentNode) node = node.parentNode;
    return node.__root === true;
  }
  getAttribute(name) { const v = this.attrs[name]; return v === undefined ? null : v; }
  setAttribute(name, value) { this.attrs[name] = value; }
  get textContent() { return this.text + this.children.map(c => c.textContent).join(''); }
  get innerText() {
    return this.attrs.__innerText !== undefined ? this.attrs.__innerText : this.textContent;
  }
  *walk() { for (const kid of this.children) { yield kid; yield* kid.walk(); } }
  querySelectorAll(sel) {
    const tests = parseSel(sel);
    return [...this.walk()].filter(el => tests.some(test => test(el)));
  }
  querySelector(sel) { return this.querySelectorAll(sel)[0] || null; }
  contains(other) { let n = other; while (n) { if (n === this) return true; n = n.parentNode; } return false; }
  closest(sel) {
    const tests = parseSel(sel);
    let node = this;
    while (node) { if (tests.some(test => test(node))) return node; node = node.parentNode; }
    return null;
  }
}

const el = (tag, attrs, text) => new El(tag, attrs, text);

/* --------------------------------------------------------- the chat list */

const root = el('div', {});
root.__root = true;
const pane = el('div', { id: 'pane-side' });
const main = el('div', { id: 'main' });
root.append(pane, main);

const two = n => String(n).padStart(2, '0');
/* The clock a row wears. Rows carry the time of their last message, and the
   watcher refuses to call anything older than three minutes an arrival. */
const clock = (backMinutes = 0) => {
  const d = new Date(Date.now() - backMinutes * 60000);
  return (d.getHours() % 12 || 12) + ':' + two(d.getMinutes()) +
         ' ' + (d.getHours() >= 12 ? 'PM' : 'AM');
};

/* A row as WhatsApp draws one: the chat name and the message preview in
   span[title] elements, the unread pill in an aria-label, the sender of a group
   message as its own line followed by a bare ":" line, and the delivery tick as
   an <svg> carrying a <title> -- which is where this build puts it. */
const mkRow = spec => {
  const row = el('div', { role: 'row' });
  if (spec.open) row.attrs['aria-selected'] = 'true';
  row.append(el('span', { title: spec.name }), el('span', { title: spec.preview }));
  if (spec.badge) row.append(el('div', { 'aria-label': spec.badge + ' unread messages' }));
  if (spec.muted) row.append(el('div', { 'aria-label': 'muted' }));
  if (spec.outgoing) {
    const svg = el('svg', {});
    svg.append(el('title', {}, 'wds-ic-read'));
    row.append(svg);
  }
  row.__spec = spec;
  row.paint = () => {
    const s = row.__spec;
    const titles = row.querySelectorAll('span[title]');
    titles[0].setAttribute('title', s.name);
    titles[1].setAttribute('title', s.preview);
    const pill = row.querySelectorAll('[aria-label]')
                    .find(e => /unread/.test(e.attrs['aria-label'] || ''));
    if (s.badge && pill) pill.setAttribute('aria-label', s.badge + ' unread messages');
    else if (s.badge) row.append(el('div', { 'aria-label': s.badge + ' unread messages' }));
    else if (pill) pill.remove();
    row.attrs.__innerText = [s.name, s.when]
        .concat(s.sender ? [s.sender, ':'] : [])
        .concat([s.preview]).join('\n');
  };
  row.paint();
  return row;
};
const update = (row, patch) => { Object.assign(row.__spec, patch); row.paint(); };

/* ------------------------------------------------------------ the sandbox */

let pings = 0;                            // arrivals the page nudged the app about
const observers = [];
const logs = [];

const document = {
  querySelector: sel => root.querySelector(sel),
  querySelectorAll: sel => root.querySelectorAll(sel),
  addEventListener() {}, dispatchEvent() {},
  styleSheets: [], body: el('body', {}), activeElement: null,
};

const sandbox = {
  document, console,
  navigator: { userAgent: 'test' },
  location: { host: 'web.whatsapp.com' },
  setTimeout, clearTimeout,
  /* watchList is installed on an interval in the page; here it runs once and
     the observer it registers is driven by hand, one pass at a time. */
  setInterval: fn => { fn(); return 0; },
  MutationObserver: class {
    constructor(cb) { this.cb = cb; observers.push(this); }
    observe() {}
  },
  Event: class { constructor(type) { this.type = type; } },
  addEventListener() {}, dispatchEvent() {},
  webkit: { messageHandlers: {
    whatsapp:      { postMessage: m => logs.push(String(m)) },
    whatsappEvent: { postMessage: () => { pings++; } },
  } },
};
sandbox.window = sandbox;
vm.createContext(sandbox);
/* A movable Date.now, so the 2.5s the watcher waits for the list to settle does
   not have to be waited out. new Date() stays real: freshness compares a row's
   clock against the wall clock, and the rows above are stamped from it. */
vm.runInContext('var __offset = 0; const __real = Date.now; Date.now = () => __real() + __offset;',
                sandbox);
vm.runInContext(fs.readFileSync(SRC, 'utf8'), sandbox, { filename: 'inject.js' });

const advance = ms => { sandbox.__offset += ms; };
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
/* One debounced pass of the chat-list watcher, exactly as a DOM change drives
   it: the observer fires, and the scan lands 150ms later. */
const scan = async () => { observers.forEach(o => o.cb()); await sleep(200); };
/* What the app would put on a banner, in one line. */
const describe = async () => {
  const answer = await sandbox.window.__whatsappDescribeUnread();
  if (answer === 'open' || answer === '') return answer;
  const [chat, sender, message] = answer.split(US);
  return sender ? chat + ' | ' + sender + ': ' + message : chat + ' | ' + message;
};

/* -------------------------------------------------------------- the checks */

let failures = 0;
const check = (label, got, want) => {
  if (got === want) { console.log('  ok   ' + label); return; }
  failures++;
  console.log('  FAIL ' + label + '\n         got  ' + JSON.stringify(got) +
              '\n         want ' + JSON.stringify(want));
};

(async () => {
  console.log('driving ' + SRC + '\n');

  const mega = mkRow({ name: 'Mega', preview: 'ون', when: clock(), badge: 0, open: true });
  const pdf  = mkRow({ name: 'Pdf & Assignments', preview: 'تيست', when: clock(1),
                       badge: 1, sender: 'Mega' });
  const joo  = mkRow({ name: 'EL Joo', preview: 'انجز يلا', when: '12:28 AM', badge: 0 });
  pane.append(mega, pdf, joo);

  sandbox.window.__whatsappSetFocus(true);
  await scan();                                    // the opening pass seeds the list
  check('a chat left unread from before the client started is not news',
        await describe(), '');

  advance(3000);                                   // the list has settled
  await scan();
  check('and it is still not news a moment later', await describe(), '');

  /* A message to a chat that is not on screen, and then the extra ask that used
     to announce it a second time. The app asks more often than messages land --
     the document title asks on its own count -- and every ask the queue could
     not answer fell through to the topmost unread row. */
  update(pdf, { preview: 'تمام', when: clock(), badge: 2 });
  await scan();
  check('a message to a chat that is not on screen is announced',
        await describe(), 'Pdf & Assignments | Mega: تمام');
  check('the next ask, with nothing queued, stays quiet', await describe(), '');
  await scan();
  check('and stays quiet after another pass of the watcher', await describe(), '');

  update(mega, { preview: 'ز', when: clock() });
  await scan();
  check('a message in the chat on screen is left to WhatsApp', await describe(), 'open');
  check('reading one chat does not re-announce the other', await describe(), '');

  /* Typing. A direct chat leaves "typing..." in the preview, a group leaves
     "Mega is typing..." -- and the second shape used to read as a message. */
  const before = pings;
  update(pdf, { preview: 'Mega is typing...', sender: null });
  await scan();
  check('a group typing raises no arrival at all', pings - before, 0);
  check('and nothing is described for it', await describe(), '');
  update(pdf, { preview: 'تمام', sender: 'Mega' });   // typing stops, message unchanged
  await scan();
  check('typing stopping is not an arrival either', await describe(), '');

  /* And typing that starts in the quarter second between the message landing
     and the app asking about it. */
  update(pdf, { preview: 'يلا بينا', when: clock(), badge: 3, sender: 'Mega' });
  await scan();
  update(pdf, { preview: 'Mega is typing...', sender: null });
  check('a banner carries the message and its sender, never the typing',
        await describe(), 'Pdf & Assignments | Mega: يلا بينا');

  /* The one case the guess exists for: a chat too far down the list to have
     been rendered, moved to the top by a message the watcher never saw arrive. */
  update(pdf, { preview: 'يلا بينا', sender: 'Mega' });
  await scan();
  const deep = mkRow({ name: 'Communication Engineer 4', preview: 'تم',
                       when: clock(), badge: 1, sender: '~Mo farhat' });
  pane.append(deep);
  await scan();
  check('a row that appears at the top unread is still guessed at',
        await describe(), 'Communication Engineer 4 | ~Mo farhat: تم');
  check('but only once', await describe(), '');

  console.log(failures ? '\n' + failures + ' failed' : '\nall checks pass');
  process.exit(failures ? 1 : 0);
})();
