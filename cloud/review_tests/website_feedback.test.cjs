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

async function render(filename, station, photos = [], analysisEnabled = false) {
  const context = vm.createContext({ require, __dirname: path.join(root, 'server/tests'),
    __clientSource: fs.readFileSync(filename, 'utf8'), Headers, URL, URLSearchParams, Date,
    console, setTimeout, clearTimeout });
  vm.runInContext(harnessSource + '\nthis.makeHarness = harness;', context);
  const ui = context.makeHarness({
    statePayload: { stations: [{ station_id: 'plant-2', name: 'Plant 2', last_seen: Date.now(), status: {}, ...station }], photos, analysis_enabled: analysisEnabled },
    analysesPayload: { analyses: [], analysis_enabled: analysisEnabled },
    jobsPayload: { jobs: [], enabled: analysisEnabled }
  });
  await ui.settle(); await ui.submit('login-form');
  return ui;
}

for (const [index, filename] of clients.entries()) {
  test(`analysis notice follows the live enabled setting rather than claiming historical-only (${index})`, async () => {
    const enabled = await render(filename, {}, [], true);
    assert.match(enabled.elements['analysis-status-notice'].innerHTML, /Automatic photo review is active/);
    assert.doesNotMatch(enabled.elements['analysis-status-notice'].innerHTML, /Historical results only|does not run new crop analysis/);
    const disabled = await render(filename, {}, [], false);
    assert.match(disabled.elements['analysis-status-notice'].innerHTML, /Automatic photo review is off/);
  });
  test(`cleared and counter-only upload errors do not masquerade as current failure (${index})`, async () => {
    for (const status of [{ upload_error_count: 9, last_upload_error: '' }, { upload_error_count: 9 }]) {
      const ui = await render(filename, { status });
      assert.doesNotMatch(ui.elements['stations-grid'].innerHTML, /Photo upload issue reported/);
      await ui.clickAction({ action: 'station-details', station: 'plant-2' });
      assert.match(ui.elements['station-dialog-content'].innerHTML, /No upload error is currently reported/);
    }
    const ui = await render(filename, { last_upload_error: 'old root error', status: { last_upload_error: '' } });
    assert.doesNotMatch(ui.elements['stations-grid'].innerHTML, /old root error/);
  });

  test(`current upload failure is escaped and consistent in card and details (${index})`, async () => {
    const ui = await render(filename, { status: { upload_error_count: 1, last_upload_error: '<script>bad</script>' } });
    assert.match(ui.elements['stations-grid'].innerHTML, /Photo upload issue reported/);
    assert.match(ui.elements['stations-grid'].innerHTML, /&lt;script&gt;bad&lt;\/script&gt;/);
    await ui.clickAction({ action: 'station-details', station: 'plant-2' });
    assert.match(ui.elements['station-dialog-content'].innerHTML, /&lt;script&gt;bad&lt;\/script&gt;/);
  });

  test(`reset-interrupted and failed results remain visible without retrying captures (${index})`, async () => {
    const ui = await render(filename, { pending_commands: [], recent_commands: [
      { type: 'take_photo', state: 'uncertain', message: 'reset_during_command', updated_at: Date.now() },
      { type: 'take_photo', state: 'failed', message: '<img src=x onerror=bad()>' },
      { type: 'stay_awake', state: 'cancelled' }
    ] });
    const html = ui.elements['stations-grid'].innerHTML;
    assert.match(html, /No pending commands/);
    assert.match(html, /Outcome uncertain/);
    assert.match(html, /Camera restarted before confirming completion/);
    assert.match(html, /not repeated automatically/);
    assert.match(html, /Take a photo · Failed/);
    assert.match(html, /Stay awake · Cancelled/);
    assert.doesNotMatch(html, /<img src=x/);
    assert.equal(ui.calls.filter(call => call.url.endsWith('/commands')).length, 0);
  });

  test(`capture acknowledgement and uploaded photo are separate, station-isolated outcomes (${index})`, async () => {
    const identity = { date: '2026-10-22', filename: '2026-10-22__10-00-00-000.jpg' };
    const station = { recent_commands: [{ type: 'take_photo', state: 'completed', result: { photo: identity } }] };
    const otherPhoto = { id: 42, station_id: 'plant-1', ...identity };
    const waiting = await render(filename, station, [otherPhoto]);
    assert.match(waiting.elements['stations-grid'].innerHTML, /Capture confirmed; photo not received yet/);
    assert.doesNotMatch(waiting.elements['stations-grid'].innerHTML, /View photo/);
    const arrived = await render(filename, station, [{ ...otherPhoto, station_id: 'plant-2' }]);
    assert.match(arrived.elements['stations-grid'].innerHTML, /Photo received/);
    assert.match(arrived.elements['stations-grid'].innerHTML, /View photo/);
    await arrived.clickAction({ action: 'open-photo', photo: '42', station: 'plant-2' });
    assert.equal(arrived.elements['photo-dialog'].open, true);
  });

  test(`history is bounded and supports older API responses without history (${index})`, async () => {
    const ui = await render(filename, { recent_commands: Array.from({ length: 12 }, () => ({ type: 'take_photo', state: 'failed' })) });
    assert.equal((ui.elements['stations-grid'].innerHTML.match(/Take a photo · Failed/g) || []).length, 3);
    await ui.clickAction({ action: 'station-details', station: 'plant-2' });
    assert.equal((ui.elements['station-dialog-content'].innerHTML.match(/Take a photo · Failed/g) || []).length, 5);
    const old = await render(filename, {});
    assert.doesNotMatch(old.elements['stations-grid'].innerHTML, /RECENT COMMAND RESULTS/);
  });
}
