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
  { title_id: 'CUSA34284', name: 'Moving Out 2', version: '01.04', game_version: '01.00', has_patch: true, size: 2.6e9, state: 'restart', mount: 'rw', source: '/mnt/usb0/ps4/CUSA34284', dlc: 0, update_issue: '', content_id: 'EP4064-CUSA34284_00-MOVINGOUT2000000', system_ver: '9.00', storage: 'usb0' }];
const CFG = { batch: true, batch_size: 5, dlc: true, restore_batch: 5, max_restore_batch: 10, paths: ['/'] };
function reply(url) {
  return url.indexOf('/api/log') === 0 ? { log: 'a\nb' } : url.indexOf('/api/config') === 0 ? CFG :
    url.indexOf('/api/games') === 0 ? { version: 'PS4Mount test', games: GAMES } : { ok: true, message: 'ok ' + url };
}
const tick = () => new Promise(r => setTimeout(r, 30));
const ok = (c, m) => { console.log((c ? 'ok   ' : 'FAIL ') + m); if (!c) process.exitCode = 1; };
function page(file, opts) {
  const reqs = [], vc = new VirtualConsole(), nav = [];
  vc.on('jsdomError', e => { if (/navigation/i.test(e.message)) nav.push(e.message); else console.log('jsdomError', e.message); });
  const dom = new JSDOM(fs.readFileSync(W + file, 'utf8'), {
    url: 'http://ps5:8770/', runScripts: 'dangerously', pretendToBeVisual: true, virtualConsole: vc,
    beforeParse(w) {
      w.setInterval = () => 0;
      w.scrollTo = () => {};
      w.confirm = () => (w.__confirm !== undefined ? w.__confirm : true);
      if (opts.dialog) {
        w.HTMLDialogElement.prototype.showModal = function () { this.setAttribute('open', ''); };
        w.HTMLDialogElement.prototype.close = function () { this.removeAttribute('open'); };
      }
      w.fetch = (url, o) => { reqs.push({ url, method: (o && o.method) || 'GET', body: o && o.body }); return Promise.resolve({ ok: true, json: () => Promise.resolve(reply(url)) }); };
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

(async () => {
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
  ok(d.querySelectorAll('.card').length === 5, 'five game cards');
  ok(d.querySelector('.card .name').textContent === 'Civ VII', 'cards sorted by name');
  { const c = [...d.querySelectorAll('.card')].find(x => x.textContent.includes('Moving Out 2'));
    ok(c && c.querySelector('.badge').textContent === 'Restart to play' && c.textContent.includes('Restart the console once'), 'a game added in this boot says to restart');
    ok(c && c.querySelector('[data-a="unmount"]') !== null, 'it can be removed from the library'); }
  d.querySelector('.summary span[data-f="restart"]').click();
  ok(d.querySelectorAll('.card').length === 1, 'the state chips filter the cards');
  d.querySelector('.summary span[data-f=""]').click();
  ok(d.querySelectorAll('.card').length === 5, 'all games again');
  d.querySelector('.art[data-g="CUSA34284"]').click(); await tick();
  ok(d.getElementById('gamedlg').hasAttribute('open') && d.getElementById('g-name').textContent === 'Moving Out 2', 'a cover opens the game');
  ok(d.getElementById('g-hero').style.backgroundImage.includes('/pic/CUSA34284'), 'with its background art');
  { const t = d.getElementById('g-facts').textContent;
    ok(t.includes('EP4064-CUSA34284_00-MOVINGOUT2000000') && t.includes('game v01.00 with its update') && t.includes('9.00'), 'details: content id, versions, firmware'); }
  ok(d.querySelector('#g-actions [data-a="unmount"]') !== null, 'and its actions');
  d.getElementById('gamedlg').removeAttribute('open');
  ok(d.querySelector('[data-a="restore"][data-t="CUSA28732"]') !== null, 'running game has Move pkgs back');
  ok(d.querySelector('[data-a="restore"][data-t="CUSA00190"]') === null, 'internal game has no Move pkgs back');
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
  const DESTS = [{ id: 'usb0', label: 'usb0', folder: '/mnt/usb0/pmscan', free: 67108864 + 1500, fits: true },
    { id: 'internal', label: 'internal storage', folder: '/data/pkg', free: 1e12, fits: true }];
  w.fetch = (url, o) => {
    if (url.indexOf('/api/check') === 0) {
      checks.push({ url, headers: (o && o.headers) || {} });
      const size = +url.split('size=')[1];
      const j = size === 1100 ? { ok: false, problem: 'needs firmware 14.00 or newer; this console has 13.52', kind: 'game', title_id: 'CUSA00002', dests: DESTS }
        : { ok: true, problem: '', kind: size === 2000 ? 'update' : 'game', title: 'Game ' + size, title_id: 'CUSA00001', app_ver: '01.00', notes: [], dests: DESTS };
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
  // the settings have the internal storage switch
  d.getElementById('settingsbtn').click(); await tick();
  ok(d.getElementById('s-internal') !== null, 'settings have the /data/pkg switch');

  // --- legacy page
  p = page('legacy.html', {}); ({ w, d, reqs } = p);
  await tick();
  ok(d.querySelectorAll('.card').length === 5 && d.getElementById('ver').textContent === 'PS4Mount test', 'legacy lists the games');
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
