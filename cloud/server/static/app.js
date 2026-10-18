(() => {
  'use strict';

  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
  const csrf = { token: '' };
  const model = { stations: [], photos: [], analyses: [], analysisJobs: [], analysisEnabled: false, busy: false, analysisBusy: false, lastUpdated: null, settingsInitialized: false, settingsDirty: false };
  let toastTimer;
  let pollingStarted = false;

  function escapeHtml(value) {
    return String(value ?? '').replace(/[&<>"']/g, (char) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[char]);
  }

  function safePhotoUrl(value) {
    if (!value) return '';
    try {
      const url = new URL(value, location.origin);
      return url.origin === location.origin && (url.protocol === 'http:' || url.protocol === 'https:') ? url.href : '';
    } catch { return ''; }
  }

  async function api(path, options = {}) {
    const headers = new Headers(options.headers || {});
    headers.set('Accept', 'application/json');
    if (options.body !== undefined) headers.set('Content-Type', 'application/json');
    if (csrf.token && options.method && options.method !== 'GET') headers.set('X-CSRF-Token', csrf.token);
    const response = await fetch(path, { credentials: 'same-origin', ...options, headers, body: options.body === undefined ? undefined : JSON.stringify(options.body) });
    let data = {};
    try { data = await response.json(); } catch { /* Error responses may have no JSON body. */ }
    if (!response.ok) {
      const detail = data.detail;
      const detailText = typeof detail === 'string' ? detail : detail ? JSON.stringify(detail) : '';
      const error = new Error(detailText || data.error || data.message || `Request failed (${response.status})`);
      error.status = response.status;
      throw error;
    }
    return data;
  }

  function showLogin() {
    $('#login-view').hidden = false;
    $('#app-view').hidden = true;
    $('#password').focus();
  }

  function showApp() {
    $('#login-view').hidden = true;
    $('#app-view').hidden = false;
  }

  async function initialize() {
    try {
      const session = await api('/api/session');
      csrf.token = session.csrf_token || '';
      if (!session.authenticated) return showLogin();
      showApp();
      await refreshState();
      await refreshAnalyses();
      startPolling();
    } catch (error) {
      showLogin();
      setLoginError(`Could not connect to Rice Cloud. ${error.message}`);
    }
  }

  function startPolling() {
    if (pollingStarted) return;
    pollingStarted = true;
    window.setInterval(() => {
      if (document.hidden || $('#app-view').hidden) return;
      refreshState({ quiet: true });
      refreshAnalyses({ quiet: true });
    }, 30000);
  }

  function minuteToInput(value) {
    if (typeof value === 'string' && /^(?:[01]\d|2[0-3]):[0-5]\d$/.test(value)) return value;
    const minute = Number(value);
    if (!Number.isInteger(minute) || minute < 0 || minute > 1439) return '';
    return `${String(Math.floor(minute / 60)).padStart(2, '0')}:${String(minute % 60).padStart(2, '0')}`;
  }

  function seedSettingsFromReportedState() {
    const feedback = $('#settings-feedback');
    if (model.settingsInitialized || model.settingsDirty || !feedback) return;
    const reports = model.stations.filter((station) => station.last_seen);
    if (!reports.length) {
      feedback.textContent = 'Defaults are unsaved requests. Choose a schedule and queue it explicitly.';
      return;
    }
    if (reports.length !== model.stations.length) {
      feedback.textContent = 'Some stations have not reported settings yet. The form remains an unsaved request.';
      return;
    }
    const configs = reports.map((station) => {
      const status = station.status || {};
      return {
        sleep_enabled: status.sleep_enabled ?? station.sleep_enabled,
        timer_enabled: status.timer_enabled,
        interval_seconds: status.interval_seconds,
        window_enabled: status.window_enabled,
        window_start: minuteToInput(status.window_start),
        window_end: minuteToInput(status.window_end)
      };
    });
    const keys = ['sleep_enabled', 'timer_enabled', 'interval_seconds', 'window_enabled', 'window_start', 'window_end'];
    const complete = configs.every((config) => keys.every((key) => config[key] !== undefined && config[key] !== null && config[key] !== ''));
    const consistent = complete && configs.every((config) => keys.every((key) => config[key] === configs[0][key]));
    if (!consistent) {
      feedback.textContent = 'Stations report different or incomplete settings. Per-station applied values are shown above; choose what to queue.';
      return;
    }
    const config = configs[0];
    $('#cloud-sleep').checked = config.sleep_enabled;
    $('#timer-enabled').checked = config.timer_enabled;
    $('#interval-hours').value = String(config.interval_seconds / 3600);
    $('#window-enabled').checked = config.window_enabled;
    $('#window-start').value = config.window_start;
    $('#window-end').value = config.window_end;
    model.settingsInitialized = true;
    feedback.textContent = 'Loaded the matching settings reported by all stations. Queue settings to send any changes.';
  }

  function dateValue(value) {
    if (value === null || value === undefined || value === '') return NaN;
    if (typeof value === 'number' || /^\d+(?:\.\d+)?$/.test(String(value).trim())) {
      const numeric = Number(value);
      return numeric > 0 && numeric < 1e12 ? numeric * 1000 : numeric;
    }
    return new Date(value).getTime();
  }

  function setLoginError(message = '') {
    const el = $('#login-error');
    el.textContent = message;
    el.hidden = !message;
  }

  async function refreshState({ quiet = false } = {}) {
    if (model.busy) return;
    model.busy = true;
    if (!quiet) {
      $('#sync-badge').innerHTML = '<span class="pulse-dot"></span> Syncing';
      $('#global-error').hidden = true;
    }
    try {
      const data = await api('/api/state');
      model.stations = Array.isArray(data.stations) ? data.stations : [];
      model.photos = Array.isArray(data.photos) ? data.photos : [];
      model.analysisEnabled = data.analysis_enabled === true;
      model.settings = data.settings || null;
      seedSettingsFromReportedState();
      model.lastUpdated = new Date();
      $('#last-updated').textContent = `Updated ${model.lastUpdated.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })}`;
      $('#sync-badge').innerHTML = '<span class="pulse-dot"></span> Up to date';
      render();
    } catch (error) {
      if (error.status === 401) { csrf.token = ''; showLogin(); }
      else {
        $('#sync-badge').innerHTML = '<span class="offline-dot"></span> Sync issue';
        $('#global-error').textContent = `Dashboard data could not be refreshed: ${error.message}. Your last loaded view is still shown.`;
        $('#global-error').hidden = false;
        if (!model.lastUpdated) {
          $('#stations-grid').innerHTML = `<div class="empty-card error-card"><span class="empty-icon">!</span><strong>Could not load station data</strong><p>${escapeHtml(error.message)}</p><button class="button secondary" data-action="refresh">Try again</button></div>`;
        }
      }
    } finally { model.busy = false; }
  }

  function formatDate(value, fallback = 'Not reported') {
    if (!value) return fallback;
    const date = new Date(dateValue(value));
    return Number.isNaN(date.getTime()) ? escapeHtml(value) : date.toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' });
  }

  function relativeTime(value) {
    if (!value) return 'No check-in reported';
    const time = dateValue(value);
    if (!Number.isFinite(time)) return 'Check-in time unavailable';
    const minutes = Math.max(0, Math.round((Date.now() - time) / 60000));
    if (minutes < 1) return 'Just checked in';
    if (minutes < 60) return `${minutes} min ago`;
    const hours = Math.floor(minutes / 60);
    if (hours < 24) return `${hours} hr${hours === 1 ? '' : 's'} ago`;
    const days = Math.floor(hours / 24);
    return `${days} day${days === 1 ? '' : 's'} ago`;
  }

  function stationById(id) { return model.stations.find((station) => String(station.station_id) === String(id)); }
  function stationName(id) { return stationById(id)?.name || id || 'Unknown station'; }

  function reportedAwake(station) {
    if (!station.last_seen) return null;
    if (typeof station.awake === 'boolean') return station.awake;
    const status = typeof station.status === 'string' ? station.status : station.status?.state || station.status?.reported_state || station.status?.mode || station.status?.power_state;
    if (typeof status === 'string') return /awake|online|active|maintenance/i.test(status) && !/sleep/i.test(status);
    return null;
  }

  function reportedStatusLabel(station, awake) {
    if (!station.last_seen) return 'Not connected yet';
    if (awake !== null) return `Reported ${awake ? 'awake' : 'sleeping'}`;
    if (typeof station.status === 'string') return `Reported ${station.status}`;
    const label = station.status?.state || station.status?.reported_state || station.status?.mode || station.status?.power_state;
    return label ? `Reported ${label}` : 'State not reported';
  }

  function maintenanceSeconds(station) {
    const value = station.maintenance_remaining_seconds ?? station.status?.maintenance_remaining_seconds;
    return Number.isFinite(Number(value)) && Number(value) > 0 ? Number(value) : 0;
  }

  function scheduleState(station) {
    if (!station.next_wake_at) return { label: 'No wake scheduled', className: 'unknown' };
    const time = dateValue(station.next_wake_at);
    if (!Number.isFinite(time)) return { label: 'Wake time unavailable', className: 'unknown' };
    if (time < Date.now() && !reportedAwake(station)) return { label: 'Wake overdue', className: 'overdue' };
    return { label: time <= Date.now() ? 'Wake time reached' : `Next wake ${formatDate(station.next_wake_at)}`, className: time <= Date.now() ? 'due' : 'scheduled' };
  }

  function statusMarkup(station) {
    const awake = reportedAwake(station);
    const label = reportedStatusLabel(station, awake);
    const className = awake === null ? 'unknown' : awake ? 'awake' : 'sleeping';
    const schedule = scheduleState(station);
    const maintenance = maintenanceSeconds(station);
    const maintenanceLabel = maintenance ? `Maintenance · ${Math.ceil(maintenance / 60)} min remaining` : '';
    return `<div class="status-pair"><span class="state-pill ${className}"><i></i>${escapeHtml(label)}</span>${maintenanceLabel ? `<span class="state-pill maintenance"><i></i>${maintenanceLabel}</span>` : ''}<span class="state-pill schedule-${schedule.className}"><i></i>${escapeHtml(schedule.label)}</span></div>`;
  }

  function getLatestPhoto(station) {
    const embedded = station.latest_photo;
    if (embedded && typeof embedded === 'object') {
      const byId = embedded.id && model.photos.find((photo) => String(photo.id) === String(embedded.id));
      const byIdentity = model.photos.find((photo) => String(photo.station_id) === String(station.station_id) && photo.date === embedded.date && photo.filename === embedded.filename);
      return byId || byIdentity || (embedded.url ? embedded : null);
    }
    return model.photos.filter((photo) => String(photo.station_id) === String(station.station_id)).sort((a, b) => dateValue(b.uploaded_at || b.date) - dateValue(a.uploaded_at || a.date))[0] || null;
  }

  function pendingMarkup(station) {
    const commands = Array.isArray(station.pending_commands) ? station.pending_commands : [];
    if (!commands.length) return '<div class="pending-empty">No pending commands</div>';
    return `<div class="pending-list">${commands.map((command) => {
      const type = command.type || 'command';
      const commandId = command.id || command.command_id;
      const title = ({ take_photo: 'Take a photo', stay_awake: 'Stay awake', resume_sleep: 'Resume sleep', set_settings: 'Update settings' })[type] || type.replaceAll('_', ' ');
      const state = command.status || 'Waiting for station';
      return `<div class="pending-command"><span><strong>${escapeHtml(title)}</strong><small>${escapeHtml(state)} · delivered at next wake</small></span>${commandId ? `<button class="cancel-command" data-action="cancel" data-station="${escapeHtml(station.station_id)}" data-command="${escapeHtml(commandId)}" aria-label="Cancel ${escapeHtml(title)}">Cancel</button>` : ''}</div>`;
    }).join('')}</div>`;
  }

  function photoThumb(photo, size = 'small', stationId = photo?.station_id || '') {
    if (!photo) return `<div class="photo-placeholder ${size}"><span>▧</span><small>No photo received yet</small></div>`;
    const url = safePhotoUrl(photo.url || (photo.id ? `/api/photos/${encodeURIComponent(photo.id)}` : ''));
    if (!url) return `<div class="photo-placeholder ${size}"><span>▧</span><small>Photo unavailable</small></div>`;
    return `<button class="photo-thumb ${size}" data-action="open-photo" data-photo="${escapeHtml(photo.id || '')}" data-station="${escapeHtml(stationId)}" aria-label="Open photo from ${escapeHtml(stationName(stationId))}"><img src="${escapeHtml(url)}" alt="Rice plant photo from ${escapeHtml(stationName(stationId))}" loading="lazy"><span class="photo-open">↗</span></button>`;
  }

  function renderStations() {
    const grid = $('#stations-grid');
    $('#station-total').textContent = `${model.stations.length} station${model.stations.length === 1 ? '' : 's'}`;
    if (!model.stations.length) {
      grid.innerHTML = '<div class="empty-card"><span class="empty-icon">⌁</span><strong>No stations have checked in</strong><p>When a camera station first connects, its reported state and photo history will appear here.</p></div>';
      return;
    }
    grid.innerHTML = model.stations.map((station) => {
      const id = String(station.station_id ?? '');
      const photo = getLatestPhoto(station);
      const lastSeen = station.last_seen ? relativeTime(station.last_seen) : 'No check-in reported';
      const uploadErrorCount = Number(station.status?.upload_error_count || 0);
      const uploadError = station.upload_error || station.last_upload_error || station.upload_message || station.status?.upload_error || station.status?.last_upload_error || station.status?.upload_message || (uploadErrorCount > 0 && (station.status?.last_upload_error || `${uploadErrorCount} upload error${uploadErrorCount === 1 ? '' : 's'} reported since reboot`));
      const applied = station.last_seen ? station.applied_settings || station.status?.settings || station.status?.applied_settings || station.status || null : null;
      const requested = (Array.isArray(station.pending_commands) ? station.pending_commands : []).find((command) => command.type === 'set_settings')?.payload;
      const intervalReported = Number.isFinite(Number(applied?.interval_seconds)) && Number(applied.interval_seconds) > 0;
      const reportedSleep = station.last_seen ? (typeof applied?.sleep_enabled === 'boolean' ? applied.sleep_enabled : station.sleep_enabled) : null;
      const settingLine = !station.last_seen ? 'Applied settings: waiting for station report' : `Applied: ${intervalReported ? `${Math.round(applied.interval_seconds / 3600 * 10) / 10}h interval` : 'interval not reported'} · ${reportedSleep === true ? 'sleep on' : reportedSleep === false ? 'sleep off' : 'sleep not reported'}${applied?.timer_enabled === true ? ' · timer on' : applied?.timer_enabled === false ? ' · timer off' : ''}`;
      return `<article class="station-card" data-station="${escapeHtml(id)}">
        <div class="station-photo">${photoThumb(photo, 'station', id)}</div>
        <div class="station-body">
          <div class="station-heading"><div><p class="station-id">CAMERA STATION <span>·</span> ${escapeHtml(id)}</p><h3>${escapeHtml(station.name || `Station ${id}`)}</h3></div><button class="icon-button more-button" data-action="station-details" data-station="${escapeHtml(id)}" aria-label="Station details">···</button></div>
          ${statusMarkup(station)}
          <div class="station-meta"><div><span class="meta-label">LAST SEEN</span><strong title="${escapeHtml(formatDate(station.last_seen))}">${escapeHtml(lastSeen)}</strong></div><div><span class="meta-label">NEXT EXPECTED WAKE</span><strong>${escapeHtml(formatDate(station.next_wake_at))}</strong></div></div>
          <div class="settings-state"><span class="meta-label">STATION SETTINGS</span><p>${settingLine}</p>${requested ? `<small class="requested-state">Requested: ${requested.interval_seconds ? `${Math.round(requested.interval_seconds / 3600 * 10) / 10}h interval` : 'settings'} · awaiting station report</small>` : ''}</div>
          ${uploadError ? `<div class="upload-error"><span>!</span><span><strong>Photo upload issue reported</strong><small>${escapeHtml(typeof uploadError === 'string' ? uploadError : 'See station status for details.')}</small></span></div>` : ''}
          <div class="station-commands"><div class="commands-head"><span class="meta-label">COMMAND QUEUE</span><span class="delivery-hint">Delivered at next wake</span></div>${pendingMarkup(station)}</div>
          <div class="station-actions"><button class="button secondary" data-action="take-photo" data-station="${escapeHtml(id)}">Take photo</button><button class="button quiet" data-action="stay-awake" data-station="${escapeHtml(id)}">Stay awake</button><button class="button quiet" data-action="resume-sleep" data-station="${escapeHtml(id)}">Resume sleep</button></div>
        </div>
      </article>`;
    }).join('');
  }

  function renderPhotos() {
    const photos = [...model.photos].sort((a, b) => dateValue(b.uploaded_at || b.date) - dateValue(a.uploaded_at || a.date));
    $('#gallery-count').textContent = String(photos.length);
    $('#gallery-total').textContent = `${photos.length} photo${photos.length === 1 ? '' : 's'}`;
    $('#recent-photos').innerHTML = photos.length ? photos.slice(0, 4).map((photo) => `<article class="recent-photo">${photoThumb(photo, 'recent')}<div><strong>${escapeHtml(stationName(photo.station_id))}</strong><small>${escapeHtml(formatDate(photo.uploaded_at || photo.date))}</small></div></article>`).join('') : '<div class="empty-inline">No photos have been uploaded yet. Received station photos will appear here.</div>';
    const stationFilter = $('#station-filter');
    const previous = stationFilter.value;
    stationFilter.innerHTML = '<option value="all">All stations</option>' + model.stations.map((station) => `<option value="${escapeHtml(station.station_id)}">${escapeHtml(station.name || station.station_id)}</option>`).join('');
    if ([...stationFilter.options].some((option) => option.value === previous)) stationFilter.value = previous;
    renderGallery();
  }

  function renderGallery() {
    const query = $('#photo-search').value.trim().toLowerCase();
    const station = $('#station-filter').value;
    const range = $('#date-filter').value;
    const now = Date.now();
    let filtered = model.photos.filter((photo) => {
      const name = `${photo.filename || ''} ${stationName(photo.station_id)}`.toLowerCase();
      if (query && !name.includes(query)) return false;
      if (station !== 'all' && String(photo.station_id) !== station) return false;
      const time = dateValue(photo.uploaded_at || photo.date);
      if (range === 'today' && (!Number.isFinite(time) || new Date(time).toDateString() !== new Date().toDateString())) return false;
      if (range === 'week' && (!Number.isFinite(time) || time < now - 7 * 86400000)) return false;
      if (range === 'month' && (!Number.isFinite(time) || time < now - 30 * 86400000)) return false;
      return true;
    }).sort((a, b) => dateValue(b.uploaded_at || b.date) - dateValue(a.uploaded_at || a.date));
    const grid = $('#photo-grid');
    if (!filtered.length) {
      const hasPhotos = model.photos.length > 0;
      grid.innerHTML = `<div class="empty-card gallery-empty"><span class="empty-icon">▧</span><strong>${hasPhotos ? 'No photos match these filters' : 'Your photo archive is empty'}</strong><p>${hasPhotos ? 'Try a different station, date range, or search.' : 'New camera uploads will be added to your archive automatically.'}</p>${hasPhotos ? '<button class="text-button" data-action="clear-filters">Clear filters</button>' : ''}</div>`;
      return;
    }
    grid.innerHTML = filtered.map((photo) => `<article class="gallery-card">${photoThumb(photo, 'gallery')}<div class="gallery-photo-info"><div><strong>${escapeHtml(stationName(photo.station_id))}</strong><small>${escapeHtml(photo.filename || 'Station photo')}</small></div><time>${escapeHtml(formatDate(photo.uploaded_at || photo.date))}</time></div></article>`).join('');
  }

  function analysisParts(entry) {
    const record = entry.record && typeof entry.record === 'object' ? entry.record : {};
    let raw = record.result_json ?? record.analysis_json ?? record.result ?? {};
    if (typeof raw === 'string') { try { raw = JSON.parse(raw); } catch { raw = {}; } }
    const result = raw?.result && typeof raw.result === 'object' ? raw.result : raw;
    return { record, result: result && typeof result === 'object' ? result : {} };
  }

  function analysisGraph(entries, key, title, unit = '') {
    const points = entries.map((entry) => {
      const { record, result } = analysisParts(entry);
      const candidate = key === 'tiller_count' ? (result.final_tiller_count ?? result.tiller_count ?? result.count) : result[key];
      const value = typeof candidate === 'number' && Number.isFinite(candidate) ? candidate : null;
      return { entry, record, value, station: entry.station_id || record.station_id || 'Unknown station', date: dateValue(record.completed_at || record.archived_at || entry.date || record.date) };
    }).filter((point) => point.value !== null).sort((a, b) => a.date - b.date);
    if (!points.length) return '';
    const W = 680, H = 190, L = 45, R = 14, T = 14, B = 34;
    let min = Math.min(...points.map((point) => point.value)), max = Math.max(...points.map((point) => point.value));
    if (min === max) { const padding = Math.abs(min) * .12 || 1; min -= padding; max += padding; }
    const x = (index) => L + (points.length === 1 ? (W - L - R) / 2 : index * (W - L - R) / (points.length - 1));
    const y = (value) => T + (max - value) * (H - T - B) / (max - min);
    const colors = ['#3b754d', '#ba8435', '#5b77a2', '#9a678e', '#388b88'];
    const stations = [...new Set(points.map((point) => point.station))];
    const grid = [0, 1, 2, 3].map((step) => {
      const yy = T + step * (H - T - B) / 3;
      const value = max - step * (max - min) / 3;
      return `<line x1="${L}" y1="${yy}" x2="${W - R}" y2="${yy}" class="chart-gridline"/><text x="${L - 7}" y="${yy + 3}" text-anchor="end" class="chart-label">${escapeHtml(Number(value.toPrecision(3)))}${escapeHtml(unit)}</text>`;
    }).join('');
    const series = stations.map((station, colorIndex) => {
      const own = points.map((point, index) => ({ ...point, index })).filter((point) => point.station === station);
      const color = colors[colorIndex % colors.length];
      const path = own.map((point, index) => `${index ? 'L' : 'M'}${x(point.index)},${y(point.value)}`).join(' ');
      const dots = own.map((point) => `<circle cx="${x(point.index)}" cy="${y(point.value)}" r="3.5" fill="${color}"><title>${escapeHtml(stationName(station))} · ${escapeHtml(formatDate(point.record.completed_at || point.entry.date || point.record.date))}: ${escapeHtml(point.value)}${escapeHtml(unit)}</title></circle>`).join('');
      return `<path d="${path}" fill="none" stroke="${color}" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/>${dots}`;
    }).join('');
    const first = points[0].record.completed_at || points[0].entry.date || points[0].record.date;
    const last = points.at(-1).record.completed_at || points.at(-1).entry.date || points.at(-1).record.date;
    const legend = stations.map((station, index) => `<span><i class="legend-color-${index % colors.length}"></i>${escapeHtml(stationName(station))}</span>`).join('');
    return `<article class="analysis-chart-card"><div class="analysis-chart-heading"><div><h3>${escapeHtml(title)}</h3><small>${points.length} recorded point${points.length === 1 ? '' : 's'} · imported history</small></div></div><svg class="analysis-chart" viewBox="0 0 ${W} ${H}" role="img" aria-label="${escapeHtml(title)} over imported analysis history">${grid}${series}<text x="${L}" y="${H - 10}" class="chart-label">${escapeHtml(formatDate(first, 'Start'))}</text><text x="${W - R}" y="${H - 10}" text-anchor="end" class="chart-label">${escapeHtml(formatDate(last, 'Latest'))}</text></svg><div class="chart-legend">${legend}</div></article>`;
  }

  function renderAnalysis() {
    const jobs = [...model.analysisJobs].sort((a, b) => dateValue(b.created_at) - dateValue(a.created_at));
    const allAnalyses = [...model.analyses];
    const historical = allAnalyses.filter((entry) => entry.source !== 'cloud_analysis');
    const totalCount = jobs.length + historical.length;

    $('#analysis-count').textContent = String(totalCount);
    $('#analysis-total').textContent = `${totalCount} report${totalCount === 1 ? '' : 's'}`;

    const statusNotice = $('#analysis-status-notice');
    if (statusNotice) {
      if (model.analysisEnabled) {
        statusNotice.innerHTML = '<span class="analysis-summary-icon">⌁</span><p><strong>Automatic photo review is active</strong><br>Newly uploaded photos are reviewed in the background. Stations do not wait for results before returning to sleep.</p>';
      } else {
        statusNotice.innerHTML = '<span class="analysis-summary-icon">i</span><p><strong>Automatic photo review is off</strong><br>It starts disabled. Existing reports and imported history remain available.</p>';
      }
    }

    // Render Cloud Analysis Jobs
    const jobsCountEl = $('#analysis-jobs-count');
    if (jobsCountEl) jobsCountEl.textContent = `${jobs.length} job${jobs.length === 1 ? '' : 's'}`;
    const jobsTarget = $('#analysis-jobs');
    if (jobsTarget) {
      if (!jobs.length) {
        jobsTarget.innerHTML = `<div class="empty-card gallery-empty"><span class="empty-icon">⌁</span><strong>No cloud analysis jobs yet</strong><p>${model.analysisEnabled ? 'Uploaded photos will appear here with live analysis status and structured results.' : 'When automatic photo analysis is enabled, new station uploads will appear here.'}</p></div>`;
      } else {
        jobsTarget.innerHTML = jobs.map((job) => {
          const matchingPhoto = (job.photo_id && model.photos.find((p) => String(p.id) === String(job.photo_id))) ||
            model.photos.find((p) => String(p.station_id) === String(job.station_id) && p.date === job.date && p.filename === job.filename);
          const photoId = job.photo_id || matchingPhoto?.id || '';
          const photoObj = matchingPhoto || (photoId ? { id: photoId, station_id: job.station_id, date: job.date, filename: job.filename } : null);
          const photoMarkup = photoObj ? photoThumb(photoObj, 'analysis', job.station_id) : '<div class="analysis-photo-empty">Photo not in active cache</div>';

          let statePill = '';
          if (job.state === 'queued') {
            statePill = '<span class="state-pill job-queued"><i></i>Queued</span>';
          } else if (['submitting', 'submitted', 'processing'].includes(job.state)) {
            statePill = '<span class="state-pill job-processing"><i></i>In progress</span>';
          } else if (job.state === 'completed') {
            statePill = '<span class="state-pill job-completed"><i></i>Completed</span>';
          } else if (job.state === 'failed') {
            statePill = '<span class="state-pill job-failed"><i></i>Failed</span>';
          } else if (job.state === 'uncertain') {
            statePill = '<span class="state-pill job-uncertain"><i></i>Needs review</span>';
          } else {
            statePill = `<span class="state-pill unknown"><i></i>${escapeHtml(job.state)}</span>`;
          }

          const metric = (label, value, unit = '') => value === null || value === undefined || value === '' ? '' : `<div class="analysis-metric"><span>${escapeHtml(label)}</span><strong>${escapeHtml(value)}${escapeHtml(unit)}</strong></div>`;

          let bodyContent = '';
          if (job.state === 'completed' && job.result) {
            const res = job.result;
            const tillers = res.final_tiller_count ?? res.tiller_count ?? 'Unreadable';
            const checks = Array.isArray(res.tiller_check_counts) && res.tiller_check_counts.length > 1 ? ` (checks: ${res.tiller_check_counts.map((c) => c ?? 'Unreadable').join(', ')})` : '';
            const stage = res.growth_stage || 'Unreadable';
            const water = res.water_level_cm !== null && res.water_level_cm !== undefined ? `${res.water_level_cm} cm` : 'Unreadable';
            const confidence = [
              ['Tillers', res.tiller_confidence],
              ['Growth stage', res.growth_confidence],
              ['Water level', res.water_confidence]
            ].filter(([, val]) => typeof val === 'number' && Number.isFinite(val));
            const notes = res.notes || '';

            bodyContent = `
              <div class="analysis-metrics">
                ${metric('Tillers', `${tillers}${checks}`)}
                ${metric('Growth stage', stage)}
                ${metric('Water level', water)}
              </div>
              ${confidence.length ? `<div class="confidence-row"><span class="meta-label">CONFIDENCE</span>${confidence.map(([label, val]) => `<span class="confidence-chip">${escapeHtml(label)} <strong>${escapeHtml(Math.round(val * 100))}%</strong></span>`).join('')}</div>` : ''}
              ${notes ? `<div class="analysis-notes"><span class="meta-label">ANALYSIS NOTES</span><p>${escapeHtml(notes)}</p></div>` : ''}
            `;
          } else if (job.state === 'failed') {
            bodyContent = `<div class="upload-error"><span>!</span><span><strong>Photo review failed</strong><small>${escapeHtml(job.error || 'The photo review could not be completed.')}</small></span></div>`;
          } else if (job.state === 'uncertain') {
            bodyContent = `<div class="upload-error analysis-uncertain"><span>!</span><span><strong>Outcome uncertain · manual review required</strong><small>${escapeHtml(job.error || 'This request will not retry automatically. Review the photo and record before deciding what to do.')}</small></span></div>`;
          } else if (['submitting', 'submitted', 'processing'].includes(job.state)) {
            bodyContent = '<div class="pending-command analysis-pending"><span><strong>Review in progress</strong><small>Background review is underway.</small></span></div>';
          } else if (job.state === 'queued') {
            bodyContent = '<div class="pending-command analysis-pending"><span><strong>Queued for review</strong><small>This photo is waiting for background review.</small></span></div>';
          } else {
            bodyContent = '<div class="pending-command analysis-pending"><span><strong>Status unavailable</strong><small>No current review status is available.</small></span></div>';
          }

          const photoLink = photoId ? `<button class="text-button job-photo-link" data-action="open-photo" data-photo="${escapeHtml(photoId)}" data-station="${escapeHtml(job.station_id)}">View photo ↗</button>` : '';
          const retryButton = job.state === 'failed' ? `<button class="button secondary retry-analysis" data-action="retry-analysis" data-job="${escapeHtml(job.id)}">Retry review</button>` : '';

          return `<article class="analysis-record">
            <div class="analysis-record-photo">${photoMarkup}</div>
            <div class="analysis-record-main">
              <div class="analysis-record-head">
                <div>
                  <div class="analysis-record-badges">
                    <span class="badge-source cloud">Cloud Analysis</span>
                    ${statePill}
                  </div>
                  <p class="eyebrow">${escapeHtml(stationName(job.station_id))} <span>·</span> ${escapeHtml(job.station_id)}</p>
                  <h3>${escapeHtml(job.filename || 'Field photo')}</h3>
                  <small class="muted">Capture: ${escapeHtml(job.date)} · ${escapeHtml(job.filename)}</small>
                </div>
                <div class="analysis-record-meta">
                  <time title="${job.state === 'completed' ? 'Completion time' : 'Queue time'}">${escapeHtml(formatDate(job.state === 'completed' ? job.updated_at : job.created_at))}</time>
                </div>
              </div>
              ${bodyContent}
              ${photoLink}
              ${retryButton}
            </div>
          </article>`;
        }).join('');
      }
    }

    // Render Historical Archive
    const archiveCountEl = $('#analysis-archive-count');
    if (archiveCountEl) archiveCountEl.textContent = `${historical.length} record${historical.length === 1 ? '' : 's'}`;

    const sortedHistorical = [...historical].sort((a, b) => dateValue(a.record?.completed_at || a.record?.archived_at || a.date || a.record?.date) - dateValue(b.record?.completed_at || b.record?.archived_at || b.date || b.record?.date));
    const charts = [analysisGraph(sortedHistorical, 'tiller_count', 'Recorded tiller count'), analysisGraph(sortedHistorical, 'water_level_cm', 'Recorded water level', ' cm')].filter(Boolean);
    const timeline = $('#analysis-timeline');
    if (timeline) timeline.innerHTML = charts.length ? `<div class="analysis-chart-grid">${charts.join('')}</div>` : '';

    const recordsTarget = $('#analysis-records');
    if (recordsTarget) {
      if (!historical.length) {
        recordsTarget.innerHTML = '<div class="empty-card gallery-empty"><span class="empty-icon">⌁</span><strong>No imported analysis records</strong><p>When legacy analysis history has been imported, its recorded values, confidence, and notes will appear here.</p></div>';
      } else {
        recordsTarget.innerHTML = historical.slice().reverse().map((entry) => {
          const { record, result } = analysisParts(entry);
          const stationId = entry.station_id || record.station_id || '';
          const stage = result.growth_stage || result.stage;
          const count = result.final_tiller_count ?? result.tiller_count ?? result.count;
          const water = result.water_level_cm;
          const confidence = [['Tillers', result.tiller_confidence], ['Growth stage', result.growth_confidence], ['Water', result.water_confidence]].filter(([, value]) => typeof value === 'number' && Number.isFinite(value));
          const notes = result.notes || result.analysis_notes || result.observations || record.notes || '';
          const source = entry.source || entry.source_table || 'Imported history';
          const date = record.completed_at || record.archived_at || entry.date || record.date;
          const matchingPhoto = model.photos.find((photo) => String(photo.station_id) === String(stationId) && photo.date === (entry.date || record.date) && photo.filename === (entry.filename || record.filename));
          const photo = matchingPhoto ? photoThumb(matchingPhoto, 'analysis') : '<div class="analysis-photo-empty">No matching photo in the cloud archive</div>';
          const metric = (label, value, unit = '') => value === null || value === undefined || value === '' ? '' : `<div class="analysis-metric"><span>${escapeHtml(label)}</span><strong>${escapeHtml(value)}${escapeHtml(unit)}</strong></div>`;
          const photoLink = matchingPhoto ? `<button class="text-button job-photo-link" data-action="open-photo" data-photo="${escapeHtml(matchingPhoto.id)}" data-station="${escapeHtml(stationId)}">View photo ↗</button>` : '';

          return `<article class="analysis-record">
            <div class="analysis-record-photo">${photo}</div>
            <div class="analysis-record-main">
              <div class="analysis-record-head">
                <div>
                  <div class="analysis-record-badges">
                    <span class="badge-source archive">Imported Local Archive</span>
                  </div>
                  <p class="eyebrow">${escapeHtml(source)} <span>·</span> ${escapeHtml(stationName(stationId))}</p>
                  <h3>${escapeHtml(entry.filename || record.filename || 'Imported analysis')}</h3>
                </div>
                <time>${escapeHtml(formatDate(date))}</time>
              </div>
              <div class="analysis-metrics">
                ${metric('Tillers', count)}
                ${metric('Growth stage', stage)}
                ${metric('Water level', water, ' cm')}
              </div>
              ${confidence.length ? `<div class="confidence-row"><span class="meta-label">RECORDED CONFIDENCE</span>${confidence.map(([label, value]) => `<span class="confidence-chip">${escapeHtml(label)} <strong>${escapeHtml(Math.round(value * 100))}%</strong></span>`).join('')}</div>` : ''}
              ${notes ? `<div class="analysis-notes"><span class="meta-label">RECORDED NOTES</span><p>${escapeHtml(notes)}</p></div>` : ''}
              ${photoLink}
            </div>
          </article>`;
        }).join('');
      }
    }
  }

  async function refreshAnalyses({ quiet = false } = {}) {
    if (model.analysisBusy) return;
    model.analysisBusy = true;
    try {
      const [analysesData, jobsData] = await Promise.all([
        api('/api/analyses').catch((err) => { if (err.status === 401) throw err; return { analyses: [] }; }),
        api('/api/analysis/jobs').catch((err) => { if (err.status === 401) throw err; return { enabled: false, jobs: [] }; })
      ]);
      model.analyses = Array.isArray(analysesData.analyses) ? analysesData.analyses : (Array.isArray(analysesData.rows) ? analysesData.rows : []);
      model.analysisJobs = Array.isArray(jobsData.jobs) ? jobsData.jobs : [];
      if (typeof jobsData.enabled === 'boolean') {
        model.analysisEnabled = jobsData.enabled;
      }
      renderAnalysis();
    } catch (error) {
      if (error.status === 401) { csrf.token = ''; showLogin(); }
      else if (!quiet || (!model.analyses.length && !model.analysisJobs.length)) {
        const target = $('#analysis-records');
        if (target) {
          target.innerHTML = `<div class="empty-card error-card"><span class="empty-icon">!</span><strong>Could not load analysis records</strong><p>${escapeHtml(error.message)}</p><button class="button secondary" data-action="refresh-analyses">Try again</button></div>`;
        }
      }
    } finally {
      model.analysisBusy = false;
    }
  }

  function render() { renderStations(); renderPhotos(); }

  function showToast(message, isError = false) {
    const toast = $('#toast');
    toast.textContent = message;
    toast.classList.toggle('error', isError);
    toast.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => toast.classList.remove('show'), 3800);
  }

  async function queueCommand(stationId, type, payload = {}) {
    try {
      await api(`/api/stations/${encodeURIComponent(stationId)}/commands`, { method: 'POST', body: { type, payload } });
      showToast('Command queued. It will be delivered at the station’s next wake.');
      await refreshState({ quiet: true });
    } catch (error) { showToast(`Could not queue command: ${error.message}`, true); }
  }

  async function cancelCommand(stationId, commandId) {
    try {
      await api(`/api/stations/${encodeURIComponent(stationId)}/commands/${encodeURIComponent(commandId)}/cancel`, { method: 'POST', body: {} });
      showToast('Queued command cancelled.');
      await refreshState({ quiet: true });
    } catch (error) { showToast(`Could not cancel command: ${error.message}`, true); }
  }

  async function retryAnalysisJob(jobId) {
    try {
      await api(`/api/analysis/jobs/${encodeURIComponent(jobId)}/retry`, { method: 'POST', body: {} });
      showToast('Photo review queued again.');
      await refreshAnalyses({ quiet: true });
    } catch (error) { showToast(`Could not retry photo review: ${error.message}`, true); }
  }

  function switchView(view) {
    const gallery = view === 'gallery';
    const analysis = view === 'analysis';
    $('#overview-view').hidden = gallery || analysis;
    $('#gallery-view').hidden = !gallery;
    $('#analysis-view').hidden = !analysis;
    $('#page-title').textContent = gallery ? 'Photo gallery' : analysis ? 'Analysis history' : 'Overview';
    $$('.nav-link[data-view]').forEach((button) => button.classList.toggle('active', button.dataset.view === view));
    $('#sidebar').classList.remove('open');
    $('#menu-button').setAttribute('aria-expanded', 'false');
    if (gallery) renderGallery();
    if (analysis && !model.analyses.length && !model.analysisJobs.length) refreshAnalyses();
  }

  function openPhoto(photoId, stationId = '') {
    let photo = model.photos.find((item) => String(item.id) === String(photoId));
    if (!photo && photoId) photo = { id: photoId, url: `/api/photos/${encodeURIComponent(photoId)}`, station_id: stationId };
    if (!photo) {
      photo = getLatestPhoto(stationById(stationId) || {});
    }
    const url = safePhotoUrl(photo?.url || (photo?.id ? `/api/photos/${encodeURIComponent(photo.id)}` : ''));
    if (!url) return showToast('This image is not available.', true);
    $('#photo-large').src = url;
    $('#photo-caption').textContent = `${stationName(photo.station_id || stationId)} · ${formatDate(photo.uploaded_at || photo.date)}`;
    $('#photo-dialog').showModal();
  }

  function openStationDetails(id) {
    const station = stationById(id);
    if (!station) return;
    const contents = $('#station-dialog-content');
    const latest = getLatestPhoto(station);
    const errors = station.upload_error || station.last_upload_error || station.upload_message;
    contents.innerHTML = `<p class="eyebrow">STATION DETAILS</p><h2>${escapeHtml(station.name || `Station ${id}`)}</h2><p class="muted">Camera station ${escapeHtml(id)}</p><div class="detail-grid"><div><span class="meta-label">REPORTED STATE</span>${statusMarkup(station)}</div><div><span class="meta-label">LAST SEEN</span><strong>${escapeHtml(formatDate(station.last_seen))}</strong></div><div><span class="meta-label">NEXT EXPECTED WAKE</span><strong>${escapeHtml(formatDate(station.next_wake_at))}</strong></div><div><span class="meta-label">APPLIED SLEEP SETTING</span><strong>${station.sleep_enabled === true ? 'Enabled' : station.sleep_enabled === false ? 'Disabled' : 'Not reported'}</strong></div></div>${errors ? `<div class="upload-error detail-error"><span>!</span><span><strong>Upload error reported</strong><small>${escapeHtml(errors)}</small></span></div>` : '<p class="empty-inline">No upload error is currently reported by this station.</p>'}<div class="detail-photo">${photoThumb(latest, 'station')}</div><h3>Pending commands</h3>${pendingMarkup(station)}<p class="delivery-hint modal-hint">Queued commands are delivered at the station’s next wake. A physical reset wakes it immediately.</p>`;
    $('#station-dialog').showModal();
  }

  $('#login-form').addEventListener('submit', async (event) => {
    event.preventDefault();
    setLoginError();
    const button = $('button[type="submit"]', event.currentTarget);
    button.disabled = true;
    try {
      const result = await api('/api/login', { method: 'POST', body: { password: $('#password').value } });
      csrf.token = result.csrf_token || '';
      $('#password').value = '';
      showApp();
      await refreshState();
      await refreshAnalyses();
      startPolling();
    } catch (error) { setLoginError(error.status === 401 ? 'That password did not match. Try again.' : `Sign in failed: ${error.message}`); }
    finally { button.disabled = false; }
  });

  $('#logout-button').addEventListener('click', async () => {
    try { await api('/api/logout', { method: 'POST', body: {} }); }
    catch (error) { showToast(`Could not close session: ${error.message}`, true); return; }
    csrf.token = '';
    model.stations = []; model.photos = []; model.analyses = []; model.analysisJobs = []; model.lastUpdated = null;
    showLogin();
    showToast('Admin session closed.');
  });

  $('#refresh-button').addEventListener('click', () => refreshState());
  $$('[data-view]').forEach((button) => button.addEventListener('click', () => switchView(button.dataset.view)));
  $('#menu-button').addEventListener('click', () => {
    const open = $('#sidebar').classList.toggle('open');
    $('#menu-button').setAttribute('aria-expanded', String(open));
  });
  $('#cloud-settings-form').addEventListener('submit', async (event) => {
    event.preventDefault();
    if (!model.stations.length) return showToast('No stations are available to receive settings.', true);
    const intervalSeconds = Math.round(Number($('#interval-hours').value) * 3600);
    if (!Number.isFinite(intervalSeconds) || intervalSeconds < 3600 || intervalSeconds > 604800) return showToast('Choose an interval between 1 and 168 hours.', true);
    const payload = {
      sleep_enabled: $('#cloud-sleep').checked,
      timer_enabled: $('#timer-enabled').checked,
      interval_seconds: intervalSeconds,
      window_enabled: $('#window-enabled').checked,
      window_start: $('#window-start').value,
      window_end: $('#window-end').value
    };
    const feedback = $('#settings-feedback');
    feedback.textContent = 'Queueing settings for each station…';
    const results = await Promise.allSettled(model.stations.map((station) => api(`/api/stations/${encodeURIComponent(station.station_id)}/commands`, { method: 'POST', body: { type: 'set_settings', payload } })));
    const succeeded = results.filter((result) => result.status === 'fulfilled').length;
    if (succeeded) model.settingsDirty = true;
    feedback.textContent = `${succeeded} of ${model.stations.length} station${model.stations.length === 1 ? '' : 's'} queued · awaiting station reports to confirm applied values.`;
    if (succeeded) await refreshState({ quiet: true });
    showToast(succeeded === model.stations.length ? `Settings requested for ${succeeded} station${succeeded === 1 ? '' : 's'}.` : `Settings queued for ${succeeded} of ${model.stations.length} stations.`, succeeded === 0);
  });
  $('#cloud-settings-form').addEventListener('input', () => { model.settingsDirty = true; });
  ['photo-search', 'station-filter', 'date-filter'].forEach((id) => $(`#${id}`).addEventListener('input', renderGallery));
  document.addEventListener('click', (event) => {
    const button = event.target.closest('[data-action]');
    if (!button) return;
    const { action, station, command, photo, job } = button.dataset;
    if (action === 'refresh') refreshState();
    if (action === 'refresh-analyses') refreshAnalyses();
    if (action === 'take-photo') queueCommand(station, 'take_photo');
    if (action === 'stay-awake') queueCommand(station, 'stay_awake');
    if (action === 'resume-sleep') queueCommand(station, 'resume_sleep');
    if (action === 'cancel') cancelCommand(station, command);
    if (action === 'retry-analysis') retryAnalysisJob(job);
    if (action === 'station-details') openStationDetails(station);
    if (action === 'open-photo') openPhoto(photo, station);
    if (action === 'clear-filters') { $('#photo-search').value = ''; $('#station-filter').value = 'all'; $('#date-filter').value = 'all'; renderGallery(); }
  });

  initialize();
})();
