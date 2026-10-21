/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * The popup: the console's address (asking the browser for leave to reach it), where to
 * save, sending on or off, and the basket of RAR parts with their password. */
function $(id) { return document.getElementById(id); }
document.querySelectorAll('[data-msg]').forEach(function (e) { e.textContent = msg(e.getAttribute('data-msg')); });

function status(id, text, kind) { $(id).textContent = text || ''; $(id).className = 'status' + (kind ? ' ' + kind : ''); }

/* The drives of the console into the select; the saved one chosen. */
function connect(host, dest) {
  status('hstatus', msg('checking'));
  return panel(host, '/api/dests').then(function (j) {
    var ds = j.dests || [];
    $('dest').innerHTML = '';
    ds.forEach(function (d) {
      var o = document.createElement('option');
      o.value = d.id;
      o.textContent = d.label + ' — ' + d.folder;
      $('dest').appendChild(o);
    });
    if (dest && ds.some(function (d) { return d.id === dest; })) $('dest').value = dest;
    status('hstatus', ds.length ? msg('connected') : msg('noDrive'), ds.length ? 'good' : 'bad');
    return save({ host: host, dest: $('dest').value });
  }).catch(function (e) { status('hstatus', msg('unreachable', [host, e.message]), 'bad'); });
}

function renderBasket(basket) {
  $('basket').innerHTML = '';
  basket.forEach(function (b, i) {
    var li = document.createElement('li'), s = document.createElement('span'), x = document.createElement('button');
    s.textContent = b.name;
    x.textContent = '×';
    x.title = msg('remove');
    x.onclick = function () {
      basket.splice(i, 1);
      save({ basket: basket }).then(function () { renderBasket(basket); });
    };
    li.appendChild(s);
    li.appendChild(x);
    $('basket').appendChild(li);
  });
  $('basketbox').style.display = basket.length ? '' : 'none';
  api.action.setBadgeText({ text: basket.length ? String(basket.length) : '' });
}

settings().then(function (s) {
  $('host').value = s.host;
  $('on').checked = s.on;
  renderBasket(s.basket);
  if (s.host) connect(s.host, s.dest);
});

$('test').onclick = function () {
  var host = $('host').value.trim().replace(/^https?:\/\//, '').replace(/[:/].*$/, '');
  if (!host) return;
  /* leave to reach the console's address, asked from this click */
  api.permissions.request({ origins: ['http://' + host + '/*'] }).then(function (ok) {
    if (ok) connect(host, '');
    else status('hstatus', msg('noPermission'), 'bad');
  });
};
$('dest').onchange = function () { save({ dest: $('dest').value }); };
$('on').onchange = function () { save({ on: $('on').checked }); };
$('clear').onclick = function () { save({ basket: [] }).then(function () { renderBasket([]); }); };
$('send').onclick = function () {
  settings().then(function (s) {
    if (!s.host || !s.dest) { status('bstatus', msg('setUpFirst'), 'bad'); return; }
    status('bstatus', msg('sending'));
    sendLinks(s.host, s.dest, s.basket.map(function (b) { return b.line; }), $('pw').value).then(function (j) {
      status('bstatus', j.message || '', j.ok ? 'good' : 'bad');
      if (j.ok) {
        $('pw').value = '';
        save({ basket: [] }).then(function () { setTimeout(function () { renderBasket([]); }, 1500); });
      }
    }).catch(function (e) { status('bstatus', msg('unreachable', [s.host, e.message]), 'bad'); });
  });
};
$('open').onclick = function (e) {
  e.preventDefault();
  settings().then(function (s) { if (s.host) api.tabs.create({ url: 'http://' + s.host + ':' + PORT + '/' }); });
};
