/* ShellClaw Web UI - vanilla JS SPA (dashboard markup in dashboardView.js) */
(function () {
  const TOKEN_KEY = 'shellclaw_token';
  const API_BASE = '';
  const dash = typeof ShellClawDashboard !== 'undefined' ? ShellClawDashboard : {};
  const escapeHtml = dash.escapeHtml;
  const dashboardMarkup = dash.dashboardMarkup;
  const mergeStatusWithWs = dash.mergeStatusWithWs || function (prev) { return prev; };

  function getToken() { return localStorage.getItem(TOKEN_KEY); }
  function setToken(t) { if (t) localStorage.setItem(TOKEN_KEY, t); else localStorage.removeItem(TOKEN_KEY); }

  function api(method, path, body) {
    const opts = { method, headers: {} };
    const token = getToken();
    if (token) opts.headers['Authorization'] = 'Bearer ' + token;
    if (body) {
      opts.headers['Content-Type'] = 'application/json';
      opts.body = typeof body === 'string' ? body : JSON.stringify(body);
    }
    return fetch(API_BASE + path, opts).then(function (r) {
      if (r.status === 401) { setToken(null); window.location.hash = '#/pair'; return Promise.reject('Unauthorized'); }
      if (!r.ok) {
        return r.text().then(function (t) {
          return Promise.reject(t || ('HTTP ' + r.status));
        });
      }
      return r.json().catch(function () { return ({}); });
    });
  }

  function wsConnect(onMessage, repliesOnly = false) {
    const token = getToken();
    if (!token) return null;
    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const path = repliesOnly ? '/ws?view=chat' : '/ws';
    const ws = new WebSocket(proto + '//' + location.host + path, ['ws', 'bearer.' + token]);
    if (repliesOnly) {
      ws.onmessage = e => onMessage(e.data);
    } else {
      ws.onmessage = e => { try { const d = JSON.parse(e.data); onMessage(d); } catch (_) {} };
    }
    return ws;
  }

  let dashboardLiveStop = null;

  function stopDashboardLive() {
    if (dashboardLiveStop) {
      dashboardLiveStop();
      dashboardLiveStop = null;
    }
  }

  function cloneWithoutType(wsMsg) {
    if (!wsMsg || typeof wsMsg !== 'object') return null;
    try {
      const o = JSON.parse(JSON.stringify(wsMsg));
      if (o && typeof o === 'object') delete o.type;
      return o;
    } catch (_) {
      return null;
    }
  }

  const routes = {
    '/': renderDashboard,
    '/chat': renderChat,
    '/config': renderConfig,
    '/skills': renderSkills,
    '/memory': renderMemory,
    '/sessions': renderSessions,
    '/cron': renderCron,
    '/logs': renderLogs,
    '/asap': renderAsap,
    '/pair': renderPair
  };

  function getRoute() {
    const h = window.location.hash.slice(1) || '/';
    return h.split('?')[0];
  }

  function render(el, html) {
    el.innerHTML = html;
  }

  function renderDashboard() {
    const token = getToken();
    if (!token) { window.location.hash = '#/pair'; return; }
    stopDashboardLive();
    render(app, '<h1>Dashboard</h1><p class="status-note">Loading live status…</p>');
    let active = true;
    let timer, ctxTimer, ws;
    dashboardLiveStop = function () {
      active = false;
      window.clearInterval(timer);
      window.clearInterval(ctxTimer);
      if (ws) ws.close();
    };
    let lastHealth = {};
    let lastStatus = {};
    let lastCtx = {};
    function paint() {
      if (active) render(app, dashboardMarkup(lastHealth, lastStatus, lastCtx));
    }
    Promise.all([
      api('GET', '/health'),
      api('GET', '/api/status'),
      api('GET', '/api/context/snapshot')
    ]).then(function (results) {
      if (!active) return;
      lastHealth = results[0] || {};
      lastStatus = results[1] || {};
      lastCtx = results[2] || {};
      paint();
      timer = window.setInterval(function () {
        api('GET', '/api/status').then(function (st) {
          lastStatus = st || lastStatus;
          paint();
        }).catch(function () {});
      }, 12000);
      ctxTimer = window.setInterval(function () {
        api('GET', '/api/context/snapshot').then(function (snap) {
          lastCtx = snap || lastCtx;
          paint();
        }).catch(function () {});
      }, 45000);
      ws = wsConnect(function (msg) {
        if (!msg || msg.type !== 'provider_status') return;
        lastStatus = mergeStatusWithWs(lastStatus, msg);
        paint();
      });
    }).catch(function (err) {
      if (!active) return;
      render(app, '<h1>Dashboard</h1><p class="status-err">' + escapeHtml(err || 'Load failed') + '</p>');
    });
  }

  function renderChat() {
    const token = getToken();
    if (!token) { window.location.hash = '#/pair'; return; }
    render(app, '<h1>Chat</h1><div id="chat-log"></div><input id="chat-input" placeholder="Type message...">');
    const log = document.getElementById('chat-log');
    const input = document.getElementById('chat-input');
    const ws = wsConnect(function (text) {
      log.textContent += text + '\n';
      log.scrollTop = log.scrollHeight;
    }, true);
    if (!ws) { log.textContent = 'Not authenticated. Go to pairing.'; return; }
    input.onkeydown = function (e) {
      if (e.key !== 'Enter') return;
      const text = input.value.trim();
      if (!text) return;
      ws.send(JSON.stringify({ type: 'message', text }));
      log.textContent += 'You: ' + text + '\n';
      input.value = '';
      log.scrollTop = log.scrollHeight;
    };
  }

  function renderConfig() {
    const token = getToken();
    if (!token) { window.location.hash = '#/pair'; return; }
    api('GET', '/api/config').then(d => {
      render(app, '<h1>Config</h1><form class="config-form" id="config-form">' +
        '<div class="form-row"><label>Model</label><input name="model" value="' + escapeHtml(d.model || '') + '"></div>' +
        '<div class="form-row"><label>Max tokens</label><input type="number" name="max_tokens" value="' + escapeHtml(String(d.max_tokens ?? 4096)) + '"></div>' +
        '<div class="form-row"><label>Temperature</label><input type="number" step="0.1" name="temperature" value="' + escapeHtml(String(d.temperature ?? 0.7)) + '"></div>' +
        '<div class="form-row"><label>Gateway host</label><input name="gateway_host" value="' + escapeHtml(d.gateway_host || '') + '"></div>' +
        '<div class="form-row"><label>Gateway port</label><input type="number" name="gateway_port" value="' + escapeHtml(String(d.gateway_port ?? 18789)) + '"></div>' +
        '<p>Gateway host and port are saved now. The process keeps the current listen address until restart.</p>' +
        '<button type="submit">Save</button></form>');
      document.getElementById('config-form').onsubmit = function (e) {
        e.preventDefault();
        const f = e.target;
        const body = {
          model: f.model.value,
          max_tokens: parseInt(f.max_tokens.value, 10),
          temperature: parseFloat(f.temperature.value),
          gateway_host: f.gateway_host.value,
          gateway_port: parseInt(f.gateway_port.value, 10)
        };
        api('PUT', '/api/config', body).then(() => { alert('Saved'); }).catch(err => alert('Error: ' + err));
      };
    }).catch(e => { render(app, '<h1>Config</h1><p class="status-err">' + escapeHtml(e || 'Load failed') + '</p>'); });
  }

  function renderSkills() {
    const token = getToken();
    if (!token) { window.location.hash = '#/pair'; return; }
    api('GET', '/api/skills').then(arr => {
      let html = '<h1>Skills</h1><ul class="skill-list" id="skill-list"></ul>' +
        '<h2>Create skill</h2><form id="skill-create"><input name="name" placeholder="Name"><textarea name="content" placeholder="Content" rows="4"></textarea><button type="submit">Create</button></form>';
      render(app, html);
      const ul = document.getElementById('skill-list');
      (arr || []).forEach(n => {
        const li = document.createElement('li');
        li.innerHTML = '<span>' + escapeHtml(n) + '</span><span><a href="#/skills/' + encodeURIComponent(n) + '">View</a> <button class="danger" data-delete="' + escapeHtml(n) + '">Delete</button></span>';
        li.querySelector('[data-delete]').onclick = () => {
          if (!confirm('Delete ' + n + '?')) return;
          api('DELETE', '/api/skills/' + encodeURIComponent(n)).then(() => { window.location.hash = '#/skills'; });
        };
        ul.appendChild(li);
      });
      document.getElementById('skill-create').onsubmit = function (e) {
        e.preventDefault();
        const f = e.target;
        api('POST', '/api/skills', { name: f.name.value, content: f.content.value }).then(() => { window.location.hash = '#/skills'; });
      };
    }).catch(e => { render(app, '<h1>Skills</h1><p class="status-err">' + escapeHtml(e || 'Load failed') + '</p>'); });
  }

  function renderSkillView(name) {
    const token = getToken();
    if (!token) { window.location.hash = '#/pair'; return; }
    api('GET', '/api/skills/' + encodeURIComponent(name)).then(d => {
      render(app, '<h1>Skill: ' + escapeHtml(name) + '</h1><textarea id="skill-content" rows="12" style="width:100%">' + escapeHtml(d.content || '') + '</textarea><button id="skill-save">Save</button> <a href="#/skills">Back</a>');
      document.getElementById('skill-save').onclick = () => {
        api('PUT', '/api/skills/' + encodeURIComponent(name), { content: document.getElementById('skill-content').value }).then(() => alert('Saved'));
      };
    }).catch(e => { render(app, '<h1>Skill</h1><p class="status-err">' + escapeHtml(e || 'Not found') + '</p>'); });
  }

  function renderMemory() {
    const token = getToken();
    if (!token) { window.location.hash = '#/pair'; return; }
    render(app, '<h1>Memory</h1><input id="mem-q" placeholder="Search..." style="width:100%;margin-bottom:0.5rem"><div id="mem-results" class="pre-wrap"></div>');
    const q = document.getElementById('mem-q');
    const res = document.getElementById('mem-results');
    function search() {
      const query = q.value.trim();
      api('GET', '/api/memory?q=' + encodeURIComponent(query) + '&limit=20').then(d => {
        res.textContent = (d.results || 'No results');
      }).catch(e => { res.textContent = 'Error: ' + e; });
    }
    q.onkeydown = e => { if (e.key === 'Enter') search(); };
    q.oninput = () => { if (q.value.length >= 2) search(); };
  }

  function renderSessions() {
    const token = getToken();
    if (!token) { window.location.hash = '#/pair'; return; }
    api('GET', '/api/sessions').then(arr => {
      let html = '<h1>Sessions</h1><ul class="session-list" id="session-list"></ul>';
      render(app, html);
      const ul = document.getElementById('session-list');
      (arr || []).forEach(id => {
        const li = document.createElement('li');
        li.innerHTML = '<span>' + escapeHtml(id) + '</span><button class="danger" data-delete="' + escapeHtml(id) + '">Delete</button>';
        li.querySelector('[data-delete]').onclick = () => {
          api('DELETE', '/api/sessions/' + encodeURIComponent(id)).then(() => { window.location.hash = '#/sessions'; });
        };
        ul.appendChild(li);
      });
    }).catch(e => { render(app, '<h1>Sessions</h1><p class="status-err">' + escapeHtml(e || 'Load failed') + '</p>'); });
  }

  function renderCron() {
    if (!getToken()) { window.location.hash = '#/pair'; return; }
    render(app, '<h1>Cron</h1><form id="cron-create">' +
      '<label>Schedule <input name="schedule" required placeholder="interval:3600"></label>' +
      '<p>Use interval:seconds, at:unix_time, or five cron fields. The gateway uses its local time. ' +
      'The day of the month and the weekday must both match.</p>' +
      '<label>Message <textarea name="message" required></textarea></label>' +
      '<label for="cron-channel">Channel</label><select id="cron-channel" name="channel"><option value="cli">CLI</option>' +
      '<option value="telegram">Telegram</option><option value="discord">Discord</option></select>' +
      '<label>Recipient <input name="recipient" placeholder="default"></label>' +
      '<button type="button" id="cron-check">Preview next run</button> ' +
      '<button type="submit" disabled>Create job</button><p id="cron-preview" aria-live="polite"></p></form>' +
      '<p id="cron-result" role="alert"></p><h2>Scheduled jobs</h2><div id="cron-jobs"></div>' +
      '<button type="button" id="cron-prev" disabled>Previous page</button> ' +
      '<button type="button" id="cron-next" disabled>Next page</button>');
    const form = document.getElementById('cron-create');
    const fields = form.elements;
    const create = form.querySelector('button[type=submit]');
    const preview = document.getElementById('cron-preview');
    const result = document.getElementById('cron-result');
    const jobs = document.getElementById('cron-jobs');
    const prev = document.getElementById('cron-prev');
    const next = document.getElementById('cron-next');
    const cursors = [''];
    let nextAfter = null;
    let request = 0;
    let previewRequest = 0;
    let checkedSchedule = '';
    let saving = false;

    function timeText(seconds) {
      const date = new Date(seconds * 1000);
      return Number.isFinite(date.getTime()) ? date.toLocaleString(undefined, {timeZoneName:'short'}) : String(seconds);
    }

    function showError(error) {
      let text = String(error);
      try { text = JSON.parse(text).error || text; } catch (_) {}
      result.textContent = text;
    }

    function cell(row, value) {
      row.insertCell().textContent = value;
    }

    function action(cell, label, method, path, id) {
      const button = document.createElement('button');
      button.type = 'button';
      button.textContent = label;
      cell.appendChild(button);
      button.onclick = async function () {
        if (method === 'DELETE' && !window.confirm('Delete job ' + id + '?')) return;
        for (const control of cell.querySelectorAll('button')) control.disabled = true;
        try {
          await api(method, path, method === 'POST' ? {} : undefined);
          result.textContent = '';
          await load();
        } catch (error) {
          showError(error);
          for (const control of cell.querySelectorAll('button')) control.disabled = false;
        }
      };
    }

    async function load() {
      const revision = ++request;
      const after = cursors[cursors.length - 1];
      prev.disabled = next.disabled = true;
      try {
        const page = await api('GET', '/api/cron/page?after=' + encodeURIComponent(after));
        if (revision !== request) return;
        nextAfter = page.next_after;
        if (page.jobs.length === 0 && cursors.length > 1) {
          cursors.pop();
          return load();
        }
        jobs.textContent = '';
        if (page.jobs.length === 0) jobs.textContent = 'No scheduled jobs.';
        else {
          const table = document.createElement('table');
          table.className = 'dash-table';
          table.innerHTML = '<thead><tr><th>Job</th><th>Schedule</th><th>Message</th><th>Destination</th>' +
            '<th>Next run</th><th>Status</th><th>Actions</th></tr></thead>';
          const body = table.createTBody();
          const states = {scheduled:'Scheduled', paused:'Paused', waiting_for_channel:'Waiting for channel',
            invalid_destination:'Invalid destination'};
          for (const job of page.jobs) {
            const row = body.insertRow();
            cell(row, job.id);
            cell(row, job.schedule_preview);
            cell(row, job.message_preview);
            cell(row, job.channel + ' / ' + job.recipient);
            cell(row, timeText(job.next_run));
            cell(row, states[job.delivery_status] || job.delivery_status);
            const controls = row.insertCell();
            const path = '/api/cron/job/' + encodeURIComponent(job.key);
            action(controls, job.enabled ? 'Pause' : 'Resume', 'POST', path + '/toggle', job.id);
            action(controls, 'Delete', 'DELETE', path, job.id);
          }
          jobs.appendChild(table);
        }
      } catch (error) {
        if (revision !== request) return;
        nextAfter = null;
        showError(error);
        jobs.textContent = 'Could not load the current jobs. ';
        const retry = document.createElement('button');
        retry.type = 'button';
        retry.textContent = 'Retry list';
        retry.onclick = () => { result.textContent = ''; load(); };
        jobs.appendChild(retry);
      }
      prev.disabled = cursors.length === 1;
      next.disabled = !nextAfter;
    }
    prev.onclick = () => { cursors.pop(); load(); };
    next.onclick = () => { cursors.push(nextAfter); load(); };

    function clearPreview() {
      previewRequest++;
      checkedSchedule = '';
      create.disabled = true;
      preview.textContent = '';
    }
    fields.schedule.oninput = clearPreview;
    document.getElementById('cron-check').onclick = async function () {
      clearPreview();
      const revision = previewRequest;
      const schedule = fields.schedule.value.trim();
      result.textContent = '';
      try {
        const data = await api('POST', '/api/cron/preview', {schedule});
        if (revision !== previewRequest) return;
        checkedSchedule = schedule;
        preview.textContent = 'Next run: ' + timeText(data.next_run);
        create.disabled = saving;
      } catch (error) { if (revision === previewRequest) showError(error); }
    };
    form.onsubmit = async function (event) {
      event.preventDefault();
      const schedule = fields.schedule.value.trim();
      if (saving || !checkedSchedule || schedule !== checkedSchedule) return;
      saving = true;
      create.disabled = true;
      try {
        const data = await api('POST', '/api/cron', {schedule, message:fields.message.value,
          channel:fields.channel.value, recipient:fields.recipient.value.trim()});
        result.textContent = 'Created job ' + data.id + '.';
        form.reset();
        clearPreview();
        cursors.length = 1;
        await load();
      } catch (error) { showError(error); }
      finally { saving = false; create.disabled = !checkedSchedule; }
    };
    load();
  }

  function renderLogs() {
    const token = getToken();
    if (!token) { window.location.hash = '#/pair'; return; }
    render(app, '<h1>Logs</h1><div id="log-output" class="pre-wrap"></div>');
    const out = document.getElementById('log-output');
    const ws = wsConnect(function (msg) {
      if (msg.type === 'log') {
        const cls = 'log-' + escapeHtml(msg.level || 'info');
        const line = document.createElement('div');
        line.className = 'log-line ' + cls;
        line.textContent = msg.text || '';
        out.appendChild(line);
        out.scrollTop = out.scrollHeight;
      }
    });
    if (!ws) out.textContent = 'Not authenticated.';
  }

  function renderAsap() {
    const token = getToken();
    if (!token) { window.location.hash = '#/pair'; return; }
    render(app, '<h1>ASAP</h1><div id="asap-content"><p class="status-warn">Loading...</p></div>');
    const content = document.getElementById('asap-content');
    const manifestP = fetch(API_BASE + '/.well-known/asap/manifest.json')
      .then(r => r.ok ? r.json() : Promise.reject('HTTP ' + r.status))
      .catch(() => null);
    const logP = api('GET', '/api/asap/log').catch(() => null);
    Promise.all([manifestP, logP]).then(function (results) {
      const manifest = results[0];
      const logData = results[1];
      let html = '';
      /* Dashboard cards */
      html += '<div class="dashboard-cards">';
      if (manifest) {
        html += '<div class="card"><div class="card-label">Agent URN</div><div class="card-value">' + escapeHtml(manifest.urn || manifest.agent_urn || '-') + '</div></div>';
        html += '<div class="card"><div class="card-label">ASAP Version</div><div class="card-value status-ok">' + escapeHtml(manifest.asap_version || '-') + '</div></div>';
        html += '<div class="card"><div class="card-label">Agent Name</div><div class="card-value">' + escapeHtml(manifest.name || '-') + '</div></div>';
      } else {
        html += '<div class="card"><div class="card-label">Manifest</div><div class="card-value status-err">unavailable</div></div>';
      }
      html += '</div>';
      /* Manifest detail */
      if (manifest) {
        html += '<h2>Manifest</h2><pre class="pre-wrap">' + escapeHtml(JSON.stringify(manifest, null, 2)) + '</pre>';
      }
      /* Envelope log table */
      html += '<h2>Envelope Log</h2>';
      if (logData && Array.isArray(logData.entries) && logData.entries.length > 0) {
        html += '<table class="asap-log-table"><thead><tr><th>Time</th><th>Dir</th><th>Type</th><th>ID</th></tr></thead><tbody>';
        logData.entries.forEach(function (e) {
          const dir = e.direction === 'in' ? '&larr; in' : '&rarr; out';
          html += '<tr><td>' + escapeHtml(e.timestamp || '-') + '</td><td>' + dir + '</td><td>' + escapeHtml(e.payload_type || '-') + '</td><td class="mono">' + escapeHtml((e.id || '-').substring(0, 26)) + '</td></tr>';
        });
        html += '</tbody></table>';
      } else if (logData && Array.isArray(logData.entries)) {
        html += '<p class="status-note">No envelope events recorded yet.</p>';
      } else {
        html += '<p class="status-warn">Could not load envelope log (unauthorized or server error).</p>';
      }
      content.innerHTML = html;
    });
  }

  function renderPair() {
    render(app, '<h1>Pair</h1><p>Enter the 6-digit pairing code shown on the server.</p>' +
      '<form class="pair-form" id="pair-form"><input name="code" placeholder="Code" maxlength="6" autocomplete="off">' +
      '<button type="submit">Pair</button></form>');
    document.getElementById('pair-form').onsubmit = function (e) {
      e.preventDefault();
      const code = e.target.code.value.trim();
      api('POST', '/pair', { code }).then(d => {
        if (d.token) { setToken(d.token); window.location.hash = '#/'; }
      }).catch(err => alert('Invalid code: ' + err));
    };
  }

  const app = document.getElementById('app');
  function navigate() {
    stopDashboardLive();
    const route = getRoute();
    const m = route.match(/^\/skills\/(.+)$/);
    if (m) { renderSkillView(decodeURIComponent(m[1])); return; }
    const fn = routes[route] || routes['/'];
    if (fn) fn();
    document.querySelectorAll('nav a').forEach(a => {
      a.classList.toggle('active', a.getAttribute('href') === '#' + route);
    });
  }
  window.addEventListener('hashchange', navigate);
  navigate();
})();
