/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * What the background and the popup share: the settings, the basket of RAR parts and
 * talking to PS4Mount's panel (port 8770) on the console. */
var api = typeof browser !== 'undefined' ? browser : chrome;
var PORT = 8770;

function msg(key, sub) { return api.i18n.getMessage(key, sub) || key; }

/* host: the console's address; dest: where it saves (usb0.., internal); on: sending at all.
   basket: RAR parts waiting for the rest, [{line, name, base}]. */
function settings() {
  return api.storage.local.get({ host: '', dest: '', on: true, basket: [] });
}
function save(obj) { return api.storage.local.set(obj); }

/* A file name from a download: its own, or the last part of its address. */
function nameOf(item) {
  var f = (item.filename || '').split(/[\\/]/).pop();
  if (f) return f;
  try {
    var u = new URL(item.finalUrl || item.url);
    return decodeURIComponent(u.pathname.split('/').pop() || '');
  } catch (e) { return ''; }
}

/* 'part' for a volume of a RAR in parts (name.part2.rar, name.r00), 'file' for a pkg, ZIP or
   RAR of its own, '' for anything else (left to the browser). */
function kindOf(name) {
  if (/\.part\d+\.rar$/i.test(name) || /\.r\d{2,3}$/i.test(name)) return 'part';
  if (/\.(pkg|zip|rar)$/i.test(name)) return 'file';
  return '';
}

/* The archive a part belongs to: its name without the volume (and a host's code before it). */
function baseOf(name) {
  return name.replace(/\.part\d+\.rar$/i, '').replace(/\.(rar|r\d{2,3})$/i, '').toLowerCase();
}

/* The line PS4Mount takes for a link: the address, and its name when the address lacks it. */
function lineOf(url, name) {
  var inUrl = '';
  try { inUrl = decodeURIComponent(new URL(url).pathname.split('/').pop() || ''); } catch (e) {}
  return inUrl.toLowerCase().indexOf(name.toLowerCase()) >= 0 || !name ? url : url + ' | ' + name;
}

function panel(host, path, opts) {
  return fetch('http://' + host + ':' + PORT + path, opts || {}).then(function (r) {
    if (!r.ok && r.status !== 400) throw new Error('HTTP ' + r.status);
    return r.json();
  });
}

/* The links to the console: {ok, message} as PS4Mount answers. */
function sendLinks(host, dest, lines, password) {
  var h = { 'X-PS4Mount': '1', 'Content-Type': 'text/plain; charset=utf-8' };
  if (password) h['X-PS4Mount-Password'] = encodeURIComponent(password);
  return panel(host, '/api/links/add?dest=' + encodeURIComponent(dest), { method: 'POST', headers: h, body: lines.join('\n') });
}
