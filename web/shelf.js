/* SPDX-License-Identifier: GPL-3.0-or-later */
function bindShelfNavigation(grid, tv, select, back) {
  var lastKey = 0;
  var directions = {37: 'Left', 38: 'Up', 39: 'Right', 40: 'Down'};
  function activeScope() { return document.querySelector('dialog[open]') || (tv ? document : grid); }
  function focus(el) {
    if (!el) return;
    el.focus();
    var id = el.getAttribute('data-g');
    if (id) select(id);
  }
  function move(direction) {
    var axis = direction === 'Left' || direction === 'Right' ? 'x' : 'y';
    var sign = direction === 'Left' || direction === 'Up' ? -1 : 1;
    var scope = activeScope();
    var from = document.activeElement;
    var controls = Array.prototype.filter.call(
      scope.querySelectorAll('button:not([disabled]),input:not([disabled]),select:not([disabled]),textarea:not([disabled]),a[href],[tabindex="0"]'),
      function (el) { return el.getClientRects().length; });
    if (controls.indexOf(from) < 0) {
      var cover = scope.querySelector('[data-g][aria-pressed="true"]');
      focus(controls.indexOf(cover) >= 0 ? cover : controls[0]);
      return;
    }
    var origin = from.getBoundingClientRect();
    var best, bestScore = Infinity;
    controls.forEach(function (el) {
      if (el === from) return;
      var rect = el.getBoundingClientRect();
      var dx = rect.left + rect.width / 2 - origin.left - origin.width / 2;
      var dy = rect.top + rect.height / 2 - origin.top - origin.height / 2;
      var along = (axis === 'x' ? dx : dy) * sign;
      if (along < 3) return;
      var across = Math.abs(axis === 'x' ? dy : dx);
      var score = along + across * 3 + (across > along ? 160 : 0);
      if (score < bestScore) { best = el; bestScore = score; }
    });
    focus(best);
  }
  document.addEventListener('keydown', function (e) {
    var direction = directions[e.keyCode] || (/^Arrow(Left|Up|Right|Down)$/.test(e.key) ? e.key.slice(5) : '');
    var editable = /^(INPUT|SELECT|TEXTAREA)$/.test(e.target.tagName) || e.target.isContentEditable;
    if (e.defaultPrevented || e.metaKey || e.ctrlKey || e.altKey) return;
    if (tv && (e.key === 'Escape' || e.keyCode === 27)) {
      lastKey = Date.now();
      e.preventDefault();
      back();
    } else if (direction && !editable && (tv || grid.contains(e.target))) {
      lastKey = Date.now();
      e.preventDefault();
      move(direction);
    } else if (tv && (e.key === 'Enter' || e.keyCode === 13)) {
      lastKey = Date.now();
    }
  });

  if (!tv || !navigator.getGamepads || !window.requestAnimationFrame) return;
  var previous = '', next = 0, confirmed = false, cancelled = false;
  function poll(now) {
    if (document.hidden) { window.requestAnimationFrame(poll); return; }
    var pads, pad;
    try { pads = navigator.getGamepads(); } catch (err) { return; }
    if (!pads) return;
    window.requestAnimationFrame(poll);
    for (var i = 0; i < pads.length; i++) if (pads[i]) { pad = pads[i]; break; }
    if (!pad) { previous = ''; confirmed = cancelled = false; return; }
    function pressed(index) {
      var button = pad.buttons[index];
      return typeof button === 'number' ? button > 0.5 : !!(button && button.pressed);
    }
    var direction = pressed(12) || pad.axes[1] < -.65 ? 'Up' :
      pressed(13) || pad.axes[1] > .65 ? 'Down' :
      pressed(14) || pad.axes[0] < -.65 ? 'Left' :
      pressed(15) || pad.axes[0] > .65 ? 'Right' : '';
    var ok = pressed(0), cancel = pressed(1);
    // Some dialog engines also surface the same physical input as native keys.
    if (Date.now() - lastKey > 250) {
      if (direction && (direction !== previous || now >= next)) {
        move(direction);
        next = now + (direction === previous ? 120 : 275);
      }
      if (ok && !confirmed) {
        var el = document.activeElement;
        if (el.tagName === 'SELECT' && el.options.length) {
          el.selectedIndex = (el.selectedIndex + 1) % el.options.length;
          var change = document.createEvent('HTMLEvents');
          change.initEvent('change', true, false);
          el.dispatchEvent(change);
        } else if (el !== document.body && !el.disabled) el.click();
        else move('Down');
      }
      if (cancel && !cancelled) back();
    }
    previous = direction;
    confirmed = ok;
    cancelled = cancel;
  }
  window.requestAnimationFrame(poll);
}
