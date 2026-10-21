// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 gasb07
// The browser extension (extension/): what it takes from a download (common.js, run with a
// stand-in for the browser's API), and that every text it shows is in both languages.
const fs = require('fs');
const vm = require('vm');
const path = require('path');
const E = path.join(__dirname, '..', '..', 'extension') + '/';
const ok = (c, m) => { console.log((c ? 'ok   ' : 'FAIL ') + m); if (!c) process.exitCode = 1; };

const ctx = { chrome: { i18n: { getMessage: () => '' }, storage: { local: {} } }, URL, fetch: () => Promise.reject(new Error('no')) };
vm.createContext(ctx);
vm.runInContext(fs.readFileSync(E + 'common.js', 'utf8'), ctx);
const { kindOf, baseOf, lineOf, nameOf } = ctx;

// --- what goes to the console, and how
ok(kindOf('Game.pkg') === 'file' && kindOf('bundle.ZIP') === 'file' && kindOf('one.rar') === 'file', 'a pkg, a ZIP, a RAR of its own: sent at once');
ok(kindOf('g.part1.rar') === 'part' && kindOf('g.PART12.rar') === 'part' && kindOf('g.r00') === 'part' && kindOf('g.r123') === 'part',
  'the parts of a RAR (name.partN.rar, name.rNN): to the basket');
ok(kindOf('setup.exe') === '' && kindOf('page.html') === '' && kindOf('pkg') === '' && kindOf('') === '', 'anything else: left to the browser');
ok(baseOf('[SITE.COM]-Game.part2.rar') === '[site.com]-game' && baseOf('game.r01') === 'game' && baseOf('game.rar') === 'game',
  'the archive a part belongs to');
const direct = 'https://host.example/files/Game.part1.rar?token=x';
ok(lineOf(direct, 'Game.part1.rar') === direct, 'a link with the name in it goes as it is');
ok(lineOf('https://host.example/get?id=7', 'Game.pkg') === 'https://host.example/get?id=7 | Game.pkg', 'a link without it gets | and the name');
ok(lineOf('https://host.example/files/%5BSITE%5D-Game.pkg', '[SITE]-Game.pkg') === 'https://host.example/files/%5BSITE%5D-Game.pkg',
  'a name percent-encoded in the address is found');
ok(nameOf({ filename: '/home/u/Downloads/Game.pkg', url: 'https://x/y' }) === 'Game.pkg' &&
  nameOf({ filename: 'C:\\Users\\u\\Game.rar', url: 'https://x/y' }) === 'Game.rar', 'the name of a download from its file (Linux, Windows)');
ok(nameOf({ filename: '', url: 'https://x/a/Game%20One.part3.rar?t=1' }) === 'Game One.part3.rar', 'or from its address while it has none');

// --- texts: every one used is in English and Spanish
const en = JSON.parse(fs.readFileSync(E + '_locales/en/messages.json', 'utf8'));
const es = JSON.parse(fs.readFileSync(E + '_locales/es/messages.json', 'utf8'));
const src = ['popup.html', 'popup.js', 'background.js', 'relay.js', 'common.js', 'manifest.json'].map((f) => fs.readFileSync(E + f, 'utf8')).join('\n');
const used = new Set();
for (const m of src.matchAll(/data-msg="([A-Za-z]+)"|msg\('([A-Za-z]+)'|__MSG_([A-Za-z]+)__/g)) used.add(m[1] || m[2] || m[3]);
const missing = [...used].filter((k) => !en[k] || !es[k]);
ok(used.size > 20 && !missing.length, 'every text in both languages' + (missing.length ? ': missing ' + missing.join(', ') : ''));
ok(Object.keys(en).sort().join() === Object.keys(es).sort().join(), 'the same texts in English and Spanish');
const man = JSON.parse(fs.readFileSync(E + 'manifest.json', 'utf8'));
ok(man.manifest_version === 3 && man.background.scripts.join() === 'common.js,relay.js,background.js' &&
  /importScripts\('common\.js', 'relay\.js'\)/.test(fs.readFileSync(E + 'background.js', 'utf8')),
  'Chrome (service worker) and Firefox (scripts) load the same files in the same order');
ok(man.optional_host_permissions.length === 2 && !man.host_permissions, 'no site is reached without asking (only optional host permissions)');
