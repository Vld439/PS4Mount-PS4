/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * The browser as the console's helper: the console's own HTTPS tops out near 10-15 MB/s,
 * its network takes twice that. While a download runs, PS4Mount hands out its blocks on
 * port 8771: /claim gives one (the address, the range), the browser fetches it at the
 * computer's speed (with its cookies) and delivers it with /put, or gives it back with
 * /drop when it cannot. With the browser closed, the console goes on alone. */
var RELAY_PORT = 8771, WORKERS = 12, running = 0, fails = 0;

function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

function relayPost(host, path, body) {
  return fetch('http://' + host + ':' + RELAY_PORT + path, { method: 'POST', headers: { 'X-PS4Mount': '1' }, body: body || '' });
}

async function relayWorker(host) {
  running++;
  try {
    for (;;) {
      var j = await (await relayPost(host, '/claim')).json();
      if (j.idle) break;
      if (j.wait) { await sleep(j.wait); continue; }
      var q = '?job=' + j.job + '&vol=' + j.vol + '&block=' + j.block, ok = false;
      try {
        var r = await fetch(j.url, { headers: { Range: 'bytes=' + j.from + '-' + (j.to - 1) }, credentials: 'include', cache: 'no-store' });
        if (r.status === 206) {
          var b = await r.arrayBuffer();
          ok = b.byteLength === j.to - j.from && (await relayPost(host, '/put' + q, b)).ok;
        }
      } catch (e) { console.log('relay block', j.block, e); }
      if (ok) { fails = 0; continue; }
      await relayPost(host, '/drop' + q).catch(function () {});
      /* the site refuses this browser: the console does it alone for a while */
      if (++fails >= WORKERS * 2) { fails = 0; await sleep(60000); break; }
      await sleep(2000);
    }
  } catch (e) { /* the console does not answer: tried again at the next alarm */ }
  running--;
}

/* Helpers at work while the console has a download: started by the alarm and when a
   download is sent; they end by themselves when it has none. */
function relayKick() {
  settings().then(function (s) {
    if (!s.relay || !s.host) return;
    while (running < WORKERS) relayWorker(s.host);
  });
}

api.alarms.create('relay', { periodInMinutes: 0.5 });
api.alarms.onAlarm.addListener(function (a) { if (a.name === 'relay') relayKick(); });
relayKick();
