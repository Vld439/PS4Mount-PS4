// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 gasb07
// Web panel tests: the main page and the page for older firmwares (/legacy) in jsdom with a
// mock API, and syntax checks of their scripts. Run with: cd test/web && npm ci && npm test
const fs = require('fs');
const { JSDOM, VirtualConsole } = require('jsdom');
const W = require('path').join(__dirname, '..', '..', 'web') + '/';
const GAMES = [
  { title_id: 'CUSA00791', name: 'N++', version: '01.10', has_patch: true, size: 4e9, state: 'ready', mount: 'rw', source: '/mnt/usb0/ps4/CUSA00791', dlc: 0, update_issue: '' },
  { title_id: 'CUSA28732', name: 'Civ VII', version: '01.16', has_patch: true, size: 9e9, state: 'running', mount: 'rw', source: '/mnt/usb0/ps4/CUSA28732', dlc: 35, update_issue: '' },
  { title_id: 'CUSA00190', name: 'Killzone', version: '01.00', has_patch: false, size: 9e9, state: 'internal', mount: '', source: '/mnt/usb0/ps4/CUSA00190', dlc: 0, update_issue: '' },
  { title_id: 'CUSA41712', name: 'Soulstice', version: '01.03', has_patch: false, size: 9e9, state: 'ready', mount: 'rw', source: '/mnt/usb0/ps4/CUSA41712', dlc: 1, update_issue: '' },
  { title_id: 'CUSA34284', name: 'Moving Out 2', version: '01.04', game_version: '01.00', has_patch: true, size: 2.6e9, state: 'restart', mount: 'rw', source: '/mnt/usb0/ps4/CUSA34284', dlc: 0, update_issue: '', content_id: 'EP4064-CUSA34284_00-MOVINGOUT2000000', system_ver: '9.00', storage: 'usb0' },
  { title_id: 'CUSA00069', name: 'Rayman Legends', version: '', has_patch: false, size: 0, state: 'offline', mount: '', source: '/mnt/usb1/ps4/CUSA00069', dlc: 0, update_issue: '', content_id: '', game_version: '', system_ver: '', storage: 'usb1' }];
const CFG = { batch: true, batch_size: 5, dlc: true, restore_batch: 5, max_restore_batch: 10, paths: ['/'] };
let REPLY_LANG; // the language the console reports (undefined: none, English)
let GAMES_VERSION = 'PS4Mount test';
let DRIVE = { configured: false }; // GET /api/drive (an object: the tests change it)
let LINKS = { configured: false }; // GET /api/links
const DESTS = { dests: [{ id: 'usb0', label: 'usb0', folder: '/mnt/usb0/PS4Mount', free: 5e11, fits: true }] };
function reply(url) {
  if (url === '/api/drive') return DRIVE;
  if (url === '/api/links') return LINKS;
  if (url === '/api/dests') return DESTS;
  return url.indexOf('/api/log') === 0 ? { log: 'a\nb' } : url.indexOf('/api/config') === 0 ? CFG :
    url.indexOf('/api/games') === 0 ? { version: GAMES_VERSION, lang: REPLY_LANG, games: GAMES } : { ok: true, message: 'ok ' + url };
}
const tick = () => new Promise(r => setTimeout(r, 30));
const ok = (c, m) => { console.log((c ? 'ok   ' : 'FAIL ') + m); if (!c) process.exitCode = 1; };
function page(file, opts) {
  const reqs = [], vc = new VirtualConsole(), nav = [];
  vc.on('jsdomError', e => { if (/navigation/i.test(e.message)) nav.push(e.message); else console.log('jsdomError', e.message); });
  // as build.sh does: the language tables where the page says <!--LANG-->
  const html = fs.readFileSync(W + file, 'utf8').replace('<!--LANG-->', () => '<script>' + fs.readFileSync(W + 'lang.js', 'utf8') + '</script>');
  const dom = new JSDOM(html, {
    url: 'http://ps5:8770/', runScripts: 'dangerously', pretendToBeVisual: true, virtualConsole: vc,
    beforeParse(w) {
      w.setInterval = () => 0;
      w.scrollTo = () => {};
      w.confirm = () => (w.__confirm !== undefined ? w.__confirm : true);
      if (opts.dialog) {
        w.HTMLDialogElement.prototype.showModal = function () { this.setAttribute('open', ''); };
        w.HTMLDialogElement.prototype.close = function () { this.removeAttribute('open'); };
      }
      w.fetch = (url, o) => { reqs.push({ url, method: (o && o.method) || 'GET', body: o && o.body, headers: (o && o.headers) || {} }); return Promise.resolve({ ok: true, json: () => Promise.resolve(reply(url)) }); };
      w.XMLHttpRequest = function () {
        const x = this; x.readyState = 0;
        x.open = (m, u) => { x.m = m; x.u = u; };
        x.setRequestHeader = () => {};
        x.send = b => { reqs.push({ url: x.u, method: x.m, body: b }); setTimeout(() => { x.readyState = 4; x.status = 200; x.responseText = JSON.stringify(reply(x.u)); x.onreadystatechange(); }, 1); };
      };
    } });
  return { dom, w: dom.window, d: dom.window.document, reqs, nav };
}
const acorn = require('acorn');
function scripts(file) {
  // parsed as HTML (scripts not run), so the browser's own rules find the script elements
  const doc = new JSDOM(fs.readFileSync(W + file, 'utf8')).window.document;
  return Array.from(doc.querySelectorAll('script'), (el) => el.textContent);
}
function parses(code, version) {
  try { acorn.parse(code, { ecmaVersion: version }); return ''; } catch (e) { return e.message; }
}

// The value of a string argument: a literal, or literals joined with +.
function literal(node) {
  if (node.type === 'Literal' && typeof node.value === 'string') return node.value;
  if (node.type === 'BinaryExpression' && node.operator === '+') {
    const a = literal(node.left), b = literal(node.right);
    return a === null || b === null ? null : a + b;
  }
  return null;
}
// Every t('...') text of a page's scripts (the ones built from parts are listed apart).
function tKeys(file) {
  const keys = [], built = [];
  scripts(file).forEach((code) => {
    (function walk(n) {
      if (!n || typeof n.type !== 'string') return;
      if (n.type === 'CallExpression' && n.callee.type === 'Identifier' && n.callee.name === 't' && n.arguments.length) {
        const v = literal(n.arguments[0]);
        if (v === null) built.push(code.slice(n.start, n.end)); else keys.push(v);
      }
      for (const k in n) { const c = n[k]; if (Array.isArray(c)) c.forEach(walk); else if (c && typeof c.type === 'string') walk(c); }
    })(acorn.parse(code, { ecmaVersion: 2017 }));
  });
  return { keys, built };
}

(async () => {
  // the language tables: ES5 (the old browser reads them), every text of the pages translated
  { const e = parses(fs.readFileSync(W + 'lang.js', 'utf8'), 5); ok(!e, 'lang.js is ES5' + (e ? ': ' + e : '')); }
  const L = new JSDOM('<script>' + fs.readFileSync(W + 'lang.js', 'utf8') + '</script>', { runScripts: 'dangerously' }).window.LANGS;
  ['index.html', 'legacy.html'].forEach((f) => {
    const { keys, built } = tKeys(f);
    const missing = keys.filter((k) => !L.es[k]);
    ok(keys.length > 20 && !missing.length, f + ': every t() text has a Spanish one' + (missing.length ? ': ' + JSON.stringify(missing) : ''));
    ok(built.every((c) => /^t\(labels\[/.test(c) || /^t\(c\.kind\)/.test(c)), f + ': t() of a computed text only for states and pkg kinds: ' + built.join(' '));
  });
  ['Ready', 'Restart to play', 'Running', 'Not in library', 'Installed on console', 'Adding', 'On another drive', 'Drive not connected',
    'game', 'update', 'add-on', 'other'].forEach((k) => ok(!!L.es[k], 'Spanish for "' + k + '"'));
  Object.keys(L.es).forEach((k) => {
    // the same ones; a translation may use one twice ("de {0} en {0}")
    const set = (x) => [...new Set(x.match(/\{\d\}/g) || [])].sort().join(), a = set(k), b = set(L.es[k]);
    if (a !== b) ok(false, 'same placeholders in the translation of "' + k + '"');
  });

  // the browser of older firmwares: plain ES5 only
  scripts('legacy.html').forEach((code, i) => { const e = parses(code, 5); ok(!e, 'legacy script ' + i + ' is ES5' + (e ? ': ' + e : '')); });
  // the head script of the main page runs everywhere before the redirect: ES5 too
  const main = scripts('index.html');
  ok(main.length === 2, 'index has the redirect script and the panel script');
  { const e = parses(main[0], 5); ok(!e, 'index redirect script is ES5' + (e ? ': ' + e : '')); }
  { const e = parses(main[1], 2017); ok(!e, 'index panel script is ES2017' + (e ? ': ' + e : '')); }

  // --- modern page, old browser: sent to /legacy
  let p = page('index.html', { dialog: false });
  await tick();
  ok(p.nav.length > 0, 'index without <dialog> navigates away (to /legacy)');
  ok(fs.readFileSync(W + 'index.html', 'utf8').includes("location.replace('/legacy')"), 'redirect target is /legacy');

  // --- modern page
  p = page('index.html', { dialog: true });
  let { w, d, reqs } = p;
  await tick();
  ok(p.nav.length === 0, 'index with <dialog> stays');
  ok(d.querySelectorAll('.card').length === 6, 'six game cards');
  ok(d.querySelector('.card .name').textContent === 'Civ VII', 'cards sorted by name');
  { const c = [...d.querySelectorAll('.card')].find(x => x.textContent.includes('Moving Out 2'));
    ok(c && c.querySelector('.badge').textContent === 'Restart to play' && c.textContent.includes('Restart the console once'), 'a game added in this boot says to restart');
    ok(c && c.querySelector('[data-a="unmount"]') !== null, 'it can be removed from the library'); }
  { const c = [...d.querySelectorAll('.card')].find(x => x.textContent.includes('Rayman Legends'));
    ok(c && c.querySelector('.badge').textContent === 'Drive not connected' && c.textContent.includes('On usb1, which is not connected'), 'a game of a drive that is away says so');
    ok(c && !c.textContent.includes(' · v ·') && !c.querySelector('button[data-a]'), 'no empty version, nothing to do with it'); }
  d.querySelector('.summary span[data-f="restart"]').click();
  ok(d.querySelectorAll('.card').length === 1, 'the state chips filter the cards');
  d.querySelector('.summary span[data-f=""]').click();
  ok(d.querySelectorAll('.card').length === 6, 'all games again');
  d.querySelector('.art[data-g="CUSA34284"]').click(); await tick();
  ok(d.getElementById('gamedlg').hasAttribute('open') && d.getElementById('g-name').textContent === 'Moving Out 2', 'a cover opens the game');
  ok(d.getElementById('g-hero').style.backgroundImage.includes('/pic/CUSA34284'), 'with its background art');
  { const t = d.getElementById('g-facts').textContent;
    ok(t.includes('EP4064-CUSA34284_00-MOVINGOUT2000000') && t.includes('game v01.00 with its update') && t.includes('9.00'), 'details: content id, versions, firmware'); }
  ok(d.querySelector('#g-actions [data-a="unmount"]') !== null, 'and its actions');
  d.getElementById('gamedlg').removeAttribute('open');
  ok(d.querySelector('#grid [data-a="restore"]') === null, 'restoring original files is not on the cards');
  w.showGame('CUSA28732');
  ok(d.querySelector('#g-actions [data-a="restore"]') !== null && d.getElementById('g-actions').textContent.includes('Restore original files'), 'a game page offers restoring its original files');
  w.showGame('CUSA00190');
  ok(d.querySelector('#g-actions [data-a="restore"]') === null, 'not for a game installed on the console');
  d.getElementById('gamedlg').removeAttribute('open');
  d.getElementById('settingsbtn').click(); await tick();
  ok(d.getElementById('setdlg').hasAttribute('open') && d.getElementById('s-rsize').value === '5', 'settings open with restore group 5');
  const rs = d.getElementById('s-rsize');
  rs.value = '12'; rs.dispatchEvent(new w.Event('input')); ok(rs.value === '10', 'restore group 12 capped to 10');
  const gs = d.getElementById('s-size');
  gs.value = '99'; gs.dispatchEvent(new w.Event('input')); ok(gs.value === '50', 'group size capped to 50');
  const kp = (el, ch) => { const e = new w.KeyboardEvent('keypress', { key: ch, charCode: ch.charCodeAt(0), cancelable: true }); el.dispatchEvent(e); return e.defaultPrevented; };
  ok(kp(gs, 'a') && kp(gs, '-') && kp(gs, 'e') && !kp(gs, '7'), 'group size: keys other than digits are blocked');
  ok(gs.getAttribute('inputmode') === 'numeric' && d.getElementById('s-rsize').getAttribute('inputmode') === 'numeric', 'numeric keyboard on both fields');
  ok(gs.type === 'number' && d.getElementById('s-rsize').type === 'number', 'both fields are type=number (numeric keyboard on the PS5)');
  gs.value = ''; reqs.length = 0; d.getElementById('setform').dispatchEvent(new w.Event('submit', { cancelable: true })); await tick();
  ok(reqs.length === 0 && d.getElementById('toast').textContent.includes('1 to 50'), 'empty group size refused');
  gs.value = '5';
  rs.value = '0'; reqs.length = 0; d.getElementById('s-restore').click(); await tick();
  ok(reqs.length === 0 && d.getElementById('toast').textContent.includes('1 to 10'), 'group 0 refused');
  rs.value = '3'; w.__confirm = false; d.getElementById('s-restore').click(); await tick();
  ok(reqs.length === 0, 'move all cancelled at the warning sends nothing');
  w.__confirm = true; d.getElementById('s-restore').click(); await tick();
  ok(reqs.length === 2 && reqs[0].url === '/api/config' && reqs[0].body === 'restore_batch=3', 'move all saves group 3 first');
  ok(reqs[1].url === '/api/restore' && reqs[1].method === 'POST' && reqs[1].body === undefined, 'then restores all (no body)');
  ok(!d.getElementById('setdlg').hasAttribute('open'), 'settings closed after starting');
  d.getElementById('settingsbtn').click(); await tick();
  rs.value = '1';
  d.getElementById('s-pick').click(); await tick();
  const rows = d.querySelectorAll('#p-rows tr');
  ok(d.getElementById('pickdlg').hasAttribute('open') && rows.length === 4, 'picker lists 4 games (internal left out)');
  ok(rows[1].textContent.includes('Game, update and 35 add-ons'), 'row says game, update and add-ons are moved back');
  ok(d.querySelector('#pickdlg .note').textContent.includes('all its add-ons'), 'picker note about add-ons');
  ok(d.getElementById('p-go').disabled, 'nothing selected: button disabled');
  const bx = d.querySelectorAll('#p-rows input');
  bx[0].checked = true; bx[2].checked = true; d.getElementById('p-rows').dispatchEvent(new w.Event('change', { bubbles: true }));
  ok(!d.getElementById('p-go').disabled && d.getElementById('p-info').textContent.includes('2 game(s) selected, removed from the library 1 at a time'), 'two selected, groups of 1 shown');
  reqs.length = 0; d.getElementById('p-go').click(); await tick();
  ok(reqs[0].body === 'restore_batch=1' && reqs[1].url === '/api/restore' && reqs[1].body === 'title_ids=CUSA00791,CUSA41712', 'selected restore sends the two title ids');
  ok(!d.getElementById('pickdlg').hasAttribute('open'), 'picker closed after starting');
  d.getElementById('settingsbtn').click(); await tick();
  d.getElementById('s-pick').click(); await tick();
  d.getElementById('p-all').checked = true; d.getElementById('p-all').dispatchEvent(new w.Event('change'));
  ok([...d.querySelectorAll('#p-rows input')].every(b => b.checked), 'select all checks every row');
  reqs.length = 0; rs.value = '7';
  d.getElementById('setform').dispatchEvent(new w.Event('submit', { cancelable: true })); await tick();
  ok(reqs[0] && reqs[0].url === '/api/config' && reqs[0].body.includes('restore_batch=7'), 'Save sends restore_batch');

  // --- sending pkgs: checked first, only usable ones sent, to the chosen place
  p = page('index.html', { dialog: true }); ({ w, d, reqs } = p);
  await tick();
  const sent = [], checks = [];
  let hold = false; // the next upload stays in flight until cancelled
  const DESTS = [{ id: 'usb0', label: 'usb0', folder: '/mnt/usb0/pmscan', free: 67108864 + 1500, fits: true },
    { id: 'internal', label: 'internal storage', folder: '/data/pkg', free: 1e12, fits: true }];
  w.fetch = (url, o) => {
    if (url.indexOf('/api/check') === 0) {
      checks.push({ url, headers: (o && o.headers) || {} });
      const size = +url.split('size=')[1];
      const j = size === 1100 ? { ok: false, problem: 'needs firmware 14.00 or newer; this console has 13.52', kind: 'game', title_id: 'CUSA00002', dests: DESTS }
        : { ok: true, problem: '', kind: size === 2000 || size === 2100 ? 'update' : 'game', title: 'Game ' + size, title_id: 'CUSA00001', app_ver: '01.00', notes: [], dests: DESTS };
      return Promise.resolve({ ok: true, json: () => Promise.resolve(j) });
    }
    return Promise.resolve({ ok: true, json: () => Promise.resolve(reply(url)) });
  };
  w.XMLHttpRequest = function () {
    const x = this, hdr = {};
    x.upload = {};
    x.open = (m, u) => { x.m = m; x.u = u; };
    x.setRequestHeader = (k, v) => { hdr[k] = v; };
    x.send = (b) => {
      sent.push({ url: x.u, method: x.m, headers: hdr, size: b.size });
      if (hold) return;
      setTimeout(() => { if (x.upload.onprogress) x.upload.onprogress({ loaded: b.size / 2 }); }, 1);
      setTimeout(() => { x.status = 200; x.responseText = JSON.stringify({ ok: true, message: 'received' }); x.onload(); }, 5);
    };
    x.abort = () => x.onabort();
  };
  ok(!d.getElementById('sendbtn').hidden, 'Send games is offered where the browser can read files');
  d.getElementById('sendbtn').click();
  ok(d.getElementById('senddlg').hasAttribute('open'), 'send dialog opens');
  const file = (name, n) => new w.File([new Uint8Array(n)], name);
  d.getElementById('drop').ondrop({ preventDefault() {}, dataTransfer: { files: [file('Good.pkg', 1000), file('New.pkg', 1100), file('Upd.pkg', 2000)] } });
  await tick(); await tick();
  ok(checks.length === 3 && checks.every(c => c.headers['X-PS4Mount'] === '1'), 'each pkg is checked first, with the panel header');
  ok(checks[0].url === '/api/check?size=1000', 'the check says the size of the whole file');
  let items = d.querySelectorAll('#queue .item');
  ok(items.length === 3, 'three pkgs in the list');
  ok(items[0].textContent.includes('Ready to send') && items[0].textContent.includes('Game 1000'), 'a good pkg is ready, with its title');
  ok(items[1].textContent.includes('Cannot be used') && items[1].textContent.includes('needs firmware 14.00'), 'a pkg for a newer firmware is refused with the reason');
  ok(items[2].textContent.includes('not enough space in usb0'), 'a pkg that does not fit the chosen drive is refused');
  ok(d.getElementById('dest').options.length === 2 && d.getElementById('dest').value === 'usb0', 'the places come from the check, first one chosen');
  d.getElementById('dest').value = 'internal'; d.getElementById('dest').onchange(); await tick();
  items = d.querySelectorAll('#queue .item');
  ok(items[2].textContent.includes('Ready to send'), 'it fits the console storage');
  ok(!d.getElementById('send-go').disabled, 'Send is enabled');
  d.getElementById('send-go').click();
  for (let i = 0; i < 10; i++) await tick();
  ok(sent.length === 2, 'only the two usable pkgs are sent');
  ok(sent[0].url === '/api/upload?dest=internal&name=Good.pkg' && sent[0].method === 'POST' && sent[0].headers['X-PS4Mount'] === '1', 'upload goes to the chosen place, with the panel header');
  ok(sent[1].url.includes('name=Upd.pkg') && sent[1].size === 2000, 'then the next one, the whole file');
  items = d.querySelectorAll('#queue .item');
  ok(items[0].textContent.includes('Sent') && items[2].textContent.includes('Sent') && items[1].textContent.includes('Cannot be used'), 'both sent, the refused one stays refused');
  ok(d.getElementById('send-go').disabled && d.getElementById('send-cancel').hidden, 'nothing left to send');
  // an update dropped before its game still goes after it; a cancelled one can be sent again
  sent.length = 0;
  d.getElementById('drop').ondrop({ preventDefault() {}, dataTransfer: { files: [file('Upd2.pkg', 2100), file('Game2.pkg', 3000)] } });
  await tick(); await tick();
  hold = true;
  d.getElementById('send-go').click(); await tick();
  ok(sent.length === 1 && sent[0].url.includes('name=Game2.pkg'), 'the game is sent before the update dropped first');
  d.getElementById('send-cancel').click(); await tick();
  items = d.querySelectorAll('#queue .item');
  ok(items[4].textContent.includes('Failed') && items[4].textContent.includes('cancelled'), 'a cancelled send says so');
  ok(sent.length === 1, 'cancelling does not start the next one (its update)');
  const again = items[4].querySelector('button[data-again]');
  ok(again && !items[3].querySelector('button[data-again]'), 'Send again is offered for the failed one only');
  hold = false; checks.length = 0;
  again.click(); await tick(); await tick();
  ok(checks.length === 1 && checks[0].url === '/api/check?size=3000', 'sending again checks the pkg again');
  d.getElementById('send-go').click();
  for (let i = 0; i < 10; i++) await tick();
  ok(sent.length === 3 && sent[1].url.includes('name=Game2.pkg') && sent[2].url.includes('name=Upd2.pkg'), 'then the game, then its update');
  // the settings have the internal storage switch
  d.getElementById('settingsbtn').click(); await tick();
  ok(d.getElementById('s-internal') !== null, 'settings have the /data/pkg switch');

  // a new PS4Mount loaded while the page is open: the page reloads itself
  {
    let reloaded = false;
    const q = page('index.html', { dialog: true });
    await tick();
    try { Object.defineProperty(q.w.location, 'reload', { value: () => { reloaded = true; } }); } catch (e) { q.w.location.reload = () => { reloaded = true; }; }
    q.w.load(); await tick();
    ok(!reloaded, 'the same version: no reload');
    const old = GAMES_VERSION; GAMES_VERSION = 'PS4Mount 9.9.9';
    q.w.load(); await tick();
    GAMES_VERSION = old;
    ok(reloaded || q.nav.length > 0, 'another version: the page reloads');
  }

  // --- Google Drive
  ok(d.getElementById('drivebtn').hidden, 'no Drive button when this build has no Drive');
  DRIVE = { configured: true, client: false, client_id: '', connected: false, login: { active: false, error: '' }, job: { busy: false, queued: [] }, games: [] };
  p = page('index.html', { dialog: true }); ({ w, d, reqs } = p);
  await tick(); await tick();
  ok(!d.getElementById('drivebtn').hidden && d.getElementById('drivelabel').textContent === 'Connect Google Drive', 'the corner button: Connect Google Drive');
  ok(d.getElementById('backupbtn').hidden, 'no backup button before connecting');
  d.getElementById('drivebtn').click(); await tick();
  ok(d.getElementById('drivedlg').open && !d.getElementById('d-setup').hidden && d.getElementById('d-connect').hidden, 'no client yet: the steps to make one, no Connect');
  ok(d.querySelector('#d-setup a').href === 'https://console.cloud.google.com/', 'with the link to Google Cloud');
  ok(d.getElementById('d-csecret').type === 'password', 'the secret is typed hidden');
  reqs.length = 0;
  d.getElementById('d-cid').value = ' 123-abc.apps.googleusercontent.com ';
  d.getElementById('d-csecret').value = 'GOCSPX-a+b';
  d.getElementById('d-setup').dispatchEvent(new w.Event('submit', { cancelable: true })); await tick();
  const cl = reqs.find((r) => r.url === '/api/drive/client');
  ok(cl && cl.method === 'POST' && cl.headers['X-PS4Mount'] === '1' &&
    cl.body === 'client_id=123-abc.apps.googleusercontent.com&client_secret=GOCSPX-a%2Bb', 'the client goes in the body, with the panel header');
  ok(!reqs.some((r) => /GOCSPX/.test(r.url)), 'never in an address');
  DRIVE = Object.assign({}, DRIVE, { client: true, client_id: '123-abc.apps.googleusercontent.com' });
  w.loadDrive(); await tick();
  ok(d.getElementById('d-setup').hidden && !d.getElementById('d-connect').hidden && d.getElementById('d-client-id').textContent.includes('123-abc'), 'with a client: Connect, and which client');
  ok(w.getComputedStyle(d.getElementById('d-setup')).display === 'none', 'and the form is really out of sight (a display rule does not win over hidden)');
  d.getElementById('d-cchange').click();
  ok(!d.getElementById('d-setup').hidden && d.getElementById('d-cid').value === '123-abc.apps.googleusercontent.com' && d.getElementById('d-csecret').value === '', 'Change: the form again, the secret not shown');
  w.renderDrive.editing = false; w.renderDrive();
  reqs.length = 0;
  d.getElementById('d-connect').click(); await tick();
  const conn = reqs.find((r) => r.url === '/api/drive/connect');
  ok(conn && conn.method === 'POST' && conn.headers['X-PS4Mount'] === '1', 'Connect posts with the panel header');
  DRIVE = Object.assign({}, DRIVE, { login: { active: true, code: 'ABCD-EFGH', url: 'https://www.google.com/device', expires: 1700, error: '' } });
  w.loadDrive(); await tick();
  ok(d.getElementById('d-usercode').textContent === 'ABCD-EFGH' && d.getElementById('d-url').href === 'https://www.google.com/device', 'the code and the address to type it in');
  ok(!d.getElementById('d-code').hidden && d.getElementById('d-connect').disabled, 'waiting for the account: Connect is off');
  DRIVE = { configured: true, client: true, client_id: '123-abc.apps.googleusercontent.com', connected: true, email: 'someone@example.com', quota_limit: 16e9, quota_used: 4e9, login: { active: false, error: '' },
    job: { busy: false, queued: [], result: '', ok: true }, listed: true, list_error: '',
    games: [{ title_id: 'CUSA00791', name: 'N++', size: 4e9, files: 2, update: true, addons: 0 },
      { title_id: 'CUSA99999', name: 'Only In Drive', size: 3e9, files: 1, update: false, addons: 2 }] };
  w.loadDrive(); await tick(); await tick();
  ok(!d.getElementById('d-on').hidden && d.getElementById('d-email').textContent === 'someone@example.com', 'connected: the account');
  ok(d.getElementById('d-quota').style.width === '25%', 'and its space used');
  ok(!d.getElementById('backupbtn').hidden && d.getElementById('drivelabel').textContent === 'Google Drive', 'connected: a backup button appears');
  let drows = d.querySelectorAll('#d-rows tr');
  ok(drows.length === 2 && drows[0].textContent.includes('N++') && drows[0].textContent.includes('on the console'), 'the games in Drive, the ones on the console marked');
  ok(!drows[0].querySelector('[data-dl]') && drows[1].querySelector('[data-dl]'), 'Download only for a game not on the console');
  ok(drows[1].textContent.includes('2 add-ons'), 'with its add-ons');
  ok(d.querySelector('.art[data-g="CUSA00791"] .b-drive') && !d.querySelector('.art[data-g="CUSA41712"] .b-drive'), 'cards of games in Drive say so');
  w.loadDests(); await tick();
  reqs.length = 0;
  drows[1].querySelector('[data-dl]').click(); await tick();
  const dl = reqs.find((r) => r.url.indexOf('/api/drive/download') === 0);
  ok(dl && dl.url === '/api/drive/download?title_id=CUSA99999&dest=usb0' && dl.headers['X-PS4Mount'] === '1', 'Download goes to the chosen drive');
  reqs.length = 0; w.__confirm = false;
  drows[1].querySelector('[data-tr]').click(); await tick();
  ok(!reqs.some((r) => r.url.indexOf('/api/drive/trash') === 0), 'Delete from Drive asks first (and a no stops it)');
  w.__confirm = true;
  drows[1].querySelector('[data-tr]').click(); await tick();
  ok(reqs.some((r) => r.url === '/api/drive/trash?title_id=CUSA99999'), 'then moves it to the Drive trash');
  reqs.length = 0;
  d.getElementById('backupbtn').click(); await tick();
  ok(reqs.some((r) => r.url === '/api/drive/upload' && r.method === 'POST'), 'Back up to Drive: the whole library');
  d.getElementById('drivedlg').close();
  w.showGame('CUSA41712');
  const up = d.querySelector('#g-actions [data-d="upload"]');
  ok(up && up.textContent === 'Send to Drive', 'the game page offers Send to Drive');
  ok(d.getElementById('g-facts').textContent.includes('not in Drive'), 'and says it is not in Drive');
  reqs.length = 0;
  up.click(); await tick();
  ok(reqs.some((r) => r.url === '/api/drive/upload?title_id=CUSA41712' && r.headers['X-PS4Mount'] === '1'), 'Send to Drive uploads that game');
  w.showGame('CUSA00791');
  ok(d.querySelector('#g-actions [data-d="upload"]').textContent === 'Send to Drive again' && d.getElementById('g-facts').textContent.includes('in Drive (3.7 GB)'), 'a game already there: Send to Drive again');
  w.showGame('CUSA00190');
  ok(!d.querySelector('#g-actions [data-d="upload"]'), 'not for a game installed the normal way');
  d.getElementById('gamedlg').close();
  DRIVE = Object.assign({}, DRIVE, { job: { busy: true, op: 'upload', title_id: 'CUSA41712', name: 'Soulstice', file: 'game.pkg', file_index: 1, files: 2, done: 5e8, total: 1e9,
    game_done: 5e8, game_total: 2e9, speed: 2.8e6, elapsed: 600, phase: 'sending', retry_in: 0, queued: [{ op: 'upload', title_id: 'CUSA00791' }] } });
  w.loadDrive(); await tick();
  let dbt = d.getElementById('db-text').textContent, dbs = d.getElementById('db-sub').textContent;
  ok(d.getElementById('drivebar').style.display === 'flex' && dbt.includes('Uploading Soulstice to Drive') && dbt.includes('25%') && dbt.includes('of 1.9 GB'), 'the whole game in progress, under the header: ' + dbt);
  ok(dbt.includes('2.7 MB/s') && dbt.includes('~9 min left'), 'with its speed (a decimal below 10 MB/s) and the time left: ' + dbt);
  w.showGame('CUSA41712');
  ok(d.getElementById('g-facts').textContent.includes('uploading to Drive (25%)') && d.querySelector('#g-actions button[disabled]') &&
    !d.querySelector('#g-actions [data-d="upload"]'), 'the page of the game being uploaded says so, no second Send to Drive');
  w.showGame('CUSA00791');
  ok(d.getElementById('g-facts').textContent.includes('waiting to go to Drive'), 'a queued game says it waits');
  d.getElementById('gamedlg').close();
  ok(dbs.includes('game.pkg (1/2)') && dbs.includes('running for 10 min') && dbs.includes('1 more waiting'), 'the file, the time so far and the queue: ' + dbs);
  DRIVE.job = Object.assign({}, DRIVE.job, { phase: 'retrying', retry_in: 8 });
  w.loadDrive(); await tick();
  ok(d.getElementById('db-text').textContent.includes('trying again in 8 s'), 'a lost connection says when it tries again');
  DRIVE.job = Object.assign({}, DRIVE.job, { phase: 'sending', speed: 0 });
  w.loadDrive(); await tick();
  ok(d.getElementById('db-text').textContent.includes('measuring the speed'), 'no time left before the speed is known');
  DRIVE = Object.assign({}, DRIVE, { login: { active: true, code: 'WXYZ-1234', url: 'https://www.google.com/device', error: '' },
    job: { busy: false, queued: [{ op: 'upload', title_id: 'CUSA41712' }] } });
  w.loadDrive(); await tick();
  dbt = d.getElementById('db-text').textContent;
  ok(d.getElementById('drivebar').style.display === 'flex' && dbt.includes('WXYZ-1234') && dbt.includes('google.com/device'), 'an ended access: the new code where everyone looks');
  DRIVE = Object.assign({}, DRIVE, { login: { active: false }, job: { busy: true, op: 'upload', title_id: 'CUSA41712', name: 'Soulstice', game_done: 1, game_total: 2, total: 2, done: 1, speed: 1, phase: 'sending', queued: [] } });
  w.loadDrive(); await tick();
  reqs.length = 0;
  d.getElementById('db-cancel').click(); await tick();
  ok(reqs.some((r) => r.url === '/api/drive/cancel'), 'and it can be cancelled');
  DRIVE = { configured: false };

  // --- downloads from links
  ok(d.getElementById('linkbtn').hidden, 'no Download links button when this build has none');
  LINKS = { configured: true, jobs: [], job: { busy: false }, result: '', ok: true };
  p = page('index.html', { dialog: true }); ({ w, d, reqs } = p);
  await tick(); await tick();
  ok(!d.getElementById('linkbtn').hidden && d.getElementById('linkbtn').textContent === 'Download links', 'the Download links button');
  d.getElementById('linkbtn').click(); await tick(); await tick();
  ok(d.getElementById('linkdlg').open && d.querySelector('#l-dest option').value === 'usb0', 'its window, with the drives to save to');
  reqs.length = 0;
  d.getElementById('l-form').dispatchEvent(new w.Event('submit', { cancelable: true })); await tick();
  ok(!reqs.some((r) => r.url.indexOf('/api/links/') === 0), 'nothing pasted: nothing sent');
  const pasted = 'https://example.com/game.part1.rar\nhttps://example.com/game.part2.rar\nhttps://example.com/f?id=1 | x.pkg';
  d.getElementById('l-text').value = pasted;
  d.getElementById('l-form').dispatchEvent(new w.Event('submit', { cancelable: true })); await tick(); await tick();
  const add = reqs.find((r) => r.url.indexOf('/api/links/add') === 0);
  ok(add && add.url === '/api/links/add?dest=usb0' && add.method === 'POST' && add.headers['X-PS4Mount'] === '1' && add.body === pasted,
    'the links go in the body as pasted, to the chosen drive, with the panel header');
  ok(d.getElementById('l-text').value === '', 'and the box is emptied once they are taken');
  LINKS = { configured: true, result: 'Big Game was downloaded: 2 pkg(s), 40.0 GB in ~1 h', ok: true, job: { busy: false },
    jobs: [{ id: 4, name: 'Big Game', dest: 'usb0', files: 12, state: 'queued', error: '' },
      { id: 5, name: 'Other', dest: 'internal', files: 1, state: 'failed', error: 'the archive has a password: PS4Mount cannot open archives with a password' }] };
  w.loadLinks(); await tick();
  let lrows = d.querySelectorAll('#l-rows tr');
  ok(lrows.length === 2 && lrows[0].textContent.includes('12 parts') && lrows[0].textContent.includes('waiting'), 'the list: a download waiting, with its parts');
  ok(lrows[1].querySelector('.bad').textContent.includes('password') && lrows[1].querySelector('[data-retry]'), 'a failed one says why, and can be tried again');
  ok(!d.getElementById('l-result').hidden && d.getElementById('l-result').textContent.includes('Big Game was downloaded'), 'the last result');
  reqs.length = 0;
  lrows[1].querySelector('[data-retry]').click(); await tick();
  ok(reqs.some((r) => r.url === '/api/links/retry?id=5' && r.headers['X-PS4Mount'] === '1'), 'Try again');
  reqs.length = 0; w.__confirm = false;
  lrows[0].querySelector('[data-drop]').click(); await tick();
  ok(!reqs.some((r) => r.url.indexOf('/api/links/cancel') === 0), 'Remove asks first (and a no stops it)');
  w.__confirm = true;
  lrows[0].querySelector('[data-drop]').click(); await tick();
  ok(reqs.some((r) => r.url === '/api/links/cancel?id=4'), 'then removes it');
  LINKS = { configured: true, result: '', ok: true, jobs: [{ id: 4, name: 'Big Game', dest: 'usb0', files: 12, state: 'running', error: '' },
    { id: 6, name: 'Next', dest: 'usb0', files: 1, state: 'queued', error: '' }],
    job: { busy: true, id: 4, name: 'Big Game', file: 'game.part02.rar', pkg: 'game.pkg', file_index: 2, files: 12, done: 1073741824, total: 4294967296,
      speed: 12582912, elapsed: 120, retry_in: 0, phase: 'receiving' } };
  w.loadLinks(); await tick();
  let lbt = d.getElementById('lb-text').textContent, lbs = d.getElementById('lb-sub').textContent;
  ok(d.getElementById('linkbar').style.display === 'flex' && lbt.includes('Downloading Big Game') && lbt.includes('25%') && lbt.includes('12 MB/s') && lbt.includes('~4 min left'),
    'the bar under the header: ' + lbt);
  ok(lbs.includes('game.part02.rar (2/12)') && lbs.includes('extracting game.pkg') && lbs.includes('1 more waiting'), 'the part, the pkg coming out of it and the queue: ' + lbs);
  ok(d.querySelector('#l-rows tr [data-drop]').textContent === 'Cancel', 'the running one is cancelled, not removed');
  LINKS.job = Object.assign({}, LINKS.job, { phase: 'probing' });
  w.loadLinks(); await tick();
  ok(d.getElementById('lb-text').textContent === 'Checking the links of Big Game…', 'first it checks the links');
  LINKS.job = Object.assign({}, LINKS.job, { phase: 'extracting', speed: 0 });
  w.loadLinks(); await tick();
  ok(d.getElementById('lb-text').textContent.includes('Extracting Big Game'), 'then extracts what is left');
  reqs.length = 0;
  d.getElementById('lb-cancel').click(); await tick();
  ok(reqs.some((r) => r.url === '/api/links/cancel?id=4'), 'the bar cancels the download');
  LINKS = { configured: false };

  // --- in Spanish: the console says "es"
  REPLY_LANG = 'es';
  p = page('index.html', { dialog: true }); ({ w, d, reqs } = p);
  await tick();
  ok(d.documentElement.lang === 'es', 'the page takes the language the console reports');
  ok(d.getElementById('rescan').textContent === 'Buscar juegos' && d.getElementById('settingsbtn').textContent === 'Ajustes', 'fixed texts in Spanish');
  ok(d.getElementById('q').placeholder === 'Buscar juego o CUSA', 'placeholders in Spanish');
  { const c = [...d.querySelectorAll('.card')].find(x => x.textContent.includes('Moving Out 2'));
    ok(c && c.querySelector('.badge').textContent === 'Reinicia para jugar' && c.textContent.includes('Reinicia la consola una vez'), 'states and notes in Spanish');
    ok(c && c.querySelector('[data-a="unmount"]').textContent === 'Quitar de la biblioteca', 'buttons in Spanish'); }
  ok(d.getElementById('summary').textContent.includes('6 juego(s)'), 'the summary in Spanish');
  { const c = [...d.querySelectorAll('.card')].find(x => x.textContent.includes('Rayman'));
    ok(c && c.textContent.includes('En usb1, que no está conectado'), 'a value inside a translated text'); }
  w.showGame('CUSA34284');
  ok(d.getElementById('g-facts').textContent.includes('Necesita firmware') && d.getElementById('g-facts').textContent.includes('v01.04 (juego v01.00 con su actualización)'), 'the game page in Spanish');
  d.getElementById('settingsbtn').click(); await tick();
  ok(d.getElementById('s-lang').value === 'auto' && d.getElementById('s-lang').options[0].textContent === 'El de la consola', 'the language setting, in Spanish');
  d.getElementById('s-lang').value = 'en'; reqs.length = 0;
  d.getElementById('setform').dispatchEvent(new w.Event('submit', { cancelable: true })); await tick();
  ok(reqs.some(r => r.url === '/api/config' && /(^|&)lang=en(&|$)/.test(r.body)), 'the chosen language is saved');
  REPLY_LANG = 'en'; w.load(); await tick(); await tick();
  ok(d.getElementById('rescan').textContent === 'Scan for games' && d.documentElement.lang === 'en', 'and the page switches back to English');
  REPLY_LANG = 'fr';
  p = page('index.html', { dialog: true }); ({ w, d, reqs } = p);
  await tick();
  ok(d.getElementById('rescan').textContent === 'Scan for games', 'a language with no table: English');
  REPLY_LANG = 'es';
  p = page('legacy.html', {}); ({ w, d, reqs } = p);
  await tick();
  ok(d.getElementById('rescan').textContent === 'Buscar juegos' && d.getElementById('summary').textContent.includes('Listo'), 'the legacy page in Spanish');
  // no fixed text of either page left in English
  ['index.html', 'legacy.html'].forEach((f) => {
    const html = fs.readFileSync(W + f, 'utf8').replace(/<script>[\s\S]*?<\/script>/g, '')
      .replace('<!--LANG-->', () => '<script>' + fs.readFileSync(W + 'lang.js', 'utf8') + '</script><script>setLang("es")</script>');
    const doc = new JSDOM(html, { runScripts: 'dangerously' }).window.document, walker = doc.createTreeWalker(doc.body, 4), left = [];
    let n;
    while ((n = walker.nextNode())) {
      if (/^(SCRIPT|STYLE|PRE|TEXTAREA|OPTION)$/.test(n.parentNode.nodeName)) continue;
      const k = n.nodeValue.replace(/\s+/g, ' ').trim();
      if (k && /[a-z]{3}/.test(k) && !n.psmEn && !/^(PS4Mount|Log|\.pkg)$/.test(k)) left.push(k);
    }
    ok(!left.length, f + ': every fixed text in Spanish' + (left.length ? ': ' + JSON.stringify(left) : ''));
  });
  REPLY_LANG = undefined;

  // --- legacy page
  p = page('legacy.html', {}); ({ w, d, reqs } = p);
  await tick();
  ok(d.querySelectorAll('.card').length === 6 && d.getElementById('ver').textContent === 'PS4Mount test', 'legacy lists the games');
  ok(d.body.textContent.includes('Restart to play') && d.body.textContent.includes('Restart the console once'), 'legacy shows the restart state');
  ok(!/=>|\blet\b|\bconst\b|`|fetch\(|showModal|<dialog/.test(fs.readFileSync(W + 'legacy.html', 'utf8').split('<script>')[1]), 'legacy script is ES5 without fetch or dialog');
  d.getElementById('nav-settings').click(); await tick();
  ok(d.getElementById('v-settings').className.includes('on') && d.getElementById('s-rsize').value === '5', 'legacy settings page with group 5');
  const lr = d.getElementById('s-rsize');
  lr.value = '15'; lr.onkeyup(); ok(lr.value === '10', 'legacy caps the restore group to 10');
  const lg = d.getElementById('s-size');
  ok(lg.onkeypress({ charCode: 98, preventDefault() {} }) === false && lg.onkeypress({ charCode: 52 }) !== false, 'legacy blocks a letter key, lets a digit through');
  ok(lg.getAttribute('inputmode') === 'numeric' && d.getElementById('s-rsize').getAttribute('inputmode') === 'numeric', 'legacy numeric keyboard on both fields');
  ok(lg.type === 'number' && d.getElementById('s-rsize').type === 'number', 'legacy fields are type=number');
  lg.value = '0'; reqs.length = 0; d.getElementById('s-save').click(); await tick();
  ok(reqs.length === 0 && d.body.textContent.includes('1 to 50'), 'legacy group size 0 refused');
  lg.value = '5';
  lr.value = '2'; reqs.length = 0; d.getElementById('s-restore').click(); await tick();
  ok(reqs[0].body === 'restore_batch=2' && reqs[1].url === '/api/restore' && reqs[1].body === '', 'legacy move all: group 2 then restore all');
  d.getElementById('nav-settings').click(); await tick();
  d.getElementById('s-rsize').value = '1';
  d.getElementById('s-pick').click(); await tick();
  ok(d.getElementById('v-pick').className.includes('on') && d.querySelectorAll('#p-rows tr').length === 4, 'legacy picker page with 4 games');
  const lb = d.querySelectorAll('#p-rows input'); lb[1].click();
  lb[2].click();
  ok(d.getElementById('p-info').textContent.includes('2 game(s) selected, removed from the library 1 at a time'), 'legacy picker counts and groups');
  reqs.length = 0; d.getElementById('p-go').click(); await tick();
  ok(reqs[1].body === 'title_ids=CUSA28732,CUSA41712', 'legacy selected restore sends title ids');
  d.getElementById('nav-log').click(); await tick();
  ok(d.getElementById('v-log').className.includes('on') && d.getElementById('log').textContent === 'a\nb', 'legacy log page');
  d.getElementById('l-back').click();
  reqs.length = 0; d.querySelector('[data-a="unmount"][data-t="CUSA00791"]').click(); await tick();
  ok(reqs[0].url === '/api/unmount?title_id=CUSA00791' && reqs[0].method === 'POST', 'legacy card action posts');
})();
