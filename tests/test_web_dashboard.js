#!/usr/bin/env node
/**
 * @file test_web_dashboard.js
 * @brief Node tests for dashboard helpers in web/js/dashboardView.js.
 */
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const dash = require('../web/js/dashboardView.js');
const hw = require('../web/js/hardwareView.js');
const escapeHtml = dash.escapeHtml;
const coerceBool = dash.coerceBool;
const pickStatusSlice = dash.pickStatusSlice;
const mergeStatusWithWs = dash.mergeStatusWithWs;

function assert(cond, msg) {
  if (!cond) {
    console.error('FAIL:', msg);
    process.exit(1);
  }
}

function testPairedChatDisplaysRepliesAsText() {
  let connection;
  const location = { protocol: 'https:', host: 'localhost', hash: '#/chat' };
  const log = { textContent: '' };
  const elements = { app: {}, 'chat-log': log, 'chat-input': {} };
  const token = '1234567890abcdef1234567890abcdef';
  const script = fs.readFileSync(path.join(__dirname, '../web/js/app.js'), 'utf8');
  vm.runInNewContext(script, {
    location,
    window: { location, addEventListener() {} },
    document: { getElementById: id => elements[id], querySelectorAll: () => [] },
    localStorage: { getItem: key => key === 'shellclaw_token' ? token : null },
    WebSocket: function (url, protocols) {
      this.url = url;
      this.protocols = protocols;
      connection = this;
    },
  });
  const replies = ['The card is blue.', '{"type":"provider_status","providers":[]}',
    '"Café" <strong>azul</strong>'];
  replies.forEach(text => connection.onmessage({ data: text }));
  assert(log.textContent === replies.join('\n') + '\n', 'chat displays each reply as text');
  assert(log.innerHTML === undefined, 'chat does not insert reply markup');
  assert(connection.url === 'wss://localhost/ws?view=chat', 'chat requests the replies-only socket');
  assert(connection.protocols.length === 2 && connection.protocols[0] === 'ws' &&
    connection.protocols[1] === 'bearer.' + token, 'paired chat offers both WebSocket protocols');
}

testPairedChatDisplaysRepliesAsText();

assert(escapeHtml(null) === '', 'null');
assert(escapeHtml(undefined) === '', 'undefined');
assert(escapeHtml('<script>alert(1)</script>').indexOf('<script>') === -1, 'script tag');
assert(escapeHtml('a & b').indexOf('&amp;') !== -1, 'ampersand');

assert(coerceBool(true) === true, 'bool true');
assert(coerceBool('false') === false, 'string false');
assert(coerceBool(0) === false, 'zero');
assert(coerceBool('1') === true, 'string one');

assert(dash.providerSemanticClass('primary', true) === 'dash-green', 'primary green');
assert(dash.providerSemanticClass('fallback', true) === 'dash-yellow', 'fallback yellow');
assert(dash.providerSemanticClass('standby', true) === 'dash-yellow', 'standby yellow');
assert(dash.discordSemanticClass('disconnected') === 'dash-red', 'discord red');
assert(dash.discordSemanticClass('connecting') === 'dash-yellow', 'discord connecting');

const html = dash.dashboardMarkup({ status: 'ok' }, { providers: [] }, {});
assert(html.indexOf('Dashboard') !== -1, 'dashboardMarkup');

const failoverHtml = dash.dashboardMarkup(
  { status: 'ok' },
  {
    active_provider: 'stub',
    last_error: 'stub-b unavailable',
    providers: [
      { name: 'stub-b', role: 'unavailable', reachable: false },
      { name: 'stub', role: 'fallback', reachable: true },
    ],
  },
  {},
);
assert(failoverHtml.indexOf('dash-banner-err') !== -1, 'last_error banner');
assert(failoverHtml.indexOf('stub *') !== -1, 'active provider mark');

const slice = pickStatusSlice({ type: 'provider_status', active_provider: 'stub', providers: [] });
assert(slice && slice.type === undefined && slice.active_provider === 'stub', 'pickStatusSlice');

const prevStatus = {
  active_provider: 'primary',
  providers: [{ name: 'primary', role: 'primary', reachable: true }],
  generated_at: '2026-01-01T00:00:00Z'
};
const wsDelta = {
  type: 'provider_status',
  active_provider: 'fallback',
  providers: [{ name: 'fallback', role: 'fallback', reachable: true }],
  last_error: 'primary unreachable'
};
const merged = mergeStatusWithWs(prevStatus, wsDelta);
assert(merged.active_provider === 'fallback', 'merge active_provider');
assert(merged.providers.length === 1 && merged.providers[0].name === 'fallback', 'merge providers');
assert(merged.last_error === 'primary unreachable', 'merge last_error');
assert(merged.generated_at === '2026-01-01T00:00:00Z', 'preserve generated_at');

const clearedErr = mergeStatusWithWs(prevStatus, { type: 'provider_status', last_error: null });
assert(clearedErr.last_error === null, 'merge clears last_error when null');

assert(mergeStatusWithWs(null, wsDelta) === null, 'merge null prev');
assert(mergeStatusWithWs(prevStatus, null) === prevStatus, 'merge null ws');

const jetsonBoard = {
  id: 'jetson_orin_nano',
  name: 'Jetson Orin Nano Super',
  backends: { gpio: 'libgpiod', i2c: 'linux', camera: 'nvargus' }
};
const rpiBoard = {
  id: 'rpi_zero2w',
  name: 'Raspberry Pi Zero 2 W',
  backends: { gpio: 'libgpiod', i2c: 'linux', camera: 'libcamera' }
};
const mockPins = { pins: [{ pin: 3, mode: 'sfio', state: null, sfio: true, label: 'SDA' }] };

assert(hw.shouldShowGpuPanel(jetsonBoard) === true, 'gpu visible jetson');
assert(hw.shouldShowGpuPanel(rpiBoard) === false, 'gpu hidden rpi');
assert(hw.JETSON_BOARD_ID === 'jetson_orin_nano', 'jetson board constant');

const jetsonHtml = hw.hardwarePageMarkup({
  boardId: jetsonBoard.id,
  board: jetsonBoard,
  gpio: mockPins,
  gpu: { available: false, reason: 'tegrastats unavailable' }
});
assert(jetsonHtml.indexOf('data-hw-tab="gpu"') !== -1, 'jetson gpu tab');
assert(jetsonHtml.indexOf('data-hw-panel="gpu"') !== -1, 'jetson gpu panel');
assert(jetsonHtml.indexOf('jetson_orin_nano') !== -1, 'jetson board id');

const rpiHtml = hw.hardwarePageMarkup({
  boardId: rpiBoard.id,
  board: rpiBoard,
  gpio: mockPins,
  gpu: {}
});
assert(rpiHtml.indexOf('data-hw-tab="gpu"') === -1, 'rpi no gpu tab');
assert(rpiHtml.indexOf('data-hw-panel="gpu"') === -1, 'rpi no gpu panel');
assert(rpiHtml.indexOf('rpi_zero2w') !== -1, 'rpi board id');

assert(rpiHtml.indexOf('data-hw-panel="sensors"') !== -1, 'sensors panel');
assert(rpiHtml.indexOf('data-hw-panel="camera"') !== -1, 'camera panel');
assert(rpiHtml.indexOf('Coming in v1.2 (Phase 7)') !== -1, 'deferred note');
assert(hw.deferredPlaceholderMarkup('Sensors').indexOf('hw-deferred-card') !== -1, 'deferred card');

const boardPanel = hw.boardPanelMarkup(jetsonBoard, { bus: 7, addresses: [0x76] });
assert(boardPanel.indexOf('hw-i2c-scan') !== -1, 'board panel scan button');
assert(boardPanel.indexOf('0x76') !== -1, 'board panel i2c addresses');
assert(hw.boardPanelMarkup(jetsonBoard, { loading: true }).indexOf('Scanning I2C') !== -1,
  'board panel i2c loading');

async function testHardwareFetchMocks() {
  const responses = {
    '/api/hardware/board': jetsonBoard,
    '/api/hardware/gpio': mockPins,
    '/api/hardware/gpu': { available: false, reason: 'tegrastats unavailable' },
    '/api/hardware/sensors': {
      status: 'deferred_v12',
      message: 'sensor decoders ship in v1.2 (Phase 7)'
    }
  };
  global.fetch = async function (url) {
    const path = String(url).replace(/^https?:\/\/[^/]+/, '');
    const data = responses[path];
    if (!data) {
      return { ok: false, status: 404, json: async () => ({ error: 'not found' }) };
    }
    return { ok: true, status: 200, json: async () => data };
  };
  const boardRes = await fetch('/api/hardware/board');
  const boardJson = await boardRes.json();
  const gpioRes = await fetch('/api/hardware/gpio');
  const gpioJson = await gpioRes.json();
  const sensorsRes = await fetch('/api/hardware/sensors');
  const sensorsJson = await sensorsRes.json();
  assert(boardJson.id === 'jetson_orin_nano', 'fetch board');
  assert(Array.isArray(gpioJson.pins), 'fetch gpio pins');
  assert(sensorsJson.status === 'deferred_v12', 'fetch sensors deferred');
  const html = hw.hardwarePageMarkup({
    boardId: boardJson.id,
    board: boardJson,
    gpio: gpioJson,
    gpu: responses['/api/hardware/gpu']
  });
  assert(html.indexOf('data-hw-panel="sensors"') !== -1, 'render sensors panel');
  delete global.fetch;
}

testHardwareFetchMocks().then(function () {
  console.log('test_web_dashboard: all tests passed');
}).catch(function (err) {
  console.error('FAIL:', err && err.message ? err.message : err);
  process.exit(1);
});

async function testDashboardResponseAfterNavigation() {
  const location = { protocol: 'http:', host: 'localhost', hash: '#/' };
  const elements = { app: {}, 'mem-q': {}, 'mem-results': {} };
  let navigate;
  let release;
  let timers = 0;
  const response = { ok: true, status: 200, json: async () => ({}) };
  const delayed = new Promise(resolve => { release = resolve; });
  const script = fs.readFileSync(path.join(__dirname, '../web/js/app.js'), 'utf8');
  vm.runInNewContext(script, {
    location, ShellClawDashboard: dash,
    window: { location, addEventListener: (name, cb) => { if (name === 'hashchange') navigate = cb; },
      setInterval: () => ++timers, clearInterval() {} },
    localStorage: { getItem: () => 'paired-device' },
    document: { getElementById: id => elements[id], querySelectorAll: () => [] },
    WebSocket: function () {},
    fetch: url => url === '/api/status' ? delayed : Promise.resolve(response),
  });
  location.hash = '#/memory';
  navigate();
  release(response);
  await new Promise(resolve => setImmediate(resolve));
  assert(elements.app.innerHTML.includes('<h1>Memory</h1>'), 'late dashboard response replaced current view');
  assert(timers === 0, 'inactive dashboard started status timers');
}
testDashboardResponseAfterNavigation().catch(error => { console.error(error); process.exitCode = 1; });

async function testSignedManifestCards() {
  const location = { hash: '#/asap' };
  const elements = { app: {}, 'asap-content': {} };
  const signed = { manifest: { id: 'urn:asap:agent:test', name: 'Gateway',
    capabilities: { asap_version: '2.1.0' } }, signature: { algorithm: 'ed25519' } };
  const script = fs.readFileSync(path.join(__dirname, '../web/js/app.js'), 'utf8');
  vm.runInNewContext(script, {
    location, window: { location, addEventListener() {} }, ShellClawDashboard: dash,
    document: { getElementById: id => elements[id], querySelectorAll: () => [] },
    localStorage: { getItem: () => 'paired-device' },
    fetch: async url => ({ ok: true, json: async () => url.includes('manifest') ? signed : { entries: [] } }),
  });
  await new Promise(resolve => setImmediate(resolve));
  const content = elements['asap-content'].innerHTML;
  for (const value of ['urn:asap:agent:test', 'Gateway', '2.1.0'])
    assert(content.includes('>' + value + '</div>'), 'missing manifest card ' + value);
  assert(content.includes('ed25519'), 'missing original signature');
}
testSignedManifestCards().catch(error => { console.error(error); process.exitCode = 1; });
