/**
 * Hardware page markup and client logic (Phase 5 Web UI).
 * Loaded from hardware.html; embedded via embed_ui.sh as /js/hardware.js.
 */
(function (global) {
  'use strict';

  const TOKEN_KEY = 'shellclaw_token';
  const ROADMAP_URL = 'https://github.com/asap-protocol/shellclaw/blob/main/README.md#shellclaw';
  const JETSON_BOARD_ID = 'jetson_orin_nano';
  const POLL_MS = 15000;

  function escapeHtml(str) {
    if (str == null || str === undefined) return '';
    const s = String(str);
    if (typeof document !== 'undefined' && document.createElement) {
      const div = document.createElement('div');
      div.textContent = s;
      return div.innerHTML;
    }
    return s
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  function coerceNum(v, fallback) {
    const n = Number(v);
    return Number.isFinite(n) ? n : fallback;
  }

  function pinCellClass(pin) {
    const mode = (pin.mode || '').toLowerCase();
    const state = pin.state == null ? '' : String(pin.state).toLowerCase();
    const sfio = pin.sfio === true || pin.sfio === 1 || mode === 'sfio';
    const parts = ['hw-pin-cell'];
    if (sfio) parts.push('hw-pin-sfio');
    else if (mode === 'input') parts.push('hw-pin-input');
    else if (mode === 'output') parts.push('hw-pin-output');
    else if (mode) parts.push('hw-pin-other');
    if (state === 'high' || state === '1' || state === true) parts.push('hw-pin-high');
    else if (state === 'low' || state === '0' || state === false) parts.push('hw-pin-low');
    return parts.join(' ');
  }

  function gpioGridSvg(pins) {
    const list = Array.isArray(pins) ? pins : [];
    const byPin = {};
    list.forEach(function (p) {
      byPin[coerceNum(p.pin, 0)] = p;
    });
    let cells = '';
    for (let num = 1; num <= 40; num++) {
      const pin = byPin[num] || { pin: num, label: '', mode: '', state: null, sfio: false };
      const label = escapeHtml(pin.label || '');
      const mode = escapeHtml(pin.mode || '-');
      const stateStr = pin.state == null || pin.state === undefined ? '—' : escapeHtml(String(pin.state));
      const cls = pinCellClass(pin);
      const x = ((num - 1) % 2) * 52;
      const y = Math.floor((num - 1) / 2) * 28;
      cells +=
        '<g class="' + cls + '" transform="translate(' + x + ',' + y + ')">' +
        '<rect class="hw-pin-rect" width="48" height="24" rx="3"/>' +
        '<text class="hw-pin-num" x="4" y="10">' + num + '</text>' +
        '<text class="hw-pin-lbl" x="4" y="21">' + (label || mode).substring(0, 8) + '</text>' +
        '<title>Pin ' + num + ' · ' + mode + ' · ' + stateStr + '</title>' +
        '</g>';
    }
    return (
      '<svg class="hw-pin-grid" viewBox="0 0 104 560" role="img" aria-label="40-pin GPIO header">' +
      cells +
      '</svg>'
    );
  }

  function gaugeMarkup(label, pct) {
    const p = Math.max(0, Math.min(100, coerceNum(pct, 0)));
    return (
      '<div class="hw-gauge">' +
      '<div class="hw-gauge-label">' + escapeHtml(label) + '</div>' +
      '<div class="hw-gauge-track"><div class="hw-gauge-fill" style="width:' + p + '%"></div></div>' +
      '<div class="hw-gauge-value">' + escapeHtml(String(Math.round(p))) + '%</div>' +
      '</div>'
    );
  }

  function deferredPlaceholderMarkup(title) {
    return (
      '<div class="hw-deferred-card">' +
      '<h2>' + escapeHtml(title) + '</h2>' +
      '<p class="hw-deferred-title">Coming in v1.2 (Phase 7)</p>' +
      '<p class="status-note">Sensor decoders and camera image return ship with the Physical World Hardware release.</p>' +
      '<p><a class="hw-roadmap-link" href="' + escapeHtml(ROADMAP_URL) + '" target="_blank" rel="noopener">View roadmap in README</a></p>' +
      '</div>'
    );
  }

  function boardPanelMarkup(board, i2c) {
    const b = board || {};
    const backends = b.backends && typeof b.backends === 'object' ? b.backends : {};
    let rows = '';
    Object.keys(backends).forEach(function (k) {
      rows += '<tr><td>' + escapeHtml(k) + '</td><td>' + escapeHtml(backends[k]) + '</td></tr>';
    });
    let i2cBlock = '';
    if (i2c && i2c.bus != null) {
      const addrs = Array.isArray(i2c.addresses) ? i2c.addresses : [];
      const hex = addrs.map(function (a) {
        return '0x' + Number(a).toString(16).padStart(2, '0');
      }).join(', ');
      i2cBlock =
        '<h2 class="dash-h2">I2C scan</h2>' +
        '<p class="dash-kv"><span class="dash-label">Bus</span> ' + escapeHtml(String(i2c.bus)) + '</p>' +
        '<p class="dash-kv"><span class="dash-label">Addresses</span> ' +
        (hex ? escapeHtml(hex) : '<span class="status-note">none detected</span>') +
        '</p>';
    } else if (i2c && i2c.loading) {
      i2cBlock = '<p class="status-note">Scanning I2C bus…</p>';
    } else if (i2c && i2c.error) {
      i2cBlock = '<p class="status-warn">' + escapeHtml(i2c.error) + '</p>';
    }
    return (
      '<div class="dashboard-cards">' +
      '<div class="card"><div class="card-label">Board ID</div><div class="card-value">' + escapeHtml(b.id || '-') + '</div></div>' +
      '<div class="card"><div class="card-label">Name</div><div class="card-value">' + escapeHtml(b.name || '-') + '</div></div>' +
      '</div>' +
      '<h2 class="dash-h2">Backends</h2>' +
      '<table class="dash-table"><thead><tr><th>Subsystem</th><th>Backend</th></tr></thead><tbody>' +
      (rows || '<tr><td colspan="2" class="status-note">No backend info.</td></tr>') +
      '</tbody></table>' +
      '<p><button type="button" class="hw-i2c-scan">Scan I2C</button></p>' +
      i2cBlock
    );
  }

  function gpioPanelMarkup(gpio) {
    const pins = gpio && Array.isArray(gpio.pins) ? gpio.pins : [];
    return (
      '<p class="dash-meta">40-pin header · <span class="hw-legend hw-pin-input">INPUT</span> ' +
      '<span class="hw-legend hw-pin-output">OUTPUT</span> <span class="hw-legend hw-pin-sfio">SFIO</span> ' +
      '<span class="hw-legend hw-pin-high">HIGH</span> <span class="hw-legend hw-pin-low">LOW</span></p>' +
      '<div class="hw-pin-grid-wrap">' + gpioGridSvg(pins) + '</div>' +
      (gpio && gpio.error ? '<p class="status-err">' + escapeHtml(gpio.error) + '</p>' : '')
    );
  }

  function gpuPanelMarkup(gpu) {
    const g = gpu || {};
    if (g.available === false) {
      return '<p class="status-note">' + escapeHtml(g.reason || 'GPU stats not available on this board.') + '</p>';
    }
    const usage = g.gpu_usage != null ? g.gpu_usage : g.gpu_percent;
    const mem = g.memory_percent != null ? g.memory_percent : g.memory_used_percent;
    const temp = g.temperature != null ? g.temperature : g.temp_c;
    const freq = g.gpu_freq_mhz != null ? g.gpu_freq_mhz : g.freq_mhz;
    const power = g.power_mode || g.power_mode_name || '';
    const llama = g.llama_server || g.llama;
    let llamaText = '-';
    if (llama && typeof llama === 'object') {
      llamaText = llama.running ? 'running' : (llama.status || 'stopped');
    } else if (typeof llama === 'string') {
      llamaText = llama;
    }
    let cards = '<div class="dashboard-cards">';
    if (temp != null) {
      cards += '<div class="card"><div class="card-label">GPU temp</div><div class="card-value">' +
        escapeHtml(String(temp)) + ' °C</div></div>';
    }
    if (freq != null) {
      cards += '<div class="card"><div class="card-label">GPU freq</div><div class="card-value">' +
        escapeHtml(String(freq)) + ' MHz</div></div>';
    }
    cards += '<div class="card"><div class="card-label">llama-server</div><div class="card-value">' +
      escapeHtml(llamaText) + '</div></div>';
    cards += '</div>';
    let html = cards;
    if (power) {
      html += '<p><span class="hw-power-badge">' + escapeHtml(String(power)) + '</span></p>';
    }
    html += '<div class="hw-gauges">' + gaugeMarkup('GPU usage', usage) + gaugeMarkup('Memory', mem) + '</div>';
    if (g.error) html += '<p class="status-err">' + escapeHtml(g.error) + '</p>';
    return html;
  }

  function hardwarePageMarkup(state) {
    const s = state || {};
    const showGpu = s.boardId === JETSON_BOARD_ID;
    let tabs = '<div class="hw-tabs" role="tablist">';
    const tabDefs = [
      { id: 'board', label: 'Board', show: true },
      { id: 'gpio', label: 'GPIO', show: true },
      { id: 'gpu', label: 'GPU', show: showGpu },
      { id: 'sensors', label: 'Sensors', show: true },
      { id: 'camera', label: 'Camera', show: true }
    ];
    let panels = '';
    let first = true;
    tabDefs.forEach(function (t) {
      if (!t.show) return;
      const active = first ? ' active' : '';
      first = false;
      tabs += '<button type="button" class="hw-tab' + active + '" data-hw-tab="' + t.id + '" role="tab">' +
        escapeHtml(t.label) + '</button>';
    });
    tabs += '</div>';
    first = true;
    tabDefs.forEach(function (t) {
      if (!t.show) return;
      const active = first ? ' active' : '';
      first = false;
      let body = '';
      if (t.id === 'board') body = boardPanelMarkup(s.board, s.i2c);
      else if (t.id === 'gpio') body = gpioPanelMarkup(s.gpio);
      else if (t.id === 'gpu') body = gpuPanelMarkup(s.gpu);
      else if (t.id === 'sensors') body = deferredPlaceholderMarkup('Sensors');
      else if (t.id === 'camera') body = deferredPlaceholderMarkup('Camera');
      panels += '<section class="hw-panel' + active + '" data-hw-panel="' + t.id + '" role="tabpanel">' + body + '</section>';
    });
    const err = s.loadError ? '<div class="dash-banner dash-banner-err">' + escapeHtml(s.loadError) + '</div>' : '';
    return (
      '<h1>Hardware</h1>' +
      '<p class="dash-meta"><span class="status-note">Live data from /api/hardware/* · polling every ' +
      (POLL_MS / 1000) + 's</span></p>' +
      err +
      tabs +
      '<div class="hw-panels">' + panels + '</div>'
    );
  }

  function activateTab(root, tabId) {
    const tabs = root.querySelectorAll('.hw-tab');
    const panels = root.querySelectorAll('.hw-panel');
    tabs.forEach(function (b) {
      b.classList.toggle('active', b.getAttribute('data-hw-tab') === tabId);
    });
    panels.forEach(function (p) {
      p.classList.toggle('active', p.getAttribute('data-hw-panel') === tabId);
    });
  }

  function bindTabs(root, onSelect) {
    const tabs = root.querySelectorAll('.hw-tab');
    tabs.forEach(function (btn) {
      btn.addEventListener('click', function () {
        const id = btn.getAttribute('data-hw-tab');
        activateTab(root, id);
        if (onSelect) onSelect(id);
      });
    });
  }

  function bindI2cScanButton(root, onScan) {
    const btn = root.querySelector('.hw-i2c-scan');
    if (!btn || !onScan) return;
    btn.addEventListener('click', function () { onScan(); });
  }

  function createApi() {
    function getToken() {
      return localStorage.getItem(TOKEN_KEY);
    }
    function api(method, path, body) {
      const opts = { method: method, headers: {} };
      const token = getToken();
      if (token) opts.headers.Authorization = 'Bearer ' + token;
      if (body) {
        opts.headers['Content-Type'] = 'application/json';
        opts.body = typeof body === 'string' ? body : JSON.stringify(body);
      }
      return fetch(path, opts).then(function (r) {
        if (r.status === 401) {
          window.location.href = '/#/pair';
          return Promise.reject('Unauthorized');
        }
        if (!r.ok) {
          return r.text().then(function (t) {
            return Promise.reject(t || 'HTTP ' + r.status);
          });
        }
        return r.json().catch(function () { return {}; });
      });
    }
    return { getToken: getToken, api: api };
  }

  function loadHardwareData(apiClient) {
    return apiClient.api('GET', '/api/hardware/board').then(function (board) {
      const boardId = board && board.id ? board.id : '';
      const showGpu = boardId === JETSON_BOARD_ID;
      const tasks = [
        apiClient.api('GET', '/api/hardware/gpio').catch(function (e) {
          return { pins: [], error: String(e) };
        })
      ];
      if (showGpu) {
        tasks.push(apiClient.api('GET', '/api/hardware/gpu').catch(function (e) {
          return { available: false, reason: String(e) };
        }));
      }
      return Promise.all(tasks).then(function (parts) {
        let idx = 0;
        const gpio = parts[idx++];
        const gpu = showGpu ? parts[idx++] : { available: false };
        return { board: board, boardId: boardId, gpio: gpio, gpu: gpu };
      });
    });
  }

  function fetchI2cScan(apiClient) {
    return apiClient.api('GET', '/api/hardware/i2c-scan').catch(function (e) {
      return { error: String(e) };
    });
  }

  function initHardwarePage() {
    const root = document.getElementById('hw-root');
    if (!root) return;
    const apiClient = createApi();
    if (!apiClient.getToken()) {
      window.location.href = '/#/pair';
      return;
    }
    let timer = null;
    let lastData = null;
    let lastError = null;
    let i2cData = null;
    let i2cLoading = false;
    function paint(data, loadError) {
      const active = root.querySelector('.hw-tab.active');
      const activeTabId = active ? active.getAttribute('data-hw-tab') : null;
      lastData = data;
      lastError = loadError;
      root.innerHTML = hardwarePageMarkup({
        board: data && data.board,
        boardId: data && data.boardId,
        gpio: data && data.gpio,
        i2c: i2cData,
        gpu: data && data.gpu,
        loadError: loadError
      });
      bindTabs(root, function (tabId) {
        if (tabId === 'board') loadI2cIfNeeded();
      });
      bindI2cScanButton(root, function () { loadI2c(true); });
      if (activeTabId) {
        activateTab(root, activeTabId);
        if (activeTabId === 'board') loadI2cIfNeeded();
      } else {
        loadI2cIfNeeded();
      }
    }
    function loadI2c(force) {
      if (i2cLoading && !force) return;
      i2cLoading = true;
      i2cData = { loading: true };
      if (lastData) paint(lastData, lastError);
      fetchI2cScan(apiClient).then(function (result) {
        i2cData = result;
        i2cLoading = false;
        if (lastData) paint(lastData, lastError);
      });
    }
    function loadI2cIfNeeded() {
      if (i2cData != null || i2cLoading) return;
      loadI2c(false);
    }
    function refresh() {
      loadHardwareData(apiClient)
        .then(function (data) { paint(data, null); })
        .catch(function (err) {
          paint({ board: {}, boardId: '', gpio: { pins: [] }, gpu: { available: false } }, String(err));
        });
    }
    root.innerHTML = '<h1>Hardware</h1><p class="status-note">Loading board…</p>';
    refresh();
    timer = window.setInterval(refresh, POLL_MS);
    window.addEventListener('beforeunload', function () {
      if (timer) window.clearInterval(timer);
    });
  }

  const api = {
    escapeHtml: escapeHtml,
    pinCellClass: pinCellClass,
    gpioGridSvg: gpioGridSvg,
    boardPanelMarkup: boardPanelMarkup,
    gpioPanelMarkup: gpioPanelMarkup,
    gpuPanelMarkup: gpuPanelMarkup,
    deferredPlaceholderMarkup: deferredPlaceholderMarkup,
    hardwarePageMarkup: hardwarePageMarkup,
    shouldShowGpuPanel: function (board) {
      return !!(board && board.id === JETSON_BOARD_ID);
    },
    JETSON_BOARD_ID: JETSON_BOARD_ID
  };

  global.ShellClawHardware = api;

  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api;
  }

  if (typeof document !== 'undefined') {
    if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', initHardwarePage);
    } else {
      initHardwarePage();
    }
  }
})(typeof globalThis !== 'undefined' ? globalThis : window);
