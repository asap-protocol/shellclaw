'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const zlib = require('node:zlib');

const root = path.join(__dirname, '..');
const header = fs.readFileSync(path.join(root, 'src/gateway/ui_assets.h'), 'utf8');
for (const [asset, sources] of [
  ['ui_app_js_gz', ['dashboardView.js', 'app.js']],
  ['ui_hardware_js_gz', ['hardwareView.js']],
]) {
  const match = header.match(new RegExp(asset + '\\[\\]\\s*=\\s*\\{([^}]+)\\}'));
  assert.ok(match, 'Missing asset ' + asset);
  const bytes = Buffer.from(match[1].match(/0x[\da-f]{2}/gi).map(byte => Number(byte)));
  const script = zlib.gunzipSync(bytes).toString('utf8');
  new vm.Script(script, { filename: asset });
  for (const source of sources) {
    const original = fs.readFileSync(path.join(root, 'web/js', source), 'utf8');
    assert.ok(script.includes(original), 'Changed source text in ' + source);
  }
}
console.log('Both embedded JavaScript files passed.');
