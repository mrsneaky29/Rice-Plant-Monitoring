const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(__dirname, '..');
const harnessSource = fs.readFileSync(path.join(root, 'server/tests/static_client.test.cjs'), 'utf8')
  .split("\ntest('")[0]
  .replace("const source = fs.readFileSync(path.join(__dirname, '..', 'static', 'app.js'), 'utf8');", 'const source = __clientSource;');
const clients = [path.join(root, 'server/static/app.js')];
if (process.env.RICE_LIVE_STATIC_CLIENT) clients.push(path.resolve(process.env.RICE_LIVE_STATIC_CLIENT));

function clientHarness(filename, statePayload) {
  const context = vm.createContext({ require, __dirname: path.join(root, 'server/tests'),
    __clientSource: fs.readFileSync(filename, 'utf8'), Headers, URL, URLSearchParams, Date,
    console, setTimeout, clearTimeout });
  vm.runInContext(harnessSource + '\nthis.makeHarness = harness;', context);
  return context.makeHarness({ statePayload });
}

for (const [index, filename] of clients.entries()) {
  test(`capture time wins over delayed upload in gallery, station card, and modal (${index})`, async () => {
    const photos = [
      { id: 102, station_id: 'plant-2', date: '2026-10-21', filename: '2026-10-21__17-15-43-577.jpg', uploaded_at: 1792640000 },
      { id: 101, station_id: 'plant-2', date: '2026-10-22', filename: '2026-10-22__09-15-00-000.jpg', uploaded_at: 1792632000 }
    ];
    const ui = clientHarness(filename, { stations: [{ station_id: 'plant-2', name: 'Plant 2', status: {}, latest_photo: photos[0] }], photos, analysis_enabled: true });
    await ui.settle(); await ui.submit('login-form');
    const gallery = ui.elements['photo-grid'].innerHTML;
    assert.ok(gallery.indexOf('data-photo="101"') < gallery.indexOf('data-photo="102"'));
    assert.match(ui.elements['stations-grid'].innerHTML, /data-photo="101"/);
    assert.match(gallery, /Captured .*camera time/);
    assert.equal(ui.elements['analysis-footer-status'].textContent, 'Automatic photo analysis is on');
    await ui.clickAction({ action: 'open-photo', photo: '102', station: 'plant-2' });
    const expected = new Date(2026, 9, 21, 17, 15, 43, 577).toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' });
    assert.ok(ui.elements['photo-caption'].textContent.includes(`Captured ${expected} (camera time)`));
    assert.match(ui.elements['photo-caption'].textContent, / · Received /);
  });

  test(`today filter uses capture date rather than arrival date (${index})`, async () => {
    const now = new Date(); const yesterday = new Date(now.getFullYear(), now.getMonth(), now.getDate() - 1);
    const day = d => `${d.getFullYear()}-${String(d.getMonth()+1).padStart(2,'0')}-${String(d.getDate()).padStart(2,'0')}`;
    const photos = [now, yesterday].map((d,i) => ({ id:i+1, station_id:'plant-2', date:day(d), filename:`${day(d)}__10-00-00-000.jpg`, uploaded_at:Math.floor(Date.now()/1000) }));
    const ui = clientHarness(filename, { stations:[], photos });
    await ui.settle(); await ui.submit('login-form');
    ui.elements['date-filter'].value = 'today';
    ui.elements['date-filter'].listeners.input[0]();
    assert.match(ui.elements['photo-grid'].innerHTML, /data-photo="1"/);
    assert.doesNotMatch(ui.elements['photo-grid'].innerHTML, /data-photo="2"/);
  });

  test(`unknown and invalid capture timestamps never masquerade as upload timestamps (${index})`, async () => {
    const photos = ['rice.jpg','2026-02-31__12-00-00-000.jpg','2026-10-22__25-00-00-000.jpg'].map((filename,i) => ({ id:i+1, station_id:'plant-2', date:'UNSORTED', filename, uploaded_at:Math.floor(Date.now()/1000) }));
    const ui = clientHarness(filename, { stations:[], photos });
    await ui.settle(); await ui.submit('login-form');
    assert.equal((ui.elements['photo-grid'].innerHTML.match(/Capture time unavailable/g)||[]).length,3);
    await ui.clickAction({ action:'open-photo', photo:'1' });
    assert.match(ui.elements['photo-caption'].textContent,/Capture time unavailable · Received/);
  });
}
