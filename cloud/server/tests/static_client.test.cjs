const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

class ElementStub {
  constructor(id = '') {
    this.id = id;
    this.listeners = {};
    this.attributes = {};
    this.dataset = {};
    this.hidden = false;
    this.disabled = false;
    this.checked = false;
    this.value = '';
    this.textContent = '';
    this.options = [];
    this.classes = new Set();
    this.classList = {
      add: (name) => this.classes.add(name),
      remove: (name) => this.classes.delete(name),
      toggle: (name, force) => {
        const enabled = force === undefined ? !this.classes.has(name) : force;
        enabled ? this.classes.add(name) : this.classes.delete(name);
        return enabled;
      },
      contains: (name) => this.classes.has(name)
    };
  }
  addEventListener(type, listener) { (this.listeners[type] ||= []).push(listener); }
  setAttribute(name, value) { this.attributes[name] = String(value); }
  focus() {}
  showModal() { this.open = true; }
  querySelector() { return this.submitButton || new ElementStub(); }
  set innerHTML(value) {
    this._html = String(value);
    if (this.id === 'station-filter') {
      this.options = [...this._html.matchAll(/<option value="([^"]*)">/g)].map((match) => ({ value: match[1] }));
    }
  }
  get innerHTML() { return this._html || ''; }
}

function harness(options = {}) {
  const ids = [
    'login-view', 'app-view', 'password', 'login-error', 'global-error', 'sync-badge', 'last-updated',
    'stations-grid', 'station-total', 'gallery-count', 'gallery-total', 'recent-photos', 'station-filter',
    'photo-search', 'date-filter', 'photo-grid', 'analysis-count', 'analysis-total', 'analysis-timeline',
    'analysis-records', 'analysis-jobs', 'analysis-jobs-count', 'analysis-archive-count', 'analysis-status-notice',
    'settings-feedback', 'cloud-sleep', 'timer-enabled', 'interval-hours', 'window-enabled',
    'window-start', 'window-end', 'login-form', 'logout-button', 'refresh-button', 'menu-button', 'sidebar',
    'cloud-settings-form', 'station-dialog', 'station-dialog-content', 'photo-dialog', 'photo-large',
    'photo-caption', 'toast', 'overview-view', 'gallery-view', 'analysis-view', 'page-title', 'analysis-footer-status'
  ];
  const elements = Object.fromEntries(ids.map((id) => [id, new ElementStub(id)]));
  elements.password.value = 'test-pass';
  elements['interval-hours'].value = '1';
  elements['window-start'].value = '06:00';
  elements['window-end'].value = '18:00';
  elements['timer-enabled'].checked = true;
  elements['window-enabled'].checked = false;
  elements['cloud-sleep'].checked = false;
  elements['station-filter'].value = 'all';
  elements['photo-search'].value = '';
  elements['date-filter'].value = 'all';
  elements['login-form'].submitButton = new ElementStub('login-submit');

  const navButtons = ['overview', 'gallery', 'analysis'].map((view) => {
    const button = new ElementStub(`nav-${view}`);
    button.dataset.view = view;
    return button;
  });
  const documentListeners = {};
  const document = {
    hidden: false,
    querySelector(selector) { return selector.startsWith('#') ? elements[selector.slice(1)] : new ElementStub(selector); },
    querySelectorAll(selector) { return selector.includes('data-view') ? navButtons : []; },
    addEventListener(type, listener) { (documentListeners[type] ||= []).push(listener); }
  };

  const nowSeconds = Math.floor(Date.now() / 1000);
  const statePayload = options.statePayload || {
    stations: [
      { station_id: 'plant-1', name: 'Plant 1', last_seen: nowSeconds - 2, next_wake_at: nowSeconds + 60, sleep_enabled: true, awake: false, status: { sleep_enabled: true, timer_enabled: true, interval_seconds: 3600, window_enabled: false, window_start: 360, window_end: 1080, upload_error_count: 0 }, latest_photo: { date: '2026-10-17', filename: 'rice.jpg', uploaded_at: nowSeconds - 5 }, pending_commands: [{ id: 'cmd-1', type: 'take_photo', payload: {} }] },
      { station_id: 'plant-2', name: 'Plant 2', last_seen: null, next_wake_at: null, sleep_enabled: false, awake: false, status: {}, latest_photo: null, pending_commands: [] },
      { station_id: 'plant-3', name: 'Plant 3', last_seen: null, next_wake_at: null, sleep_enabled: false, awake: false, status: {}, latest_photo: null, pending_commands: [] }
    ],
    photos: [{ id: 42, station_id: 'plant-1', date: '2026-10-17', filename: 'rice.jpg', url: '/api/photos/42', uploaded_at: nowSeconds - 5 }],
    analysis_enabled: false
  };
  const analysesPayload = options.analysesPayload || {
    analyses: [{ id: 7, source: 'analyses', source_key: 'plant-1|2026-10-17|rice.jpg', station_id: 'plant-1', date: '2026-10-17', filename: 'rice.jpg', record: { result_json: JSON.stringify({ final_tiller_count: 8, growth_stage: 'active tillering', water_level_cm: 2.4, tiller_confidence: 0.82, growth_confidence: 0.73, water_confidence: 0.9, notes: 'Eight visible shoots.' }), model: 'archive-model', effort: 'medium', passes: 2, completed_at: nowSeconds } }],
    analysis_enabled: false
  };
  const jobsPayload = options.jobsPayload || { enabled: false, jobs: [] };
  const calls = [];
  let intervals = [];
  const fetch = async (url, options = {}) => {
    const call = { url: String(url), method: options.method || 'GET', headers: options.headers, body: options.body };
    calls.push(call);
    let payload = {};
    if (call.url === '/api/session') payload = { authenticated: false, csrf_token: null };
    else if (call.url === '/api/login') payload = { authenticated: true, csrf_token: 'test-csrf' };
    else if (call.url === '/api/state') payload = statePayload;
    else if (call.url === '/api/analyses') payload = analysesPayload;
    else if (call.url === '/api/analysis/jobs') {
      if (options.failJobsRequest) throw new Error('Jobs API unavailable');
      payload = jobsPayload;
    }
    else if (/^\/api\/analysis\/jobs\/[^/]+\/retry$/.test(call.url)) payload = { ok: true, state: 'queued' };
    else if (call.url.includes('/cancel')) payload = { ok: true };
    else if (call.url.endsWith('/commands')) payload = { ok: true };
    else throw new Error(`Unexpected API request ${call.method} ${call.url}`);
    return { ok: true, status: 200, json: async () => payload };
  };

  const source = fs.readFileSync(path.join(__dirname, '..', 'static', 'app.js'), 'utf8');
  const context = vm.createContext({
    document, window: { setInterval(callback, ms) { intervals.push({ callback, ms }); return intervals.length; } },
    location: { origin: 'http://localhost' }, fetch, Headers, URL, URLSearchParams,
    Date, Math, Number, String, Promise, Array, Object, JSON, RegExp, Error, setTimeout, clearTimeout,
    console
  });
  vm.runInContext(source, context, { filename: 'app.js' });

  async function submit(id) {
    const form = elements[id];
    const listener = form.listeners.submit?.[0];
    assert.ok(listener, `${id} submit handler was installed`);
    await listener({ preventDefault() {}, currentTarget: form });
  }
  async function clickAction(dataset) {
    const button = new ElementStub('action');
    button.dataset = dataset;
    const listener = documentListeners.click?.[0];
    assert.ok(listener, 'delegated click handler was installed');
    await listener({ target: { closest: () => button } });
  }
  async function settle() { await new Promise((resolve) => setTimeout(resolve, 0)); }

  return { elements, navButtons, documentListeners, calls, intervals, statePayload, jobsPayload, submit, clickAction, settle };
}

test('admin login loads the API state, matches latest photos, and starts polling', async () => {
  const ui = harness();
  await ui.settle();
  assert.equal(ui.elements['login-view'].hidden, false);
  assert.equal(ui.intervals.length, 0, 'unauthenticated page does not start polling');

  await ui.submit('login-form');
  assert.equal(ui.elements['app-view'].hidden, false);
  assert.equal(ui.intervals.length, 1, 'polling starts after login');
  assert.equal(ui.intervals[0].ms, 30000);
  assert.match(ui.elements['stations-grid'].innerHTML, /Reported sleeping/);
  assert.match(ui.elements['stations-grid'].innerHTML, /Just checked in/, 'epoch-second last_seen is interpreted as seconds, not milliseconds');
  assert.match(ui.elements['stations-grid'].innerHTML, /Not connected yet/);
  assert.doesNotMatch(ui.elements['stations-grid'].innerHTML, /Plant 2[\s\S]*?Reported sleeping/);
  assert.match(ui.elements['stations-grid'].innerHTML, /src="http:\/\/localhost\/api\/photos\/42"/, 'station latest photo joins photo list by station/date/filename');
  assert.match(ui.elements['last-updated'].textContent, /Updated/);
  assert.equal(ui.elements['analysis-footer-status'].textContent, 'Automatic photo analysis is off');
  assert.ok(ui.calls.some((call) => call.url === '/api/analyses'));
});

test('schedule submit sends exact backend fields and firmware-compatible settings to all three stations', async () => {
  const ui = harness();
  await ui.settle();
  await ui.submit('login-form');
  await ui.submit('cloud-settings-form');
  const requests = ui.calls.filter((call) => call.method === 'POST' && call.url.endsWith('/commands'));
  assert.equal(requests.length, 3);
  for (const call of requests) {
    const body = JSON.parse(call.body);
    assert.equal(body.type, 'set_settings');
    assert.deepEqual(Object.keys(body.payload).sort(), ['interval_seconds', 'sleep_enabled', 'timer_enabled', 'window_enabled', 'window_end', 'window_start']);
    assert.equal(body.payload.sleep_enabled, false);
    assert.equal(body.payload.interval_seconds, 3600);
    assert.equal(body.payload.window_start, '06:00');
    assert.equal(body.payload.window_end, '18:00');
    assert.ok(body.payload.interval_seconds >= 3600 && body.payload.interval_seconds <= 604800);
    assert.equal(call.headers.get('X-CSRF-Token'), 'test-csrf');
  }
});

test('schedule form loads only settings reported consistently by every station', async () => {
  const ui = harness();
  for (const station of ui.statePayload.stations) {
    station.last_seen = Math.floor(Date.now() / 1000);
    station.sleep_enabled = true;
    station.status = { sleep_enabled: true, timer_enabled: false, interval_seconds: 7200, window_enabled: true, window_start: 480, window_end: 1140 };
  }
  await ui.settle();
  await ui.submit('login-form');
  assert.equal(ui.elements['cloud-sleep'].checked, true);
  assert.equal(ui.elements['timer-enabled'].checked, false);
  assert.equal(ui.elements['interval-hours'].value, '2');
  assert.equal(ui.elements['window-enabled'].checked, true);
  assert.equal(ui.elements['window-start'].value, '08:00');
  assert.equal(ui.elements['window-end'].value, '19:00');
  assert.match(ui.elements['settings-feedback'].textContent, /matching settings reported by all stations/);
});

test('analysis archive renders only imported values, confidence, notes, and recorded-value charts', async () => {
  const ui = harness();
  await ui.settle();
  await ui.submit('login-form');
  const html = ui.elements['analysis-records'].innerHTML;
  assert.match(html, /8/);
  assert.match(html, /active tillering/);
  assert.match(html, /82%/);
  assert.match(html, /Eight visible shoots\./);
  assert.match(ui.elements['analysis-timeline'].innerHTML, /Recorded tiller count/);
  assert.match(ui.elements['analysis-timeline'].innerHTML, /Recorded water level/);
  assert.doesNotMatch(html, /Reanaly[sz]e|Queue analysis/);
});

test('analysis jobs show every worker state without exposing internal provider details', async () => {
  const cases = [
    ['queued', /Queued for review/, /waiting for background review/],
    ['processing', /In progress/, /Background review is underway/],
    ['submitting', /In progress/, /Background review is underway/],
    ['submitted', /In progress/, /Background review is underway/],
    ['completed', /Completed/, /Unreadable/],
    ['failed', /Failed/, /Photo review failed/],
    ['uncertain', /Needs review/, /manual review required/]
  ];
  for (const [state, ...expected] of cases) {
    const job = {
      id: 91, photo_id: 42, station_id: 'plant-1', date: '2026-10-17', filename: 'rice.jpg',
      state, response_id: 'internal-response-id', model: 'internal-model-name', passes: 2,
      result: state === 'completed' ? { final_tiller_count: null, growth_stage: null, water_level_cm: null } : null,
      error: state === 'failed' ? 'Review could not be completed.' : null
    };
    const ui = harness({ jobsPayload: { enabled: true, jobs: [job] } });
    await ui.settle();
    await ui.submit('login-form');
    const html = ui.elements['analysis-jobs'].innerHTML;
    for (const pattern of expected) assert.match(html, pattern, `${state} job should include ${pattern}`);
    assert.doesNotMatch(html, /internal-response-id|internal-model-name|Responses API/);
    const retry = html.includes('data-action="retry-analysis"');
    assert.equal(retry, state === 'failed', 'only failed jobs expose a retry action');
    if (state === 'uncertain') assert.match(html, /manual review required/);
    if (state === 'failed') {
      await ui.clickAction({ action: 'retry-analysis', job: '91' });
      const request = ui.calls.find((call) => call.url === '/api/analysis/jobs/91/retry');
      assert.ok(request);
      assert.equal(request.method, 'POST');
      assert.equal(request.headers.get('X-CSRF-Token'), 'test-csrf');
    } else {
      assert.equal(ui.calls.some((call) => call.url.endsWith('/retry')), false, 'rendering other states never retries them');
    }
  }
});

test('analysis jobs endpoint errors fall back to an empty state and inline styles stay out of CSP-protected markup', async () => {
  const ui = harness({ failJobsRequest: true });
  await ui.settle();
  await ui.submit('login-form');
  assert.match(ui.elements['analysis-jobs'].innerHTML, /No cloud analysis jobs yet/);
  const html = fs.readFileSync(path.join(__dirname, '..', 'static', 'index.html'), 'utf8');
  const js = fs.readFileSync(path.join(__dirname, '..', 'static', 'app.js'), 'utf8');
  assert.doesNotMatch(html, /\sstyle\s*=/i);
  assert.doesNotMatch(js, /\sstyle\s*=/i);
});

test('queued command cancellation uses the station command ID route', async () => {
  const ui = harness();
  await ui.settle();
  await ui.submit('login-form');
  await ui.clickAction({ action: 'cancel', station: 'plant-1', command: 'cmd-1' });
  const cancel = ui.calls.find((call) => call.method === 'POST' && call.url === '/api/stations/plant-1/commands/cmd-1/cancel');
  assert.ok(cancel);
  assert.equal(cancel.headers.get('X-CSRF-Token'), 'test-csrf');
});
