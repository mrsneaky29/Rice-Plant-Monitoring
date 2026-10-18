#ifndef WEB_PAGE_H
#define WEB_PAGE_H

static const char PAGE_INDEX_HTML[] = R"HTMLPAGE(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32-CAM Monitoring</title>
<style>
  :root {
    --bg1:#0f1123; --bg2:#1a1040; --card:rgba(255,255,255,0.06);
    --border:rgba(255,255,255,0.10); --accent:#8b5cf6; --accent2:#06b6d4;
    --green:#22c55e; --red:#ef4444; --amber:#f59e0b;
    --text:#f1f5f9; --muted:#94a3b8;
  }
  * { box-sizing:border-box; margin:0; }
  body {
    padding:16px; color:var(--text); min-height:100vh;
    font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
    background:linear-gradient(160deg,var(--bg1) 0%,var(--bg2) 55%,#0c1e3e 100%);
    background-attachment:fixed;
  }
  .wrap { max-width:560px; margin:0 auto; display:flex; flex-direction:column; gap:14px; }

  header { text-align:center; padding:6px 0 2px; }
  h1 {
    font-size:22px; font-weight:800; letter-spacing:0.3px;
    background:linear-gradient(90deg,var(--accent2),var(--accent));
    -webkit-background-clip:text; background-clip:text; color:transparent;
  }
  .livewrap { display:inline-flex; align-items:center; gap:6px; margin-top:6px; padding:4px 12px; border-radius:999px; background:rgba(0,0,0,0.25); border:1px solid var(--border); }
  .dot { width:9px; height:9px; border-radius:50%; background:var(--red); flex:0 0 auto; }
  .dot.online { background:var(--green); animation:blink 1s step-start infinite; }
  @keyframes blink { 50% { opacity:0.15; } }
  .livetext { font-size:11px; font-weight:800; letter-spacing:0.6px; color:var(--red); }
  .livetext.online { color:var(--green); }

  .infogrid {
    display:grid; grid-template-columns:repeat(2,1fr); gap:8px; margin-top:12px;
  }
  .infocell {
    background:var(--card); border:1px solid var(--border); border-radius:12px;
    padding:8px 10px; text-align:left;
  }
  .infocell .k { font-size:10px; color:var(--muted); text-transform:uppercase; letter-spacing:0.5px; }
  .infocell .v { font-size:14px; font-weight:700; margin-top:2px; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .timeDate { font-family:inherit; color:var(--muted); font-weight:600; font-size:13px; }
  .timeClock {
    font-family:"SF Mono",SFMono-Regular,Consolas,"Roboto Mono",monospace;
    color:var(--accent2); font-weight:800; margin-left:12px; letter-spacing:0.5px; font-size:15px;
  }

  .card {
    background:var(--card); border:1px solid var(--border); border-radius:16px;
    padding:16px; backdrop-filter:blur(8px);
  }
  .row { display:flex; align-items:center; justify-content:space-between; gap:12px; flex-wrap:wrap; }
  .card h2 { font-size:15px; font-weight:700; }
  .card h2 span { margin-right:6px; }

  button {
    border:none; border-radius:10px; padding:10px 16px; font-size:14px; font-weight:700;
    cursor:pointer; color:#fff; background:linear-gradient(90deg,var(--accent),var(--accent2));
  }
  button.active { background:var(--red); }
  button:disabled { background:#334155; color:var(--muted); cursor:not-allowed; }
  button.ghost { background:transparent; border:1px solid var(--border); color:var(--muted); padding:8px 12px; }
  button.small { padding:7px 11px; font-size:12px; }

  .stream-box { width:100%; aspect-ratio:4/3; background:#000; border-radius:12px; overflow:hidden; display:flex; align-items:center; justify-content:center; margin-top:12px; }
  .stream-box img { width:100%; height:100%; object-fit:contain; display:block; }
  .placeholder { color:var(--muted); font-size:14px; }

  .btn-capture {
    background:linear-gradient(90deg,#16a34a,#22c55e); flex:1; min-width:0; padding:14px; font-size:16px;
  }
  .thumb { width:76px; height:57px; border-radius:10px; object-fit:cover; background:#000; border:1px solid var(--border); flex:0 0 auto; }
  .flash-toggle {
    flex:0 0 auto; width:48px; height:48px; padding:0; font-size:19px;
    background:rgba(0,0,0,0.3); border:1px solid var(--border); color:var(--muted);
  }
  .flash-toggle.on { background:linear-gradient(90deg,#f59e0b,#fbbf24); color:#1a1040; border-color:transparent; }
  .note { color:var(--muted); font-size:12px; text-align:center; margin-top:8px; min-height:14px; }

  .intervalRow { display:flex; align-items:center; gap:10px; margin-top:14px; }
  .intervalRow .stat-label { flex:0 0 auto; }
  .intervalRow input { flex:0 0 auto; }
  .intervalEcho { color:var(--accent2); font-size:12px; font-weight:600; margin-top:10px; }

  .seg { display:flex; gap:4px; margin-top:12px; padding:3px; border-radius:10px; background:rgba(0,0,0,0.3); border:1px solid var(--border); }
  .seg-opt {
    flex:1; background:transparent; color:var(--muted); font-size:12px; font-weight:700;
    padding:7px 8px; border-radius:8px;
  }
  .seg-opt.active { background:linear-gradient(90deg,var(--accent),var(--accent2)); color:#fff; }
  #windowInputs input[type=time] { flex:1; min-width:0; }
  #windowInputs .stat-label { flex:0 0 auto; }

  .divider { height:1px; background:var(--border); margin:18px 0 14px; }
  .subhead { font-size:14px; font-weight:700; display:flex; align-items:center; }
  .subhead span { margin-right:6px; }

  input[type=text], input[type=date], input[type=time], input[type=password] {
    padding:8px 10px; border-radius:10px; border:1px solid var(--border);
    background:rgba(0,0,0,0.35); color:var(--text); font-size:14px; text-align:center;
  }
  input[type=text] { width:110px; }
  .hint { color:var(--muted); font-size:11px; margin-top:8px; }

  select {
    padding:8px 10px; border-radius:10px; border:1px solid var(--border);
    background:rgba(0,0,0,0.35); color:var(--text); font-size:14px;
  }

  details.card { padding:0; overflow:hidden; }
  details.card summary {
    list-style:none; cursor:pointer; padding:16px; display:flex; align-items:center; justify-content:space-between;
  }
  details.card summary::-webkit-details-marker { display:none; }
  details.card summary .chev { color:var(--muted); font-size:12px; transition:transform 0.15s; }
  details.card[open] summary .chev { transform:rotate(90deg); }
  details.card .body { padding:0 16px 16px; }

  details.nested { border:1px solid var(--border); border-radius:12px; margin-top:12px; background:rgba(0,0,0,0.18); overflow:hidden; }
  details.nested summary {
    list-style:none; cursor:pointer; padding:10px 12px; display:flex; align-items:center; justify-content:space-between;
    font-size:13px; font-weight:700; color:var(--muted);
  }
  details.nested summary::-webkit-details-marker { display:none; }
  details.nested summary .chev { font-size:11px; transition:transform 0.15s; }
  details.nested[open] summary .chev { transform:rotate(90deg); }
  details.nested .nbody { padding:0 12px 12px; }

  .filters { display:flex; flex-direction:column; gap:10px; }
  .frow { display:flex; align-items:center; gap:8px; flex-wrap:wrap; }
  .frow label { font-size:11px; color:var(--muted); min-width:64px; }
  .frow input, .frow select { flex:1; min-width:100px; }

  /* Always-visible select-all / batch action bar */
  .abar { display:flex; align-items:center; justify-content:space-between; gap:8px; padding:10px 12px; border-radius:12px; background:rgba(0,0,0,0.2); border:1px solid var(--border); flex-wrap:wrap; }
  .abar.active { background:rgba(34,197,94,0.10); border-color:rgba(34,197,94,0.35); }
  .abar .cnt { font-size:13px; font-weight:700; display:flex; align-items:center; gap:8px; }
  .abar .btns { display:flex; gap:8px; flex-wrap:wrap; }
  .abar button { padding:8px 12px; font-size:13px; }
  .btn-danger { background:linear-gradient(90deg,#dc2626,#ef4444); }

  .checkbox { width:19px; height:19px; accent-color:var(--green); flex:0 0 auto; cursor:pointer; }

  .plist-wrap { max-height:340px; overflow-y:auto; margin-top:12px; border-radius:12px; -webkit-overflow-scrolling:touch; }
  .plist { display:flex; flex-direction:column; gap:6px; }
  .prow {
    display:flex; align-items:center; gap:10px;
    padding:9px 12px; border-radius:10px; background:rgba(0,0,0,0.25);
    border:1px solid var(--border); font-size:13px; min-height:20px;
  }
  .prow.sel { border-color:var(--green); background:rgba(34,197,94,0.14); }
  .prow .nm { flex:1; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; cursor:pointer; }
  .prow .sz { color:var(--muted); font-size:12px; flex:0 0 auto; }
  .prow .dlbtn { padding:6px; width:30px; height:30px; flex:0 0 auto; display:flex; align-items:center; justify-content:center; }
  .prow .dlbtn svg { width:15px; height:15px; }

  .pager { display:flex; align-items:center; justify-content:center; gap:12px; margin-top:12px; }
  .pager span { color:var(--muted); font-size:13px; min-width:100px; text-align:center; }
  .empty { color:var(--muted); font-size:13px; text-align:center; padding:16px 0; }

  .warn { background:rgba(239,68,68,0.14); border:1px solid rgba(239,68,68,0.4); color:#fca5a5; border-radius:12px; padding:10px; font-size:13px; text-align:center; display:none; }
  .warn.show { display:block; }

  /* Hold-to-confirm buttons (Erase All, Reboot) */
  .holdBtn { position:relative; overflow:hidden; }
  .holdBtn .holdFill { position:absolute; left:0; top:0; bottom:0; width:0%; background:rgba(255,255,255,0.4); }
  .holdBtn .label { position:relative; z-index:1; }

  .storageLine { display:flex; align-items:center; justify-content:space-between; gap:10px; flex-wrap:wrap; font-size:13px; }
  .storageLine .stat-value { font-size:14px; }

  .wifi-list { display:flex; flex-direction:column; gap:6px; margin-top:10px; }
  .wifi-row { display:flex; align-items:center; justify-content:space-between; gap:8px; padding:8px 10px; border-radius:10px; background:rgba(0,0,0,0.25); border:1px solid var(--border); font-size:13px; }
  .wifi-row .name { display:flex; align-items:center; gap:8px; overflow:hidden; }
  .wifi-row .badge { font-size:10px; font-weight:800; color:var(--green); border:1px solid var(--green); border-radius:999px; padding:1px 7px; flex:0 0 auto; }
  .wifi-row .del { padding:5px 9px; font-size:12px; flex:0 0 auto; }
  .wifi-add { display:flex; flex-direction:column; gap:8px; margin-top:12px; }
  .wifi-add input { width:100%; text-align:left; }

  .dhtReading { display:flex; flex-direction:column; gap:2px; }
  .dhtVal { font-size:20px; font-weight:800; }
  .dhtVal b { color:var(--accent2); }

  .chart-wrap { margin-top:12px; }
  #dhtChart { width:100%; height:220px; display:block; border-radius:12px; background:rgba(0,0,0,0.25); border:1px solid var(--border); touch-action:none; cursor:grab; }
  .chart-legend { display:flex; align-items:center; gap:14px; flex-wrap:wrap; margin-top:8px; font-size:11px; color:var(--muted); }
  .chart-legend i { display:inline-block; width:10px; height:10px; border-radius:50%; margin-right:5px; vertical-align:middle; }
  .chart-legend i.dot-c { background:#f97316; }
  .chart-legend i.dot-h { background:#38bdf8; }

  .logbox {
    margin-top:10px; max-height:280px; overflow-y:auto; padding:10px 12px; border-radius:12px;
    background:#0b1220; border:1px solid var(--border); font-family:"SF Mono",Consolas,monospace;
    font-size:11px; line-height:1.45; color:#9ae6b4; white-space:pre-wrap; word-break:break-word;
  }

  #modal {
    display:none; position:fixed; inset:0; background:rgba(0,0,0,0.85); z-index:50;
    flex-direction:column; align-items:center; justify-content:center; gap:14px; padding:16px;
  }
  #modal.show { display:flex; }
  #modal img { max-width:100%; max-height:70vh; border-radius:12px; background:#000; }
  #modal .mname { color:var(--text); font-size:13px; font-weight:600; word-break:break-all; text-align:center; }
  #modal .mbtns { display:flex; gap:10px; flex-wrap:wrap; justify-content:center; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>ESP32-CAM Monitoring</h1>
    <div class="livewrap">
      <span id="liveDot" class="dot"></span>
      <span id="liveText" class="livetext">OFFLINE</span>
    </div>
    <div class="infogrid">
      <div class="infocell"><div class="k">WiFi</div><div class="v" id="chipSsid">-</div></div>
      <div class="infocell"><div class="k">Signal</div><div class="v" id="chipRssi">-</div></div>
      <div class="infocell"><div class="k">IP Address</div><div class="v" id="chipIp">-</div></div>
      <div class="infocell"><div class="k">mDNS</div><div class="v">monitoring.local</div></div>
      <div class="infocell" style="grid-column:1/3;">
        <div class="k">Device Time</div>
        <div class="v"><span id="chipDate" class="timeDate">-</span><span id="chipClock" class="timeClock">-</span></div>
      </div>
    </div>
  </header>

  <div id="cameraWarn" class="warn">Camera not detected &mdash; check the ribbon cable, then reboot from Advanced Settings</div>
  <div id="sdWarn" class="warn">SD card not detected &mdash; insert a card and reboot</div>
  <div id="fullWarn" class="warn">SD card full &mdash; capture stopped</div>
  <div id="eraseWarn" class="warn">Erasing all photos&hellip; captures are paused</div>
  <div id="rebootWarn" class="warn">Rebooting&hellip; the page will reconnect automatically</div>

  <div class="card">
    <div class="row">
      <h2><span>&#128187;</span>Console</h2>
      <button class="ghost small" onclick="loadDeviceLog(true)">&#8635;</button>
    </div>
    <pre id="deviceLogBox" class="logbox">Waiting for device logs&hellip;</pre>
  </div>

  <div class="card">
    <div class="row">
      <h2><span>&#9889;</span>Soil moisture (ADS1115)</h2>
      <span class="hint" id="adsStatus">looking for chip&hellip;</span>
    </div>
    <div class="hint" style="margin-top:6px;">Sensor is dry/open at ~3.3 V and wet/short at ~0 V. Display is inverted: higher % = wetter.</div>
    <div class="infogrid" style="margin-top:10px;">
      <div class="infocell" style="grid-column:1/-1;"><div class="k">A0 moisture</div><div class="v" id="adsA0">--</div></div>
    </div>
  </div>

  <div class="card">
    <div class="row">
      <h2><span>&#128249;</span>Live View</h2>
      <div style="display:flex;gap:8px;">
        <button id="torchBtn" class="flash-toggle" onclick="toggleTorch()" title="Torch (steady light)">&#128161;</button>
        <button id="streamBtn" onclick="toggleStream()">Start</button>
      </div>
    </div>
    <div class="stream-box">
      <span id="streamPlaceholder" class="placeholder">Live view off</span>
      <img id="streamImg" style="display:none;">
    </div>
  </div>

  <div class="card">
    <h2 style="margin-bottom:12px;"><span>&#128248;</span>Capture</h2>
    <div class="row">
      <img id="thumb" class="thumb" style="display:none;">
      <button id="captureBtn" class="btn-capture" onclick="takePhoto()">&#128248; Take Photo</button>
      <button id="flashBtn" class="flash-toggle" onclick="toggleFlash()" title="Flash on capture">&#9889;</button>
    </div>
    <div id="captureNote" class="note"></div>

    <div class="divider"></div>

    <div class="row">
      <span class="subhead"><span>&#9200;</span>Scheduled Capture</span>
      <button id="timerBtn" onclick="toggleTimer()">Enable</button>
    </div>
    <div id="scheduleControls">
      <div class="intervalRow">
        <span class="stat-label">Every</span>
        <input type="text" id="interval" value="10s" placeholder="10s / 5m / 1h">
      </div>

      <div class="seg" id="windowSeg">
        <button type="button" class="seg-opt active" id="segAllDay" onclick="setWindowMode(false)">All day</button>
        <button type="button" class="seg-opt" id="segWindow" onclick="setWindowMode(true)">Time window</button>
      </div>
      <div class="intervalRow" id="windowInputs" style="display:none;">
        <span class="stat-label">From</span>
        <input type="time" id="winStart" value="06:00">
        <span class="stat-label">to</span>
        <input type="time" id="winEnd" value="20:00">
      </div>
      <button class="small" style="width:100%;margin-top:10px;" onclick="updateCaptureSchedule()">Update</button>
    </div>

    <div id="intervalEcho" class="intervalEcho"></div>
    <div class="hint">Formats: <b>30s</b>, <b>10m</b>, <b>2h</b>, <b>1h30m</b> or plain seconds. Fields are always editable; hit Update to save changes (Enable/Disable applies them too).</div>
  </div>

  <div class="card">
    <div class="row">
      <h2><span>&#9728;</span>Cloud power</h2>
      <button id="sleepBtn" onclick="toggleSleepMode()">Enable sleep</button>
    </div>
    <div id="cloudStatus" class="hint">Cloud is not configured.</div>
    <div class="row" style="margin-top:10px;">
      <button class="small" onclick="setAwakeSession(true)">Stay awake until reboot</button>
      <button class="ghost small" onclick="setAwakeSession(false)">Resume sleep</button>
    </div>
  </div>

  <details class="card" id="galleryCard">
    <summary>
      <h2><span>&#128444;</span>Gallery</h2>
      <span style="display:flex;align-items:center;gap:10px;">
        <button type="button" class="ghost small" id="galleryRefreshBtn" title="Refresh gallery" style="padding:6px;width:30px;height:30px;display:flex;align-items:center;justify-content:center;">
          <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" width="15" height="15"><path d="M21 12a9 9 0 1 1-2.64-6.36"/><path d="M21 3v6h-6"/></svg>
        </button>
        <span class="chev">&#9656;</span>
      </span>
    </summary>
    <div class="body">

      <div class="abar" id="abar">
        <span class="cnt">
          <input type="checkbox" class="checkbox" id="selectAllBox" onclick="onSelectAllToggle(this.checked)">
          <span id="selCount">0</span> selected
        </span>
        <div class="btns">
          <button class="small" id="zipBtn" onclick="zipSelected()" disabled>&#8681; ZIP</button>
          <button class="small btn-danger" id="delBtn" onclick="deleteSelected()" disabled>Delete</button>
        </div>
      </div>

      <details class="nested" id="filtersDetails">
        <summary>Filters <span class="chev">&#9656;</span></summary>
        <div class="nbody">
          <div class="filters">
            <div class="frow"><label>From date</label><input type="date" id="fDateFrom"></div>
            <div class="frow"><label>To date</label><input type="date" id="fDateTo"></div>
            <div class="frow"><label>From time</label><input type="time" id="fTimeFrom" step="1"></div>
            <div class="frow"><label>To time</label><input type="time" id="fTimeTo" step="1"></div>
            <div class="frow"><label>Per page</label>
              <select id="perSel" onchange="onPerChange()">
                <option value="10">10</option>
                <option value="25">25</option>
                <option value="50">50</option>
                <option value="100">100</option>
                <option value="100000">Unlimited</option>
              </select>
            </div>
            <div class="frow" style="justify-content:flex-end;">
              <button class="small" onclick="applyFilters()">Apply</button>
              <button class="ghost small" onclick="resetFilters()">Reset to default</button>
            </div>
          </div>
        </div>
      </details>

      <div class="plist-wrap"><div id="plist" class="plist"><div class="empty">Loading&hellip;</div></div></div>
      <div class="pager">
        <button class="ghost" id="prevBtn" onclick="pageStep(1)">&larr; Older</button>
        <span id="pageInfo">-</span>
        <button class="ghost" id="nextBtn" onclick="pageStep(-1)">Newer &rarr;</button>
      </div>
    </div>
  </details>

  <details class="card" id="dhtCard">
    <summary><h2><span>&#127777;&#65039;</span>Environment Sensor</h2><span class="chev">&#9656;</span></summary>
    <div class="body">

      <div class="row">
        <div class="dhtReading">
          <span class="dhtVal"><b id="dhtTemp">--</b>&deg;C</span>
          <span class="dhtVal"><b id="dhtHum">--</b>% RH</span>
          <span class="hint" id="dhtAge" style="margin:0;">no reading yet</span>
        </div>
        <button class="small" onclick="dhtCaptureNow()">Capture Now</button>
      </div>

      <div class="divider"></div>

      <div class="row">
        <span class="subhead"><span>&#9200;</span>Scheduled Reading</span>
        <button id="dhtTimerBtn" onclick="dhtToggleTimer()">Enable</button>
      </div>
      <div id="dhtScheduleControls">
        <div class="intervalRow">
          <span class="stat-label">Every</span>
          <input type="text" id="dhtInterval" value="1m" placeholder="30s / 1m / 5m">
        </div>
        <div class="seg" id="dhtWindowSeg">
          <button type="button" class="seg-opt active" id="dhtSegAllDay" onclick="dhtSetWindowMode(false)">All day</button>
          <button type="button" class="seg-opt" id="dhtSegWindow" onclick="dhtSetWindowMode(true)">Time window</button>
        </div>
        <div class="intervalRow" id="dhtWindowInputs" style="display:none;">
          <span class="stat-label">From</span>
          <input type="time" id="dhtWinStart" value="06:00">
          <span class="stat-label">to</span>
          <input type="time" id="dhtWinEnd" value="20:00">
        </div>
        <button class="small" style="width:100%;margin-top:10px;" onclick="dhtUpdateSchedule()">Update</button>
      </div>
      <div id="dhtIntervalEcho" class="intervalEcho"></div>
      <div class="hint">Formats: <b>30s</b>, <b>1m</b>, <b>5m</b>, <b>1h</b> or plain seconds. Fields are always editable; hit Update to save changes.</div>

      <div class="divider"></div>

      <div class="row">
        <span class="subhead"><span>&#128203;</span>Recent Readings</span>
        <div style="display:flex;gap:8px;">
          <button class="ghost small" style="color:var(--red);border-color:var(--red);" onclick="dhtEraseAll()">Delete All</button>
          <button class="ghost small" onclick="dhtLoadRecent()">&#8635;</button>
        </div>
      </div>
      <div class="plist-wrap" style="max-height:220px;"><div id="dhtRecentList" class="plist"><div class="empty">Loading&hellip;</div></div></div>

      <div class="divider"></div>

      <span class="subhead"><span>&#128200;</span>History &amp; Export</span>
      <div class="filters" style="margin-top:10px;">
        <div class="frow"><label>From date</label><input type="date" id="dhtDateFrom"></div>
        <div class="frow"><label>To date</label><input type="date" id="dhtDateTo"></div>
        <div class="frow"><label>From time</label><input type="time" id="dhtTimeFrom" step="1"></div>
        <div class="frow"><label>To time</label><input type="time" id="dhtTimeTo" step="1"></div>
        <div class="frow" style="justify-content:flex-end;">
          <button class="small" onclick="dhtLoadHistory()">Load</button>
          <button class="ghost small" onclick="dhtExportCsv()">Export CSV</button>
        </div>
      </div>
      <div id="dhtHistoryNote" class="note"></div>
      <div class="chart-wrap">
        <canvas id="dhtChart" height="220"></canvas>
        <div class="chart-legend">
          <span><i class="dot-c"></i>Temperature (&deg;C)</span>
          <span><i class="dot-h"></i>Humidity (%)</span>
          <span class="hint" style="margin:0;">Scroll to zoom &middot; drag to pan</span>
          <button class="ghost small" onclick="dhtResetZoom()">Reset zoom</button>
        </div>
      </div>

    </div>
  </details>

  <details class="card" id="waterCard">
    <summary><h2><span>&#127807;</span>Rice Plant Analysis (AI Vision)</h2><span class="chev">&#9656;</span></summary>
    <div class="body">

      <div class="row">
        <span class="subhead"><span>&#129504;</span>AI Vision</span>
        <button id="waterVisionBtn" onclick="waterToggleVision()">Enable</button>
      </div>
      <div class="hint">When enabled, every newly captured photo is analyzed for water level, tiller count, and rice growth stage. Results and confidence values are stored on the SD card and appear beside the source photograph in the Gallery. Unclear measurements are reported as unreadable instead of guessed.</div>

      <div class="divider"></div>

      <div class="subhead"><span>&#128273;</span>OpenAI API Key</div>
      <div class="hint" style="margin-top:6px;">Stored on the device (not the SD card), never sent back to this page once saved.</div>
      <div class="row" style="margin-top:10px;flex-wrap:nowrap;">
        <input type="password" id="waterApiKey" placeholder="sk-..." style="flex:1;text-align:left;">
        <button class="small" onclick="waterSaveApiKey()">Save</button>
      </div>
      <div class="hint" id="waterKeyStatus" style="margin-top:6px;"></div>

      <div class="divider"></div>

      <div class="row">
        <span class="subhead"><span>&#128203;</span>Analysis History</span>
        <button class="ghost small" style="color:var(--red);border-color:var(--red);" onclick="waterEraseAll()">Delete All</button>
      </div>
      <div class="hint">Analyses are shown next to their source photograph in the Gallery below. This button clears analysis history only; photographs remain on the card.</div>

    </div>
  </details>

  <details class="card" id="advancedCard">
    <summary><h2><span>&#9881;&#65039;</span>Advanced Settings</h2><span class="chev">&#9656;</span></summary>
    <div class="body">

      <div class="subhead"><span>&#128190;</span>Storage</div>
      <div class="storageLine" style="margin-top:10px;">
        <span><span id="advPhotoCount" class="stat-value">-</span> photos &bull; <span id="advFreeSpace" class="stat-value">-</span>/<span id="advTotalSpace" class="stat-value">-</span> MB free</span>
        <button id="eraseBtn" class="btn-danger holdBtn" title="Press and hold 10 seconds">
          <span class="holdFill"></span><span class="label">Hold 10s: Erase All</span>
        </button>
      </div>
      <div class="hint">Erasing runs in the background and deletes every photo and date folder on the SD card.</div>

      <div class="divider"></div>

      <div class="subhead"><span>&#128246;</span>WiFi Settings</div>
      <div class="hint" style="margin-top:6px;">Saved networks are stored on the device (not the SD card). On boot or reconnect it joins whichever saved network has the strongest signal.</div>
      <div id="wifiList" class="wifi-list"><div class="empty">Loading&hellip;</div></div>
      <div class="wifi-add">
        <input type="text" id="wifiSsid" placeholder="Network name (SSID)">
        <input type="password" id="wifiPass" placeholder="Password">
        <button class="small" onclick="addWifi()">Add Network</button>
      </div>

      <div class="divider"></div>

      <div class="subhead"><span>&#128260;</span>Reboot Device</div>
      <div class="row" style="margin-top:10px;">
        <span class="hint" style="margin:0;">Restarts the ESP32. Live view and capture pause for a few seconds.</span>
        <button id="rebootBtn" class="btn-danger holdBtn" title="Press and hold 10 seconds">
          <span class="holdFill"></span><span class="label">Hold 10s: Reboot</span>
        </button>
      </div>

    </div>
  </details>
</div>

<div id="modal" onclick="if(event.target===this)closeModal()">
  <img id="modalImg">
  <div class="mname" id="modalName"></div>
  <div class="mname" id="modalAnalysisStatus" style="color:var(--muted);"></div>
  <div class="mbtns">
    <button id="modalAnalyzeBtn" onclick="manualAnalyzeCurrent()">&#10024; Analyze photo</button>
    <button onclick="downloadCurrent()">&#8681; Download</button>
    <button class="btn-danger" onclick="deleteCurrent()">Delete</button>
    <button class="ghost" onclick="closeModal()">Close</button>
  </div>
</div>

<script>
var DL_SVG = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M12 3v12"/><path d="M7 10l5 5 5-5"/><path d="M5 21h14"/></svg>';

var streaming = false, timerEnabled = false, erasing = false, flashEnabled = true, torchOn = false;
var dhtTimerEnabled = false, lastDhtReadingEpoch = null;
var waterVisionEnabled = false, openAiKeySet = false;
var lastKnownPhotoCount = null;  // used to auto-refresh the gallery when scheduled captures add photos
var lastStatusOkAt = 0;

function $(id) { return document.getElementById(id); }
function api(path) { return fetch(path).then(function (r) { return r.json(); }); }
function escapeHtml(value) {
  return String(value).replace(/[&<>"']/g, function (c) {
    return ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'})[c];
  });
}
function pad2(n) { return (n < 10 ? '0' : '') + n; }

function streamUrl() {
  return window.location.protocol + '//' + window.location.hostname + ':81/stream';
}

/* --- Live indicator: green flash while /status is answering, red + OFFLINE otherwise --- */
function setOnline(ok) {
  $('liveDot').classList.toggle('online', ok);
  $('liveText').classList.toggle('online', ok);
  $('liveText').textContent = ok ? 'LIVE' : 'OFFLINE';
}
// If a poll doesn't land within 5s of the last one, flip to OFFLINE even
// before fetch() reports an error (e.g. WiFi silently dropped).
setInterval(function () {
  if (lastStatusOkAt && Date.now() - lastStatusOkAt > 5000) { setOnline(false); }
}, 1000);

/* --- Push browser clock to the device so photos get real timestamps --- */
function sendTime() {
  var epoch = Math.floor(Date.now() / 1000);
  var tz = -new Date().getTimezoneOffset();
  fetch('/time?epoch=' + epoch + '&tz=' + tz).catch(function () {});
}

/* --- Locally-ticking clock: increments every second between status polls,
   resynced whenever /status returns a fresh reading. Date and time render
   in separate spans (different font/weight/spacing, see CSS). --- */
var clockBase = null;   // Date object matching device time at last sync
var clockBaseAt = 0;    // performance.now() at that sync
function syncClock(deviceTimeStr) {
  var m = /^(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})$/.exec(deviceTimeStr);
  if (!m) { clockBase = null; return; }
  clockBase = new Date(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]);
  clockBaseAt = performance.now();
}
function tickClock() {
  if (!clockBase) { $('chipDate').textContent = 'not set'; $('chipClock').textContent = ''; return; }
  var elapsed = Math.round((performance.now() - clockBaseAt) / 1000);
  var t = new Date(clockBase.getTime() + elapsed * 1000);
  $('chipDate').textContent = t.getFullYear() + '-' + pad2(t.getMonth() + 1) + '-' + pad2(t.getDate());
  $('chipClock').textContent = pad2(t.getHours()) + ':' + pad2(t.getMinutes()) + ':' + pad2(t.getSeconds());
}
setInterval(tickClock, 1000);

function todayStr() {
  var t = clockBase ? new Date(clockBase.getTime() + Math.round((performance.now() - clockBaseAt) / 1000) * 1000) : new Date();
  return t.getFullYear() + '-' + pad2(t.getMonth() + 1) + '-' + pad2(t.getDate());
}

function toggleStream() {
  streaming = !streaming;
  var img = $('streamImg'), ph = $('streamPlaceholder'), btn = $('streamBtn');
  if (streaming) {
    img.onerror = function () {
      streaming = false; img.onerror = null; img.style.display = 'none'; img.src = '';
      ph.style.display = 'block'; ph.textContent = 'Camera busy — try again';
      btn.textContent = 'Start'; btn.classList.remove('active');
    };
    img.src = streamUrl(); img.style.display = 'block'; ph.style.display = 'none';
    btn.textContent = 'Stop'; btn.classList.add('active');
  } else {
    img.onerror = null; img.src = ''; img.style.display = 'none';
    ph.style.display = 'block'; ph.textContent = 'Live view off';
    btn.textContent = 'Start'; btn.classList.remove('active');
  }
}

function takePhoto() {
  var note = $('captureNote');
  note.textContent = 'Capturing...';
  api('/snap').then(function (data) {
    if (data.ok) {
      note.textContent = 'Saved ' + data.filename;
      $('thumb').src = '/latest.jpg?t=' + Date.now();
      $('thumb').style.display = 'block';
      runQuery();  // refresh the gallery under whatever filter is active
    } else {
      note.textContent = 'Failed: ' + data.error;
    }
    refreshStatus();
  }).catch(function () { note.textContent = 'Request failed'; });
}

function toggleFlash() {
  var next = flashEnabled ? 0 : 1;
  flashEnabled = !flashEnabled;
  $('flashBtn').classList.toggle('on', flashEnabled);  // optimistic
  fetch('/flash?enable=' + next).then(refreshStatus).catch(function () {});
}

function toggleTorch() {
  var next = torchOn ? 0 : 1;
  torchOn = !torchOn;
  $('torchBtn').classList.toggle('on', torchOn);  // optimistic
  fetch('/torch?enable=' + next).then(refreshStatus).catch(function () {});
}

/* --- Interval parsing: "90", "30s", "10m", "2h", "1h30m" --- */
function parseInterval(str) {
  str = String(str).trim().toLowerCase();
  if (/^\d+$/.test(str)) return parseInt(str, 10);
  var total = 0, ok = false, m, re = /(\d+)\s*([smh])/g;
  while ((m = re.exec(str))) {
    ok = true;
    total += parseInt(m[1], 10) * (m[2] === 'h' ? 3600 : m[2] === 'm' ? 60 : 1);
  }
  return ok ? total : 0;
}
function fmtInterval(sec) {
  if (sec % 3600 === 0) return (sec / 3600) + 'h';
  if (sec % 60 === 0) return (sec / 60) + 'm';
  if (sec > 3600) return Math.floor(sec / 3600) + 'h' + Math.floor((sec % 3600) / 60) + 'm';
  return sec + 's';
}

// Enable/Disable toggles the schedule on/off, applying whatever is
// currently in the fields at the same time -- so you don't have to
// separately hit Update right before turning it on. Turning it off just
// pauses it; interval/window are left as they are.
function toggleTimer() {
  if (!timerEnabled) {
    var sec = parseInterval($('interval').value);
    if (sec < 1) {
      $('intervalEcho').textContent = 'Invalid interval — try 30s, 10m or 2h';
      return;
    }
    var useWindow = $('segWindow').classList.contains('active');
    var q = '/timer?enable=1&interval=' + sec + '&win=' + (useWindow ? 1 : 0);
    if (useWindow) {
      q += '&wstart=' + timeToMin($('winStart').value) + '&wend=' + timeToMin($('winEnd').value);
    }
    fetch(q).then(refreshStatus);
  } else {
    fetch('/timer?enable=0').then(refreshStatus);
  }
}

// Saves the currently-entered interval + window without changing whether
// the schedule is running -- the fields are always editable regardless of
// enabled state, this is the explicit "save what I typed" action.
function updateCaptureSchedule() {
  var sec = parseInterval($('interval').value);
  if (sec < 1) {
    $('intervalEcho').textContent = 'Invalid interval — try 30s, 10m or 2h';
    return;
  }
  var useWindow = $('segWindow').classList.contains('active');
  var q = '/timer?interval=' + sec + '&win=' + (useWindow ? 1 : 0);
  if (useWindow) {
    q += '&wstart=' + timeToMin($('winStart').value) + '&wend=' + timeToMin($('winEnd').value);
  }
  fetch(q).then(function () {
    $('intervalEcho').textContent = '✓ Updated successfully';
    setTimeout(refreshStatus, 1500);  // let the success message show before the next poll overwrites it
  }).catch(function () { $('intervalEcho').textContent = 'Update failed'; });
}

/* --- Capture window: all day, or a daily HH:MM-HH:MM range --- */
function timeToMin(hhmm) {
  var m = /^(\d{1,2}):(\d{2})/.exec(hhmm || '');
  return m ? (parseInt(m[1], 10) * 60 + parseInt(m[2], 10)) : 0;
}
function minToTime(min) {
  min = ((min % 1440) + 1440) % 1440;
  return (min < 600 ? '0' : '') + Math.floor(min / 60) + ':' + (min % 60 < 10 ? '0' : '') + (min % 60);
}
// Purely local UI -- which tab is highlighted and whether the time inputs
// show. Doesn't save anything by itself; Update/Enable do that.
function setWindowMode(useWindow) {
  $('segAllDay').classList.toggle('active', !useWindow);
  $('segWindow').classList.toggle('active', useWindow);
  $('windowInputs').style.display = useWindow ? 'flex' : 'none';
}

function refreshStatus() {
  api('/status').then(function (data) {
    lastStatusOkAt = Date.now();
    setOnline(true);
    $('advPhotoCount').textContent = data.photo_count;
    $('advFreeSpace').textContent = data.free_mb;
    $('advTotalSpace').textContent = data.total_mb;
    $('sdWarn').classList.toggle('show', !data.sd_mounted);
    $('cameraWarn').classList.toggle('show', !data.camera_ok);
    $('sleepBtn').disabled = !data.cloud_enabled;
    $('sleepBtn').textContent = data.sleep_enabled ? 'Disable sleep' : 'Enable sleep';
    $('sleepBtn').classList.toggle('active', data.sleep_enabled);
    $('cloudStatus').textContent = !data.cloud_enabled ? 'Cloud is not configured. Add a station token and trusted root CA in cloud_secrets.h.' :
      (data.sleep_enabled ? 'Sleep enabled; next timer wake uploads saved photos and checks commands.' : 'Cloud sync is enabled; sleep is off.') +
      (data.stay_awake ? ' Staying awake until resume or reboot.' : '') +
      (data.maintenance_remaining_seconds ? ' Maintenance window: ' + Math.ceil(data.maintenance_remaining_seconds / 60) + ' min remaining.' : '') +
      (data.upload_error_count ? ' Upload errors: ' + data.upload_error_count + ' (' + data.last_upload_error + ').' : '');

    // Scheduled captures happen entirely on the device with no request the
    // page is involved in, so this is how new photos (from the auto-timer,
    // or from another browser tab) show up here: notice the count moved.
    if (lastKnownPhotoCount !== null && data.photo_count !== lastKnownPhotoCount) {
      runQuery();
    }
    lastKnownPhotoCount = data.photo_count;

    $('chipSsid').textContent = data.ssid || '-';
    $('chipIp').textContent = data.ip || '-';
    $('chipRssi').textContent = data.rssi + ' dBm';
    if (data.time_set) { syncClock(data.time); tickClock(); }
    else { clockBase = null; $('chipDate').textContent = 'not set'; $('chipClock').textContent = ''; }

    timerEnabled = data.timer_enabled;
    var timerBtn = $('timerBtn');
    timerBtn.textContent = timerEnabled ? 'Disable' : 'Enable';
    timerBtn.classList.toggle('active', timerEnabled);
    if (document.activeElement.id !== 'interval') {
      $('interval').value = fmtInterval(data.timer_interval);
    }

    // Fields stay editable regardless of enabled state now -- Update/Enable
    // are the explicit actions that save them, so there's nothing to lock.
    if (document.activeElement.id !== 'segAllDay' && document.activeElement.id !== 'segWindow') {
      $('segAllDay').classList.toggle('active', !data.win);
      $('segWindow').classList.toggle('active', data.win);
      $('windowInputs').style.display = data.win ? 'flex' : 'none';
    }
    if (document.activeElement.id !== 'winStart') { $('winStart').value = minToTime(data.wstart); }
    if (document.activeElement.id !== 'winEnd') { $('winEnd').value = minToTime(data.wend); }

    var msg = 'Currently capturing every ' + fmtInterval(data.timer_interval);
    if (timerEnabled && data.win && !data.in_window) {
      msg = 'Paused — outside the ' + minToTime(data.wstart) + '–' + minToTime(data.wend) + ' window';
    } else if (timerEnabled && data.win) {
      msg += ' (within ' + minToTime(data.wstart) + '–' + minToTime(data.wend) + ')';
    }
    $('intervalEcho').textContent = msg;

    $('fullWarn').classList.toggle('show', data.sd_full && !data.erasing);

    var wasErasing = erasing;
    erasing = data.erasing;
    $('eraseWarn').classList.toggle('show', erasing);
    if (wasErasing && !erasing) { runQuery(); }

    var captureBtn = $('captureBtn');
    captureBtn.disabled = erasing;

    flashEnabled = data.flash_enabled;
    $('flashBtn').classList.toggle('on', flashEnabled);
    $('flashBtn').title = flashEnabled ? 'Flash on capture (tap to disable)' : 'Flash off (tap to enable)';

    torchOn = data.torch_on;
    $('torchBtn').classList.toggle('on', torchOn);
    $('torchBtn').title = torchOn ? 'Torch on (tap to turn off)' : 'Torch off (tap for steady light)';

    if (data.ads_ok) {
      $('adsStatus').textContent = '0x' + ('0' + (data.ads_addr || 0).toString(16)).slice(-2) + '  RX0/TX0';
      $('adsA0').textContent = (data.ads_m0 || 0).toFixed(0) + '%  (' + data.ads_a0.toFixed(2) + ' V)';
    } else {
      $('adsStatus').textContent = 'not found (SDA=RX0 SCL=TX0)';
      $('adsA0').textContent = '--';
    }

    if (data.dht_have) {
      $('dhtTemp').textContent = data.dht_c.toFixed(1);
      $('dhtHum').textContent = data.dht_h.toFixed(1);
      $('dhtAge').textContent = data.dht_age_s < 5 ? 'just now' : data.dht_age_s + 's ago';
      var approxEpoch = Math.floor(Date.now() / 1000) - data.dht_age_s;
      if (lastDhtReadingEpoch !== null && Math.abs(approxEpoch - lastDhtReadingEpoch) > 2) {
        dhtLoadRecent();
      }
      lastDhtReadingEpoch = approxEpoch;
    } else {
      $('dhtAge').textContent = 'no reading yet';
    }

    dhtTimerEnabled = data.dht_enabled;
    var dhtTimerBtn = $('dhtTimerBtn');
    dhtTimerBtn.textContent = dhtTimerEnabled ? 'Disable' : 'Enable';
    dhtTimerBtn.classList.toggle('active', dhtTimerEnabled);
    if (document.activeElement.id !== 'dhtInterval') {
      $('dhtInterval').value = fmtInterval(data.dht_interval);
    }

    if (document.activeElement.id !== 'dhtSegAllDay' && document.activeElement.id !== 'dhtSegWindow') {
      $('dhtSegAllDay').classList.toggle('active', !data.dht_win);
      $('dhtSegWindow').classList.toggle('active', data.dht_win);
      $('dhtWindowInputs').style.display = data.dht_win ? 'flex' : 'none';
    }
    if (document.activeElement.id !== 'dhtWinStart') { $('dhtWinStart').value = minToTime(data.dht_wstart); }
    if (document.activeElement.id !== 'dhtWinEnd') { $('dhtWinEnd').value = minToTime(data.dht_wend); }

    var dhtMsg = 'Reading every ' + fmtInterval(data.dht_interval);
    if (dhtTimerEnabled && data.dht_win && !data.dht_in_window) {
      dhtMsg = 'Paused — outside the ' + minToTime(data.dht_wstart) + '–' + minToTime(data.dht_wend) + ' window';
    } else if (dhtTimerEnabled && data.dht_win) {
      dhtMsg += ' (within ' + minToTime(data.dht_wstart) + '–' + minToTime(data.dht_wend) + ')';
    }
    $('dhtIntervalEcho').textContent = dhtMsg;

    waterVisionEnabled = data.ai_vision_enabled;
    var waterVisionBtn = $('waterVisionBtn');
    waterVisionBtn.textContent = waterVisionEnabled ? 'Disable' : 'Enable';
    waterVisionBtn.classList.toggle('active', waterVisionEnabled);

    openAiKeySet = !!data.openai_key_set;
    $('waterKeyStatus').textContent = openAiKeySet ? 'API key is set.' : 'No API key saved yet — add one before enabling analysis.';
  }).catch(function () { setOnline(false); });
}

function toggleSleepMode() {
  var enabled = !$('sleepBtn').classList.contains('active');
  fetch('/sleep?enable=' + (enabled ? 1 : 0)).then(function (r) { return r.json(); }).then(function (data) {
    if (!data.ok) { $('cloudStatus').textContent = 'Sleep change failed: ' + data.error; return; }
    refreshStatus();
  }).catch(function () { $('cloudStatus').textContent = 'Sleep change failed'; });
}

function setAwakeSession(awake) {
  fetch('/sleep?awake=' + (awake ? 1 : 0)).then(refreshStatus).catch(function () {});
}

/* ===================== Gallery ===================== */
/* Filters ARE the browsing mechanism: from/to date + from/to time + per
   page. Defaults to "today, all day, 10/page" which covers most use.
   A day-by-day fetch (parallel) builds the full matching set in memory,
   then paging is instant (no repeat server calls). */

var curRows = [];      // full filtered set, newest first
var curPage = 0, PER = 10;
var selected = {};      // key "date|name" -> {date,n,s}
var MAX_BATCH = 15;
var modalDate = '', modalFile = '';

function resetFilters() {
  var t = todayStr();
  $('fDateFrom').value = t;
  $('fDateTo').value = t;
  $('fTimeFrom').value = '00:00:00';
  $('fTimeTo').value = '23:59:59';
  $('perSel').value = '10';
  PER = 10;
  applyFilters();
}
function applyFilters() {
  PER = parseInt($('perSel').value, 10) || 10;
  runQuery();
}
function onPerChange() {
  PER = parseInt($('perSel').value, 10) || 10;
  curPage = 0;
  renderPage();
}

function eachDateInRange(from, to) {
  var dates = [];
  var d = new Date(from + 'T00:00:00');
  var end = new Date(to + 'T00:00:00');
  if (isNaN(d) || isNaN(end) || d > end) return dates;
  var guard = 0;
  while (d <= end && guard < 62) {  // cap a single query to ~2 months
    dates.push(d.getFullYear() + '-' + pad2(d.getMonth() + 1) + '-' + pad2(d.getDate()));
    d.setDate(d.getDate() + 1);
    guard++;
  }
  return dates;
}

// Runs `fn` over `items` with at most `limit` calls in flight at once.
// A wide date range fired all at once via Promise.all() could exceed the
// device's small HTTP server connection pool (esp_http_server defaults to
// ~7 concurrent sockets shared with /status polling etc.), so this caps
// concurrency instead of firing every request simultaneously.
function mapLimit(items, limit, fn) {
  return new Promise(function (resolve) {
    var results = new Array(items.length);
    if (!items.length) { resolve(results); return; }
    var next = 0, active = 0, completed = 0;
    function runNext() {
      while (active < limit && next < items.length) {
        (function (i) {
          active++;
          fn(items[i], i).then(function (r) {
            results[i] = r;
            active--; completed++;
            if (completed === items.length) { resolve(results); }
            else { runNext(); }
          });
        })(next++);
      }
    }
    runNext();
  });
}

var queryGeneration = 0;  // guards against a slow/stale query overwriting a newer one's results

function runQuery() {
  var myGeneration = ++queryGeneration;
  var from = $('fDateFrom').value || todayStr();
  var to = $('fDateTo').value || from;
  var dates = eachDateInRange(from, to);
  if (!dates.length) { curRows = []; renderPage(); return; }

  var ft = $('fTimeFrom').value, tt = $('fTimeTo').value;
  var qTail = '&per=100000';
  if (ft) qTail += '&from=' + ft.replace(/:/g, '').padEnd(6, '0');
  if (tt) qTail += '&to=' + tt.replace(/:/g, '').padEnd(6, '9');

  $('plist').innerHTML = '<div class="empty">Searching&hellip;</div>';
  mapLimit(dates, 3, function (d) {
    return api('/photos?date=' + encodeURIComponent(d) + '&page=0' + qTail).then(function (data) {
      return (data.files || []).map(function (f) { return { date: d, n: f.n, s: f.s }; });
    }).catch(function () { return []; });
  }).then(function (perDate) {
    if (myGeneration !== queryGeneration) return;  // a newer query already superseded this one
    var all = [].concat.apply([], perDate);
    all.sort(function (a, b) {
      var ka = a.date + a.n, kb = b.date + b.n;
      return ka < kb ? 1 : ka > kb ? -1 : 0;  // newest first
    });
    curRows = all;
    curPage = 0;
    selected = {};
    renderPage();
    loadWaterReadings(dates, myGeneration);
  });
}

// Joins AI-vision plant analyses onto the gallery by filename, keyed
// "date|photoName" -- fetched separately from /photos since it's a small,
// independent CSV-backed list and most photos won't have a reading at all.
var waterReadings = {};
function loadWaterDatePage(d, page, rows) {
  return api('/water/list?date=' + encodeURIComponent(d) + '&page=' + page + '&per=100').then(function (data) {
    var nextRows = rows.concat(data.rows || []);
    if ((data.rows || []).length && nextRows.length < (data.total || 0)) {
      return loadWaterDatePage(d, page + 1, nextRows);
    }
    return nextRows.map(function (r) { return { date: d, r: r }; });
  });
}
function loadWaterReadings(dates, myGeneration) {
  mapLimit(dates, 3, function (d) {
    return loadWaterDatePage(d, 0, []).catch(function () { return []; });
  }).then(function (perDate) {
    if (myGeneration !== queryGeneration) return;
    var all = [].concat.apply([], perDate);
    waterReadings = {};
    all.forEach(function (item) {
      var key = item.date + '|' + item.r.photo;
      if (item.r.photo) { waterReadings[key] = item.r; }
    });
    renderPage();
  });
}

function rowKey(r) { return r.date + '|' + r.n; }

function pageStep(dir) {
  var totalPages = Math.max(1, Math.ceil(curRows.length / PER));
  var next = curPage + dir;
  if (next < 0 || next >= totalPages) return;
  curPage = next;
  renderPage();
}

function renderPage() {
  var totalPages = Math.max(1, Math.ceil(curRows.length / PER));
  if (curPage >= totalPages) curPage = totalPages - 1;
  if (curPage < 0) curPage = 0;
  var pageRows = curRows.slice(curPage * PER, curPage * PER + PER);

  var list = $('plist');
  list.innerHTML = '';
  if (!pageRows.length) {
    list.innerHTML = '<div class="empty">No photos match this filter</div>';
  }
  var multiDay = $('fDateFrom').value !== $('fDateTo').value;
  pageRows.forEach(function (f) {
    var key = rowKey(f);
    var row = document.createElement('div');
    row.className = 'prow' + (selected[key] ? ' sel' : '');
    var kb = Math.round(f.s / 1024);
    var label = multiDay ? (f.date + ' / ' + f.n) : f.n;

    var box = document.createElement('input');
    box.type = 'checkbox'; box.className = 'checkbox';
    box.checked = !!selected[key];
    box.onclick = function (e) { e.stopPropagation(); toggleSelected(f); };

    var nm = document.createElement('span');
    nm.className = 'nm';
    var reading = waterReadings[f.date + '|' + f.n];
    var readingHtml = '';
    if (reading) {
      readingHtml = ' <span style="color:var(--accent);">&#128167; ' +
        (reading.cm === null ? 'unreadable' : reading.cm + 'cm') + '</span>' +
        ' <span style="color:var(--green);">&#127793; ' +
        (reading.tillers === null ? 'tillers unreadable' : reading.tillers + ' tillers') + '</span>';
      if (reading.stage) {
        readingHtml += ' <span style="color:var(--muted);">' + escapeHtml(reading.stage) + '</span>';
      }
      readingHtml += ' <span style="color:var(--muted);font-size:11px;">confidence W ' +
        Math.round((reading.wconf || 0) * 100) + '% / T ' +
        Math.round((reading.tconf || 0) * 100) + '% / G ' +
        Math.round((reading.gconf || 0) * 100) + '%</span>';
    }
    nm.innerHTML = '&#128247; ' + escapeHtml(label) + readingHtml;
    if (reading && reading.notes) nm.title = reading.notes;
    nm.onclick = function () { openModal(f.date, f.n); };

    var sz = document.createElement('span');
    sz.className = 'sz'; sz.textContent = kb + ' KB';

    var dl = document.createElement('button');
    dl.className = 'dlbtn'; dl.innerHTML = DL_SVG;
    dl.onclick = function (e) { e.stopPropagation(); downloadPhoto(f.date, f.n); };

    row.appendChild(box); row.appendChild(nm); row.appendChild(sz); row.appendChild(dl);
    list.appendChild(row);
  });

  $('pageInfo').textContent = 'Page ' + (curPage + 1) + ' / ' + totalPages + ' (' + curRows.length + ')';
  $('prevBtn').disabled = curPage + 1 >= totalPages;
  $('nextBtn').disabled = curPage === 0;
  updateBar();
}

/* --- Selection: checkboxes are always visible; "select all" covers every
   row matched by the current filter, not just the current page --- */
function toggleSelected(f) {
  var key = rowKey(f);
  if (selected[key]) { delete selected[key]; } else { selected[key] = f; }
  renderPage();
}
function onSelectAllToggle(checked) {
  if (checked) {
    curRows.forEach(function (f) { selected[rowKey(f)] = f; });
  } else {
    selected = {};
  }
  renderPage();
}
function selCount() { return Object.keys(selected).length; }
function updateBar() {
  var n = selCount();
  $('selCount').textContent = n;
  $('abar').classList.toggle('active', n > 0);
  $('zipBtn').disabled = n === 0;
  $('delBtn').disabled = n === 0;
  $('selectAllBox').checked = curRows.length > 0 && n === curRows.length;
  $('selectAllBox').indeterminate = n > 0 && n < curRows.length;
}

// Groups selected rows by date (server API is per-date) and splits each
// date's list into MAX_BATCH-sized chunks so the query string never grows
// past what the device's HTTP server accepts.
function groupSelectedForBatch() {
  var byDate = {};
  Object.keys(selected).forEach(function (k) {
    var f = selected[k];
    (byDate[f.date] = byDate[f.date] || []).push(f.n);
  });
  var batches = [];
  Object.keys(byDate).forEach(function (d) {
    var names = byDate[d];
    for (var i = 0; i < names.length; i += MAX_BATCH) {
      batches.push({ date: d, names: names.slice(i, i + MAX_BATCH) });
    }
  });
  return batches;
}

function zipSelected() {
  var batches = groupSelectedForBatch();
  if (!batches.length) return;
  batches.forEach(function (b, i) {
    setTimeout(function () {
      var a = document.createElement('a');
      a.href = '/zip?date=' + encodeURIComponent(b.date) + '&files=' + b.names.join(',');
      a.download = 'photos_' + b.date + (batches.length > 1 ? '_' + (i + 1) : '') + '.zip';
      document.body.appendChild(a); a.click(); a.remove();
    }, i * 600);  // stagger so the browser doesn't drop overlapping downloads
  });
}

function deleteSelected() {
  var batches = groupSelectedForBatch();
  if (!batches.length) return;
  var total = selCount();
  if (!confirm('Delete ' + total + ' photo(s)? This cannot be undone.')) return;

  var chain = Promise.resolve();
  batches.forEach(function (b) {
    chain = chain.then(function () {
      return api('/delete?date=' + encodeURIComponent(b.date) + '&files=' + b.names.join(','));
    });
  });
  chain.then(function () {
    runQuery();
    refreshStatus();
  }).catch(function () {});
}

function photoUrl(date, name, dl) {
  return '/photo?date=' + encodeURIComponent(date) + '&name=' + encodeURIComponent(name) + (dl ? '&dl=1' : '');
}
function downloadPhoto(date, name) {
  var a = document.createElement('a');
  a.href = photoUrl(date, name, true);
  a.download = date + '_' + name;
  document.body.appendChild(a); a.click(); a.remove();
}
function openModal(date, name) {
  modalDate = date; modalFile = name;
  $('modalImg').src = photoUrl(date, name, false);
  $('modalName').textContent = date + ' / ' + name;
  var existing = waterReadings[date + '|' + name];
  $('modalAnalysisStatus').textContent = existing
    ? 'Analysis available. You can analyze again to refresh it.'
    : (openAiKeySet ? 'Ready for manual analysis.' : 'Save an OpenAI API key before analyzing.');
  $('modalAnalyzeBtn').disabled = !openAiKeySet;
  $('modalAnalyzeBtn').textContent = '\u2728 Analyze photo';
  $('modal').classList.add('show');
}
function closeModal() {
  $('modal').classList.remove('show');
  $('modalImg').src = '';
}
function downloadCurrent() { downloadPhoto(modalDate, modalFile); }
function manualAnalyzeCurrent() {
  if (!modalDate || !modalFile) return;
  if (!openAiKeySet) {
    $('modalAnalysisStatus').textContent = 'Save an OpenAI API key in Plant Analysis settings first.';
    return;
  }
  var date = modalDate, name = modalFile;
  var btn = $('modalAnalyzeBtn');
  btn.disabled = true;
  btn.textContent = 'Queueing\u2026';
  $('modalAnalysisStatus').textContent = 'Sending this photo to the analysis queue\u2026';
  fetch('/water/analyze?date=' + encodeURIComponent(date) + '&name=' + encodeURIComponent(name), { method:'POST' })
    .then(function (r) { return r.json(); })
    .then(function (d) {
      if (!d.ok) {
        var messages = {
          key_required:'Save an OpenAI API key first.', photo_not_found:'The photo is no longer on the SD card.',
          queue_full:'The analysis queue is full. Try again shortly.', analysis_unavailable:'The analysis service did not start.'
        };
        $('modalAnalysisStatus').textContent = messages[d.error] || ('Could not queue analysis: ' + (d.error || 'unknown error'));
        btn.disabled = false;
        btn.textContent = '\u2728 Analyze photo';
        return;
      }
      $('modalAnalysisStatus').textContent = d.already_pending
        ? 'This photo is already waiting for analysis.'
        : 'Queued. The result will appear beside the photo when ready.';
      btn.textContent = d.already_pending ? 'Already queued' : 'Queued';
      setTimeout(function () { runQuery(); }, 8000);
      setTimeout(function () {
        runQuery();
        if (modalDate === date && modalFile === name) {
          btn.disabled = false;
          btn.textContent = '\u2728 Analyze again';
          $('modalAnalysisStatus').textContent = 'Refresh the Gallery if the result is still processing.';
        }
      }, 25000);
    })
    .catch(function () {
      $('modalAnalysisStatus').textContent = 'Request failed. Check the ESP32-CAM connection and try again.';
      btn.disabled = false;
      btn.textContent = '\u2728 Analyze photo';
    });
}
function deleteCurrent() {
  if (!confirm('Delete ' + modalFile + '? This cannot be undone.')) return;
  api('/delete?date=' + encodeURIComponent(modalDate) + '&files=' + modalFile).then(function () {
    closeModal();
    runQuery();
    refreshStatus();
  }).catch(function () {});
}

/* ===================== Advanced Settings ===================== */

/* --- Hold-to-confirm: press and hold `ms` milliseconds to fire `onDone`.
   A fill sweeps across the button as visual progress; releasing early
   cancels and resets it. --- */
function bindHold(id, ms, onDone) {
  var el = $(id), fill = el.querySelector('.holdFill');
  var timer = null;
  function start(e) {
    e.preventDefault();
    if (el.disabled) return;
    fill.style.transition = 'width ' + ms + 'ms linear';
    fill.style.width = '100%';
    timer = setTimeout(function () { timer = null; reset(); onDone(); }, ms);
  }
  function reset() {
    if (timer) { clearTimeout(timer); timer = null; }
    fill.style.transition = 'none';
    fill.style.width = '0%';
    void fill.offsetWidth;  // force reflow so the next hold restarts from 0
  }
  el.addEventListener('pointerdown', start);
  el.addEventListener('pointerup', function () { reset(); });
  el.addEventListener('pointerleave', function () { reset(); });
  el.addEventListener('pointercancel', function () { reset(); });
}

bindHold('eraseBtn', 10000, function () {
  var typed = prompt('This permanently deletes ALL photos on the SD card.\nType ERASE to confirm:');
  if (typed !== 'ERASE') return;
  api('/format?confirm=ERASE').then(function (d) {
    if (!d.ok) alert('Erase not started: ' + (d.error || 'unknown'));
    refreshStatus();
  }).catch(function () {});
});

bindHold('rebootBtn', 10000, function () {
  $('rebootWarn').classList.add('show');
  fetch('/reboot?confirm=YES').catch(function () {});
  setTimeout(function () { location.reload(); }, 9000);
});

/* ===================== DHT22 temperature/humidity sensor ===================== */

function dhtCaptureNow() {
  $('dhtAge').textContent = 'reading…';
  api('/dht/now').then(function (d) {
    if (d.ok) {
      $('dhtTemp').textContent = d.c.toFixed(1);
      $('dhtHum').textContent = d.h.toFixed(1);
      $('dhtAge').textContent = 'just now';
      lastDhtReadingEpoch = Math.floor(Date.now() / 1000);
      dhtLoadRecent();
    } else {
      $('dhtAge').textContent = 'read failed — check wiring';
    }
  }).catch(function () { $('dhtAge').textContent = 'request failed'; });
}

function dhtToggleTimer() {
  if (!dhtTimerEnabled) {
    var sec = parseInterval($('dhtInterval').value);
    if (sec < 1) {
      $('dhtIntervalEcho').textContent = 'Invalid interval — try 30s, 1m or 5m';
      return;
    }
    var useWindow = $('dhtSegWindow').classList.contains('active');
    var q = '/dht/config?enable=1&interval=' + sec + '&win=' + (useWindow ? 1 : 0);
    if (useWindow) {
      q += '&wstart=' + timeToMin($('dhtWinStart').value) + '&wend=' + timeToMin($('dhtWinEnd').value);
    }
    fetch(q).then(refreshStatus);
  } else {
    fetch('/dht/config?enable=0').then(refreshStatus);
  }
}

function dhtUpdateSchedule() {
  var sec = parseInterval($('dhtInterval').value);
  if (sec < 1) {
    $('dhtIntervalEcho').textContent = 'Invalid interval — try 30s, 1m or 5m';
    return;
  }
  var useWindow = $('dhtSegWindow').classList.contains('active');
  var q = '/dht/config?interval=' + sec + '&win=' + (useWindow ? 1 : 0);
  if (useWindow) {
    q += '&wstart=' + timeToMin($('dhtWinStart').value) + '&wend=' + timeToMin($('dhtWinEnd').value);
  }
  fetch(q).then(function () {
    $('dhtIntervalEcho').textContent = '✓ Updated successfully';
    setTimeout(refreshStatus, 1500);
  }).catch(function () { $('dhtIntervalEcho').textContent = 'Update failed'; });
}

function dhtSetWindowMode(useWindow) {
  $('dhtSegAllDay').classList.toggle('active', !useWindow);
  $('dhtSegWindow').classList.toggle('active', useWindow);
  $('dhtWindowInputs').style.display = useWindow ? 'flex' : 'none';
}

function dhtEraseAll() {
  if (!confirm('Delete ALL DHT temperature/humidity history? This cannot be undone. Photos are not affected.')) return;
  api('/dht/eraseall?confirm=ERASE').then(function (d) {
    if (d.ok) { dhtLoadRecent(); } else { alert('Erase failed: ' + (d.error || 'unknown')); }
  }).catch(function () {});
}

/* --- Recent readings: always today, newest 20 --- */
function dhtLoadRecent() {
  var date = todayStr();
  api('/dht/list?date=' + encodeURIComponent(date) + '&page=0&per=20').then(function (data) {
    var rows = (data.rows || []).slice().reverse();  // newest first
    var list = $('dhtRecentList');
    list.innerHTML = '';
    if (!rows.length) {
      list.innerHTML = '<div class="empty">No readings yet today</div>';
      return;
    }
    rows.forEach(function (r) {
      var row = document.createElement('div');
      row.className = 'prow';
      row.innerHTML = '<span class="nm">&#127777;&#65039; ' + r.t + '</span><span class="sz">' + r.c.toFixed(1) + '&deg;C &bull; ' + r.h.toFixed(1) + '%</span>';
      list.appendChild(row);
    });
  }).catch(function () {});
}

/* ===================== Rice plant analysis (OpenAI vision) ===================== */

function waterToggleVision() {
  var q = '/water/vision?enable=' + (waterVisionEnabled ? 0 : 1);
  fetch(q).then(refreshStatus);
}

function waterEraseAll() {
  if (!confirm('Delete ALL plant-analysis history? This cannot be undone. Source photos are not affected.')) return;
  api('/water/eraseall?confirm=ERASE').then(function (d) {
    if (!d.ok) { alert('Erase failed: ' + (d.error || 'unknown')); }
    else { waterReadings = {}; renderPage(); }
  }).catch(function () {});
}

function waterSaveApiKey() {
  var key = $('waterApiKey').value.trim();
  if (!key) { alert('Paste your OpenAI API key first'); return; }
  fetch('/water/apikey', {method:'POST', headers:{'Content-Type':'text/plain'}, body:key}).then(function (r) { return r.json(); }).then(function (d) {
    if (d.ok) {
      $('waterApiKey').value = '';
      $('waterKeyStatus').textContent = 'Saved.';
      refreshStatus();
    } else {
      alert('Could not save key: ' + (d.error || 'unknown'));
    }
  }).catch(function () {});
}

/* --- History: date/time range fetched day-by-day (reusing the gallery's
   eachDateInRange + mapLimit helpers), merged ascending for the chart --- */
var dhtHistoryRows = [];  // ascending by time: [{ts: Date, c, h}]

function dhtResetHistoryFilters() {
  var t = todayStr();
  $('dhtDateFrom').value = t;
  $('dhtDateTo').value = t;
  $('dhtTimeFrom').value = '00:00:00';
  $('dhtTimeTo').value = '23:59:59';
}

function dhtLoadHistory() {
  var from = $('dhtDateFrom').value || todayStr();
  var to = $('dhtDateTo').value || from;
  var dates = eachDateInRange(from, to);
  if (!dates.length) { dhtHistoryRows = []; dhtResetZoom(); return; }

  var ft = $('dhtTimeFrom').value, tt = $('dhtTimeTo').value;
  var qTail = '&per=2000';
  if (ft) qTail += '&from=' + ft.replace(/:/g, '').padEnd(6, '0');
  if (tt) qTail += '&to=' + tt.replace(/:/g, '').padEnd(6, '9');

  $('dhtHistoryNote').textContent = 'Loading…';
  mapLimit(dates, 3, function (d) {
    return api('/dht/list?date=' + encodeURIComponent(d) + '&page=0' + qTail).then(function (data) {
      return (data.rows || []).map(function (r) {
        var m = /^(\d{2}):(\d{2}):(\d{2})/.exec(r.t);
        var ts = new Date(d + 'T' + (m ? r.t : '00:00:00'));
        return { ts: ts, c: r.c, h: r.h };
      });
    }).catch(function () { return []; });
  }).then(function (perDate) {
    var all = [].concat.apply([], perDate);
    all.sort(function (a, b) { return a.ts - b.ts; });
    dhtHistoryRows = all;
    $('dhtHistoryNote').textContent = all.length + ' reading(s) loaded';
    dhtResetZoom();
  });
}

function dhtExportCsv() {
  if (!dhtHistoryRows.length) { alert('Load a date range first'); return; }
  var lines = ['timestamp,temperature_c,humidity_pct'];
  dhtHistoryRows.forEach(function (r) {
    var t = r.ts;
    var stamp = t.getFullYear() + '-' + pad2(t.getMonth() + 1) + '-' + pad2(t.getDate()) + ' ' +
      pad2(t.getHours()) + ':' + pad2(t.getMinutes()) + ':' + pad2(t.getSeconds());
    lines.push(stamp + ',' + r.c.toFixed(1) + ',' + r.h.toFixed(1));
  });
  var blob = new Blob([lines.join('\n')], { type: 'text/csv' });
  var url = URL.createObjectURL(blob);
  var a = document.createElement('a');
  a.href = url;
  a.download = 'dht11_' + ($('dhtDateFrom').value || 'export') + '_to_' + ($('dhtDateTo').value || '') + '.csv';
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(function () { URL.revokeObjectURL(url); }, 1000);
}

/* --- Lightweight canvas line chart: no external library, so it stays a
   few KB instead of pulling in a ~200KB charting library over WiFi on
   every page load. Two lines (temp/humidity) share one Y axis (a
   deliberate simplification -- their ranges are different, but a real
   dual-axis chart is a lot more code for a device dashboard). Zoom via
   mouse wheel / pinch, pan via drag, both operating on an index window
   into dhtHistoryRows rather than redrawing from a refetch. --- */
var dhtView = { start: 0, end: 0 };

function dhtResetZoom() {
  dhtView.start = 0;
  dhtView.end = dhtHistoryRows.length;
  dhtDrawChart();
}

function dhtDrawChart() {
  var canvas = $('dhtChart');
  var ctx = canvas.getContext('2d');
  var cssW = canvas.clientWidth || 320;
  var cssH = 220;
  var dpr = window.devicePixelRatio || 1;
  canvas.width = cssW * dpr;
  canvas.height = cssH * dpr;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, cssW, cssH);

  var rows = dhtHistoryRows.slice(dhtView.start, dhtView.end);
  var pad = { l: 36, r: 10, t: 10, b: 22 };
  var plotW = cssW - pad.l - pad.r;
  var plotH = cssH - pad.t - pad.b;

  if (rows.length < 2) {
    ctx.fillStyle = 'rgba(148,163,184,0.8)';
    ctx.font = '12px -apple-system,sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText(dhtHistoryRows.length ? 'Zoomed too far — scroll out' : 'No data — set a range and Load', cssW / 2, cssH / 2);
    return;
  }

  var minV = Infinity, maxV = -Infinity;
  rows.forEach(function (r) {
    minV = Math.min(minV, r.c, r.h);
    maxV = Math.max(maxV, r.c, r.h);
  });
  if (minV === maxV) { minV -= 1; maxV += 1; }
  var vSpan = (maxV - minV) * 1.1;
  var vMin = minV - (maxV - minV) * 0.05;

  var tMin = rows[0].ts.getTime(), tMax = rows[rows.length - 1].ts.getTime();
  var tSpan = Math.max(1, tMax - tMin);

  function xFor(t) { return pad.l + ((t - tMin) / tSpan) * plotW; }
  function yFor(v) { return pad.t + plotH - ((v - vMin) / vSpan) * plotH; }

  // Gridlines + Y labels
  ctx.strokeStyle = 'rgba(255,255,255,0.08)';
  ctx.fillStyle = 'rgba(148,163,184,0.7)';
  ctx.font = '10px -apple-system,sans-serif';
  ctx.textAlign = 'right';
  for (var i = 0; i <= 4; i++) {
    var v = vMin + (vSpan * i / 4);
    var y = yFor(v);
    ctx.beginPath(); ctx.moveTo(pad.l, y); ctx.lineTo(cssW - pad.r, y); ctx.stroke();
    ctx.fillText(v.toFixed(0), pad.l - 6, y + 3);
  }

  // X labels (start / mid / end of visible window)
  ctx.textAlign = 'center';
  [0, 0.5, 1].forEach(function (frac) {
    var idx = Math.min(rows.length - 1, Math.round(frac * (rows.length - 1)));
    var t = rows[idx].ts;
    var label = pad2(t.getMonth() + 1) + '/' + pad2(t.getDate()) + ' ' + pad2(t.getHours()) + ':' + pad2(t.getMinutes());
    ctx.fillText(label, xFor(t.getTime()), cssH - 6);
  });

  function drawLine(key, color) {
    ctx.beginPath();
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.8;
    rows.forEach(function (r, i) {
      var x = xFor(r.ts.getTime()), y = yFor(r[key]);
      if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    });
    ctx.stroke();
  }
  drawLine('c', '#f97316');
  drawLine('h', '#38bdf8');
}

// Pan/zoom: operate on index bounds into dhtHistoryRows.
(function () {
  var canvas = $('dhtChart');
  var dragging = false, dragStartX = 0, dragStartView = null;
  var pinchStartDist = 0, pinchStartView = null;

  function clampView() {
    var n = dhtHistoryRows.length;
    if (dhtView.end - dhtView.start < 5) dhtView.end = Math.min(n, dhtView.start + 5);
    if (dhtView.start < 0) { dhtView.end -= dhtView.start; dhtView.start = 0; }
    if (dhtView.end > n) { dhtView.start -= (dhtView.end - n); dhtView.end = n; }
    if (dhtView.start < 0) dhtView.start = 0;
  }

  canvas.addEventListener('wheel', function (e) {
    if (!dhtHistoryRows.length) return;
    e.preventDefault();
    var span = dhtView.end - dhtView.start;
    var zoomIn = e.deltaY < 0;
    var factor = zoomIn ? 0.85 : 1.18;
    var rect = canvas.getBoundingClientRect();
    var frac = (e.clientX - rect.left) / rect.width;
    var pivot = dhtView.start + span * frac;
    var newSpan = Math.max(5, Math.min(dhtHistoryRows.length, Math.round(span * factor)));
    dhtView.start = Math.round(pivot - newSpan * frac);
    dhtView.end = dhtView.start + newSpan;
    clampView();
    dhtDrawChart();
  }, { passive: false });

  canvas.addEventListener('mousedown', function (e) {
    dragging = true; dragStartX = e.clientX; dragStartView = { start: dhtView.start, end: dhtView.end };
    canvas.style.cursor = 'grabbing';
  });
  window.addEventListener('mousemove', function (e) {
    if (!dragging || !dhtHistoryRows.length) return;
    var rect = canvas.getBoundingClientRect();
    var span = dragStartView.end - dragStartView.start;
    var dxFrac = (e.clientX - dragStartX) / rect.width;
    var shift = Math.round(-dxFrac * span);
    dhtView.start = dragStartView.start + shift;
    dhtView.end = dragStartView.end + shift;
    clampView();
    dhtDrawChart();
  });
  window.addEventListener('mouseup', function () { dragging = false; canvas.style.cursor = 'grab'; });

  canvas.addEventListener('touchstart', function (e) {
    if (e.touches.length === 1) {
      dragging = true; dragStartX = e.touches[0].clientX; dragStartView = { start: dhtView.start, end: dhtView.end };
    } else if (e.touches.length === 2) {
      dragging = false;
      pinchStartDist = Math.hypot(e.touches[0].clientX - e.touches[1].clientX, e.touches[0].clientY - e.touches[1].clientY);
      pinchStartView = { start: dhtView.start, end: dhtView.end };
    }
  }, { passive: true });
  canvas.addEventListener('touchmove', function (e) {
    if (!dhtHistoryRows.length) return;
    var rect = canvas.getBoundingClientRect();
    if (e.touches.length === 1 && dragging) {
      var span = dragStartView.end - dragStartView.start;
      var dxFrac = (e.touches[0].clientX - dragStartX) / rect.width;
      var shift = Math.round(-dxFrac * span);
      dhtView.start = dragStartView.start + shift;
      dhtView.end = dragStartView.end + shift;
      clampView();
      dhtDrawChart();
    } else if (e.touches.length === 2 && pinchStartView) {
      var dist = Math.hypot(e.touches[0].clientX - e.touches[1].clientX, e.touches[0].clientY - e.touches[1].clientY);
      var scale = pinchStartDist / Math.max(1, dist);
      var span0 = pinchStartView.end - pinchStartView.start;
      var mid = pinchStartView.start + span0 / 2;
      var newSpan = Math.max(5, Math.min(dhtHistoryRows.length, Math.round(span0 * scale)));
      dhtView.start = Math.round(mid - newSpan / 2);
      dhtView.end = dhtView.start + newSpan;
      clampView();
      dhtDrawChart();
    }
  }, { passive: true });
  canvas.addEventListener('touchend', function () { dragging = false; pinchStartView = null; });

  window.addEventListener('resize', function () { if (dhtHistoryRows.length) dhtDrawChart(); });
})();

/* --- Device log viewer --- */
var lastLogText = '';
function loadDeviceLog(force) {
  var box = $('deviceLogBox');
  if (!box) return;
  fetch('/log?lines=120').then(function (r) { return r.text(); }).then(function (text) {
    var next = text || '(no log yet)';
    if (!force && next === lastLogText) return;
    lastLogText = next;
    var stick = box.scrollHeight - box.scrollTop - box.clientHeight < 40;
    box.textContent = next;
    if (stick || force) box.scrollTop = box.scrollHeight;
  }).catch(function () {
    if (force) box.textContent = 'Failed to load log';
  });
}

/* --- WiFi network manager --- */
function loadWifiList() {
  api('/wifi/list').then(function (data) {
    var list = $('wifiList');
    var nets = data.networks || [];
    if (!nets.length) { list.innerHTML = '<div class="empty">No saved networks</div>'; return; }
    list.innerHTML = '';
    nets.forEach(function (n) {
      var row = document.createElement('div');
      row.className = 'wifi-row';
      var connected = n.ssid === data.connected;
      row.innerHTML =
        '<span class="name">&#128246; ' + n.ssid + (connected ? '<span class="badge">CONNECTED</span>' : '') + '</span>' +
        '<button class="del ghost">Delete</button>';
      row.querySelector('.del').onclick = function () { deleteWifi(n.ssid); };
      list.appendChild(row);
    });
  }).catch(function () {
    $('wifiList').innerHTML = '<div class="empty">Couldn\'t load saved networks</div>';
  });
}
function addWifi() {
  var ssid = $('wifiSsid').value.trim();
  var pass = $('wifiPass').value;
  if (!ssid) { alert('Enter a network name'); return; }
  api('/wifi/add?ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass)).then(function (d) {
    if (!d.ok) { alert('Could not save network: ' + (d.error || 'unknown')); return; }
    $('wifiSsid').value = ''; $('wifiPass').value = '';
    loadWifiList();
  }).catch(function () {});
}
function deleteWifi(ssid) {
  if (!confirm('Remove saved network "' + ssid + '"?')) return;
  api('/wifi/delete?ssid=' + encodeURIComponent(ssid)).then(function () { loadWifiList(); }).catch(function () {});
}

$('galleryRefreshBtn').addEventListener('click', function (e) {
  e.preventDefault();   // don't let the click bubble into <summary> and toggle the accordion
  e.stopPropagation();
  var filtersDetails = $('filtersDetails');
  if (filtersDetails.open) { filtersDetails.open = false; }  // "close the headers"
  runQuery();
});

$('dhtCard').addEventListener('toggle', function () {
  if (this.open) { dhtLoadRecent(); }
});

resetFilters();
dhtResetHistoryFilters();
sendTime();
refreshStatus();
loadWifiList();
loadDeviceLog(true);
setInterval(refreshStatus, 2000);
setInterval(loadDeviceLog, 2000);
</script>
</body>
</html>
)HTMLPAGE";

#endif  // WEB_PAGE_H
