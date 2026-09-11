/**
 * Dashboard markup helpers (provider/Discord/context panels).
 * Loaded before app.js in dev; concatenated into embedded bundle via embed_ui.sh.
 */
(function (global) {
  'use strict';

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

  function coerceBool(v) {
    if (v === true || v === 1 || v === '1' || v === 'true') return true;
    if (v === false || v === 0 || v === '0' || v === 'false') return false;
    return !!v;
  }

  /** Provider semantics: GREEN = primary reachable; YELLOW = fallback/standby; RED = unreachable. */
  function providerSemanticClass(role, reachable) {
    if (!reachable) return 'dash-red';
    if (role === 'primary') return 'dash-green';
    if (role === 'fallback' || role === 'standby') return 'dash-yellow';
    return 'dash-red';
  }

  /** Discord lifecycle: GREEN = connected; YELLOW = disabled/connecting; RED = disconnected. */
  function discordSemanticClass(lifecycle) {
    const lc = lifecycle || '';
    if (lc === 'connected') return 'dash-green';
    if (lc === 'disconnected') return 'dash-red';
    if (lc === 'disabled') return 'dash-yellow';
    return 'dash-yellow';
  }

  function pickStatusSlice(wsMsg) {
    if (!wsMsg || typeof wsMsg !== 'object') return null;
    const o = Object.assign({}, wsMsg);
    delete o.type;
    return o;
  }

  /**
   * Merge WebSocket provider_status deltas into the last polled /api/status snapshot.
   */
  function mergeStatusWithWs(prev, wsMsg) {
    const slice = pickStatusSlice(wsMsg);
    if (!slice || !prev) return prev;
    const out = Object.assign({}, prev);
    if (slice.active_provider !== undefined) out.active_provider = slice.active_provider;
    if (slice.providers !== undefined) out.providers = slice.providers;
    if (slice.generated_at !== undefined) out.generated_at = slice.generated_at;
    if (Object.prototype.hasOwnProperty.call(slice, 'last_error')) out.last_error = slice.last_error;
    return out;
  }

  function dashboardMarkup(health, status, ctxSnap) {
    const h = health || {};
    const st = status || {};
    const d0 = ctxSnap || {};
    const dashLines = (d0.dashboard && typeof d0.dashboard === 'object') ? d0.dashboard : {};
    let provRows = '';
    const providers = Array.isArray(st.providers) ? st.providers : [];
    const activeProv = typeof st.active_provider === 'string' ? st.active_provider : '';
    providers.forEach(function (row) {
      const name = escapeHtml(row.name || '-');
      const role = row.role || '';
      const reachable = coerceBool(row.reachable);
      const sem = providerSemanticClass(role, reachable);
      const activeMark = activeProv && row.name === activeProv ? ' *' : '';
      provRows += '<tr>' +
        '<td class="' + sem + '">' + name + activeMark + '</td>' +
        '<td>' + escapeHtml(role) + '</td>' +
        '<td>' + (reachable ? 'yes' : 'no') + '</td>' +
        '</tr>';
    });
    let discordLc = '';
    let discordRs = '';
    if (st.discord && typeof st.discord === 'object') {
      discordLc = st.discord.lifecycle || '';
      discordRs = st.discord.reason || '';
    }
    const dClass = discordSemanticClass(discordLc);
    const lastErr = st.last_error ? '<div class="dash-banner dash-banner-err">' + escapeHtml(st.last_error) + '</div>' : '';
    let ctxBlock = '';
    if (dashLines.note) ctxBlock += '<div class="dash-kv">' + escapeHtml(String(dashLines.note)) + '</div>';
    if (dashLines.location_hint) ctxBlock += '<div class="dash-kv"><span class="dash-label">coords</span> ' + escapeHtml(String(dashLines.location_hint)) + '</div>';
    if (dashLines.location_line) ctxBlock += '<div class="dash-kv"><span class="dash-label">place</span> ' + escapeHtml(String(dashLines.location_line)) + '</div>';
    if (dashLines.weather_line) ctxBlock += '<div class="dash-kv">' + escapeHtml(String(dashLines.weather_line)) + '</div>';
    if (dashLines.holiday_line) ctxBlock += '<div class="dash-kv">' + escapeHtml(String(dashLines.holiday_line)) + '</div>';
    if (!ctxBlock) ctxBlock = '<p class="status-note">Invoke get_context to populate enrichment.</p>';

    let cards = '';
    cards += '<div class="dashboard-cards">';
    cards += '<div class="card"><div class="card-label">Health</div><div class="card-value status-ok">' + escapeHtml(h.status || 'ok') + '</div></div>';
    cards += '<div class="card"><div class="card-label">Uptime</div><div class="card-value">' + escapeHtml(h.uptime != null ? h.uptime + 's' : '-') + '</div></div>';
    cards += '<div class="card"><div class="card-label">Sandbox</div><div class="card-value ' + (coerceBool(h.sandbox_enabled) ? 'status-ok' : 'status-warn') + '">' +
      (coerceBool(h.sandbox_enabled) ? 'enabled' : 'disabled') + '</div></div>';
    cards += '<div class="card"><div class="card-label">ASAP</div><div class="card-value ' + (coerceBool(h.asap_enabled) ? 'status-ok' : 'status-warn') + '">' +
      (coerceBool(h.asap_enabled) ? 'live' : 'off') + '</div></div>';
    cards += '<div class="card"><div class="card-label">Active provider</div><div class="card-value">' + escapeHtml(activeProv || '-') + '</div></div>';
    cards += '</div>';

    return '' +
      '<h1>Dashboard</h1>' +
      '<p class="dash-meta">' +
      '<span class="status-note">Polling /api/status; WebSocket merges provider deltas; snapshot from /api/context/snapshot.</span>' +
      '</p>' +
      lastErr +
      cards +
      '<h2 class="dash-h2">Providers</h2>' +
      '<table class="dash-table"><thead><tr><th>Name</th><th>Role</th><th>Reachable</th></tr></thead><tbody>' +
      (provRows || '<tr><td colspan="3" class="status-note">No provider rows.</td></tr>') +
      '</tbody></table>' +
      '<h2 class="dash-h2">Discord</h2>' +
      '<table class="dash-table"><tbody>' +
      '<tr><td class="' + dClass + '">lifecycle</td><td>' + escapeHtml(discordLc || '-') + '</td></tr>' +
      (discordRs ? '<tr><td colspan="2" class="status-note">' + escapeHtml(discordRs) + '</td></tr>' : '') +
      '</tbody></table>' +
      '<h2 class="dash-h2">Context cache</h2>' +
      '<div class="dash-context">' + ctxBlock + '</div>';
  }

  const api = {
    escapeHtml: escapeHtml,
    coerceBool: coerceBool,
    providerSemanticClass: providerSemanticClass,
    discordSemanticClass: discordSemanticClass,
    pickStatusSlice: pickStatusSlice,
    mergeStatusWithWs: mergeStatusWithWs,
    dashboardMarkup: dashboardMarkup
  };

  global.ShellClawDashboard = api;

  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api;
  }
})(typeof globalThis !== 'undefined' ? globalThis : window);
