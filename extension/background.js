/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Downloads of a pkg, a ZIP or a RAR go to the console instead of the computer: the
 * browser's download is cancelled and its address (after redirects, so a link that only
 * works once is not spent twice) sent to PS4Mount. The parts of a RAR wait in the basket
 * until the popup sends them together, as one download. Chrome (and Brave, Edge) asks
 * through onDeterminingFilename, before its "Save as" window; Firefox has only onCreated. */
if (typeof importScripts === 'function') importScripts('common.js');

function notify(text) {
  api.notifications.create({ type: 'basic', iconUrl: 'icons/icon128.png', title: msg('name'), message: text });
}

function badge(basket) {
  api.action.setBadgeText({ text: basket.length ? String(basket.length) : '' });
  api.action.setBadgeBackgroundColor({ color: '#3b82f6' });
}

/* Whether the download goes to the console (resolves true once it is cancelled here). */
function take(item, name) {
  var kind = kindOf(name), url = item.finalUrl || item.url;
  if (!kind || !/^https?:/i.test(url)) return Promise.resolve(false);
  return settings().then(function (s) {
    if (!s.on || !s.host || !s.dest) return false;
    console.log('to the console:', name, url);
    api.downloads.cancel(item.id).then(function () { return api.downloads.erase({ id: item.id }); }).catch(function (e) {
      console.log('cancel', e);
    });
    var line = lineOf(url, name);
    /* name.rar is the first volume when its name.r00... are in the basket */
    if (kind === 'file' && /\.rar$/i.test(name) && s.basket.some(function (b) { return b.base === baseOf(name); }))
      kind = 'part';
    if (kind === 'file') {
      sendLinks(s.host, s.dest, [line], '').then(function (j) {
        notify(j.ok ? msg('sent', [name]) : msg('refused', [j.message || '?']));
      }).catch(function (e) { notify(msg('unreachable', [s.host, e.message])); });
      return true;
    }
    var basket = s.basket.filter(function (b) { return b.line !== line; });
    basket.push({ line: line, name: name, base: baseOf(name) });
    return save({ basket: basket }).then(function () {
      badge(basket);
      notify(msg('kept', [name, String(basket.length)]));
      return true;
    });
  });
}

if (api.downloads.onDeterminingFilename) {
  /* the name the browser would give it is known here, before any window asks where */
  api.downloads.onDeterminingFilename.addListener(function (item, suggest) {
    var name = nameOf(item);
    if (!kindOf(name)) {
      suggest();
      return;
    }
    take(item, name).then(function (taken) { if (!taken) suggest(); }, function () { suggest(); });
    return true; /* suggest() comes later */
  });
} else {
  var waiting = {}; /* Firefox: downloads whose name is not known yet */
  api.downloads.onCreated.addListener(function (item) {
    var name = nameOf(item);
    if (kindOf(name) || item.filename) take(item, name);
    else waiting[item.id] = true;
  });
  api.downloads.onChanged.addListener(function (d) {
    if (!waiting[d.id] || !d.filename || !d.filename.current) return;
    delete waiting[d.id];
    api.downloads.search({ id: d.id }).then(function (items) {
      if (items[0]) take(items[0], nameOf(items[0]));
    });
  });
}

settings().then(function (s) { badge(s.basket); });
