'use strict';
/* Lab Scheduler frontend.
   The browser never decides who gets a computer: it asks the C++ server for the
   finished schedule (/api/simulate) and then only *draws* it, replaying the
   timeline minute by minute. */

const $ = (sel, root = document) => root.querySelector(sel);
const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

const app = {
  cfg: null,        // { computers, policy, requests, limits, policies }
  result: null,     // schedule from /api/simulate
  compare: null,    // from /api/compare
  sweep: null,      // from /api/sweep
  time: 0, t0: 0, t1: 0,
  playing: false, speed: 8, lastFrame: 0,
  tab: 'sim',
  hues: new Map(),
  clearTimer: null,
};

const POLICY_NAMES = { FCFS: 'First-come, first-served', SJF: 'Shortest job first', PRIORITY: 'Priority' };
const POLICY_SHORT = { FCFS: 'FCFS', SJF: 'Shortest job', PRIORITY: 'Priority' };
const PRIORITY_LABELS = { 1: 'Urgent', 2: 'High', 3: 'Normal', 4: 'Low', 5: 'Lowest' };

/* ------------------------------------------------------------ helpers -- */
const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const f1 = (x) => (Math.round(x * 10) / 10).toFixed(1);
const pct = (x) => (x * 100).toFixed(1) + '%';
const plural = (n, one, many) => (n === 1 ? one : many);
const hueOf = (id) => app.hues.get(id) ?? 210;

let toastTimer = 0;
function toast(message, kind = 'info') {
  const el = $('#toast');
  el.textContent = message;
  el.dataset.kind = kind;
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { el.hidden = true; }, 4500);
}

async function api(method, url, body, raw = false) {
  const opts = { method, headers: {} };
  if (body !== undefined) {
    opts.body = raw ? body : JSON.stringify(body);
    opts.headers['Content-Type'] = raw ? 'text/plain' : 'application/json';
  }
  let res;
  try {
    res = await fetch(url, opts);
  } catch {
    throw new Error('Cannot reach the server. Check that lab_scheduler is still running.');
  }
  let data = null;
  try { data = await res.json(); } catch { /* not JSON */ }
  if (!res.ok) throw new Error((data && data.error) || `Request failed (${res.status}).`);
  return data;
}

/* -------------------------------------------------------- data loading -- */
async function loadAll() {
  const [cfg, result] = await Promise.all([api('GET', '/api/state'), api('GET', '/api/simulate')]);
  setData(cfg, result, true);
}

// Called after any change: `cfg` is the state the server returned.
async function afterChange(cfg) {
  const result = await api('GET', '/api/simulate');
  setData(cfg, result, false);
  app.compare = null;
  app.sweep = null;
  if (app.tab === 'compare') loadCompare();
  if (app.tab === 'capacity') loadSweep();
}

function setData(cfg, result, first) {
  app.cfg = cfg;
  app.result = result;
  app.hues = new Map(cfg.requests.map((r, i) => [r.id, Math.round(i * 137.508) % 360]));
  const m = result.metrics;
  app.t0 = m.totalRequests > 0 ? m.windowStart : 0;
  app.t1 = m.totalRequests > 0 ? m.windowEnd : 0;
  if (first || app.time < app.t0 || app.time > app.t1) app.time = app.t0;
  setPlaying(false);
  renderAll();
}

async function mutate(fn, okMessage) {
  try {
    const cfg = await fn();
    await afterChange(cfg);
    if (okMessage) toast(okMessage);
    return true;
  } catch (e) {
    toast(e.message, 'error');
    return false;
  }
}

async function loadCompare() {
  try { app.compare = await api('GET', '/api/compare'); renderCompare(); }
  catch (e) { toast(e.message, 'error'); }
}

async function loadSweep() {
  const max = Math.min(30, Math.max(12, app.cfg.computers + 4));
  try { app.sweep = await api('GET', `/api/sweep?max=${max}`); renderCapacity(); }
  catch (e) { toast(e.message, 'error'); }
}

/* ----------------------------------------------------------- rendering -- */
function renderAll() {
  renderControls();
  renderKpis();
  renderRequests();
  renderGantt();
  renderSchedule();
  renderTime();
  if (app.tab === 'compare' && app.compare) renderCompare();
  if (app.tab === 'capacity' && app.sweep) renderCapacity();
}

function renderControls() {
  const { cfg } = app;
  $$('#policy-seg button').forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.policy === cfg.policy)));
  const input = $('#pc-input');
  if (document.activeElement !== input) input.value = cfg.computers;
  input.max = cfg.limits.maxComputers;
  $('#pc-minus').disabled = cfg.computers <= 1;
  $('#pc-plus').disabled = cfg.computers >= cfg.limits.maxComputers;
  const pol = cfg.policies.find((p) => p.key === cfg.policy);
  $('#policy-desc').textContent = pol ? pol.description : '';
  $('#priority-hint').textContent = cfg.policy === 'PRIORITY' ? '1 is served first.' : 'Only used by the Priority policy.';
  $('#f-duration').max = cfg.limits.maxDuration;
  $('#f-arrival').max = cfg.limits.maxArrival;
  $('#f-id').maxLength = cfg.limits.maxIdLength;
  $('#th-pri').classList.toggle('dim', cfg.policy !== 'PRIORITY');
}

function renderKpis() {
  const r = app.result, m = r.metrics, has = m.totalRequests > 0;
  const worst = has && m.maxWait > 0 ? r.sessions.find((s) => s.wait === m.maxWait) : null;
  const items = [
    { label: 'Average wait', value: has ? f1(m.avgWait) : '–', unit: has ? 'min' : '',
      hint: has ? (m.studentsWhoWaited === 0 ? 'Nobody had to wait' : `${m.studentsWhoWaited} of ${m.totalRequests} students waited`) : 'No requests yet',
      tip: 'Average of (start time − arrival time) over every student' },
    { label: 'Longest wait', value: has ? m.maxWait : '–', unit: has ? 'min' : '',
      hint: worst ? `${worst.studentId} waited longest` : (has ? 'Nobody had to wait' : ''),
      tip: 'The worst waiting time of any single student' },
    { label: 'Computer utilization', value: has ? pct(m.utilization) : '–', unit: '',
      hint: has ? 'Busy time out of available time' : '',
      tip: 'Total session minutes ÷ (computers × minutes between the first arrival and the last finish)' },
    { label: 'Lab empties at', value: has ? `minute ${m.windowEnd}` : '–', unit: '',
      hint: has ? `${m.makespan} min after the first arrival` : '',
      tip: 'When the last session ends' },
    { label: 'Longest queue', value: has ? m.maxQueueLength : '–', unit: has ? plural(m.maxQueueLength, 'student', 'students') : '',
      hint: has ? (m.maxQueueLength === 0 ? 'A computer was always free' : 'Waiting at the same time') : '',
      tip: 'The most students waiting at the same moment' },
  ];
  $('#kpis').innerHTML = items.map((k) =>
    `<div class="kpi" title="${esc(k.tip)}"><dt>${k.label}</dt><dd class="kpi-value">${k.value}${k.unit ? `<span class="unit">${k.unit}</span>` : ''}</dd><dd class="kpi-hint">${esc(k.hint)}</dd></div>`
  ).join('');
}

function renderRequests() {
  const { cfg } = app;
  const n = cfg.requests.length;
  $('#req-count').textContent = n ? `(${n})` : '';
  $('#btn-clear').disabled = n === 0;
  const dim = cfg.policy !== 'PRIORITY' ? ' dim' : '';
  $('#req-body').innerHTML = n === 0
    ? '<tr><td colspan="5" class="empty-cell">No requests yet. Add one above, or load sample data.</td></tr>'
    : cfg.requests.map((r) =>
        `<tr><td><span class="dot" style="--h:${hueOf(r.id)}"></span>${esc(r.id)}</td>` +
        `<td class="num">${r.arrival}</td><td class="num">${r.duration}</td>` +
        `<td class="num${dim}" title="${PRIORITY_LABELS[r.priority] || ''}">${r.priority}</td>` +
        `<td><button type="button" class="row-remove" data-remove="${esc(r.id)}" aria-label="Remove ${esc(r.id)}">×</button></td></tr>`
      ).join('');
}

/* ---- lab floor, queue, counts: depend on the current time ---- */
function renderTime() {
  const r = app.result, t = app.time;
  const has = r.metrics.totalRequests > 0;

  const slider = $('#time-slider');
  slider.min = app.t0; slider.max = app.t1; slider.value = t;
  slider.disabled = !has;
  $('#btn-play').disabled = !has;
  $('#btn-restart').disabled = !has;
  $('#time-label').textContent = `Minute ${Math.floor(t)}`;

  const running = new Map();
  let notArrived = 0, done = 0;
  const waiting = [];
  for (const s of r.sessions) {
    if (s.arrival > t) notArrived++;
    else if (t < s.start) waiting.push(s);
    else if (t < s.end) running.set(s.computer, s);
    else done++;
  }
  waiting.sort((a, b) => a.start - b.start || a.computer - b.computer);  // true service order

  $('#counts').innerHTML =
    `<div><dt title="Not arrived yet (new)">Not arrived</dt><dd>${notArrived}</dd></div>` +
    `<div><dt title="Waiting for a computer (ready)">Waiting</dt><dd>${waiting.length}</dd></div>` +
    `<div><dt title="Using a computer (running)">Using a computer</dt><dd>${running.size}</dd></div>` +
    `<div><dt title="Session finished (terminated)">Finished</dt><dd>${done}</dd></div>`;

  let floor = '';
  for (let c = 1; c <= r.computers; c++) {
    const s = running.get(c);
    if (s) {
      const left = Math.max(0, Math.ceil(s.end - t));
      const prog = Math.min(100, Math.max(0, ((t - s.start) / s.duration) * 100));
      floor += `<li class="pc busy" style="--h:${hueOf(s.studentId)}" aria-label="Computer ${c}: ${esc(s.studentId)}, ${left} minutes left">` +
        `<div class="pc-screen"><span class="pc-name">PC ${c}</span><span class="pc-id">${esc(s.studentId)}</span>` +
        `<span class="pc-left">${left} min left</span><div class="pc-track"><div class="pc-fill" style="width:${prog}%"></div></div></div></li>`;
    } else {
      floor += `<li class="pc free" aria-label="Computer ${c}: free"><div class="pc-screen"><span class="pc-name">PC ${c}</span><span class="pc-free">Free</span></div></li>`;
    }
  }
  $('#floor').innerHTML = floor;

  $('#queue').innerHTML = !has
    ? '<li class="queue-empty">Add requests to see the line form.</li>'
    : waiting.length === 0
      ? '<li class="queue-empty">No one is waiting.</li>'
      : waiting.map((s, i) =>
          `<li class="chip${i === 0 ? ' next' : ''}"${i === 0 ? ' title="Next to get a computer"' : ''}><b>${esc(s.studentId)}</b><span>${Math.floor(t - s.arrival)} min</span></li>`
        ).join('');

  const now = $('#g-now');
  if (now) now.style.left = `${ganttPercent(t)}%`;
}

/* ---- gantt timeline ---- */
function ganttPercent(t) {
  const span = Math.max(1, app.t1 - app.t0);
  return Math.min(100, Math.max(0, ((t - app.t0) / span) * 100));
}

function niceStep(raw) {
  const steps = [1, 2, 5, 10, 15, 20, 30, 60, 120, 180, 240, 300, 600, 1200, 2400];
  return steps.find((s) => s >= raw) || 5000;
}

function renderGantt() {
  const r = app.result, host = $('#gantt');
  if (r.sessions.length === 0) {
    host.innerHTML = '<div class="empty-state"><strong>Nothing to schedule yet</strong>Add a request or load sample data to see the timeline.</div>';
    host.style.cursor = 'default';
    return;
  }
  host.style.cursor = '';
  const span = Math.max(1, app.t1 - app.t0);
  const step = niceStep(span / 10);
  const pctOf = (v) => ((v - app.t0) / span) * 100;

  let ticks = '', labels = '';
  for (let v = Math.ceil(app.t0 / step) * step; v <= app.t1; v += step) {
    ticks += `<div class="g-tick" style="left:${pctOf(v)}%"></div>`;
    labels += `<span style="left:${pctOf(v)}%">${v}</span>`;
  }

  const byComputer = new Map();
  for (const s of r.sessions) {
    if (!byComputer.has(s.computer)) byComputer.set(s.computer, []);
    byComputer.get(s.computer).push(s);
  }
  let rows = '';
  for (let c = 1; c <= r.computers; c++) {
    const bars = (byComputer.get(c) || []).map((s) => {
      const w = (s.duration / span) * 100;
      const tip = `${s.studentId} on PC ${c}: arrived ${s.arrival}, started ${s.start}, finished ${s.end} (waited ${s.wait} min)`;
      return `<div class="g-bar" style="--h:${hueOf(s.studentId)};left:${pctOf(s.start)}%;width:${w}%" data-narrow="${w < 5}" title="${esc(tip)}"><span>${esc(s.studentId)}</span></div>`;
    }).join('');
    rows += `<div class="g-row"><div class="g-label">PC ${c}</div><div class="g-track">${bars}</div></div>`;
  }

  host.innerHTML = `<div class="g-inner" id="g-inner">${rows}<div class="g-axis">${labels}</div>` +
    `<div class="g-overlay" id="g-overlay">${ticks}<div class="g-now" id="g-now" style="left:${ganttPercent(app.time)}%"></div></div></div>`;
}

function scrubFromPointer(ev) {
  const overlay = $('#g-overlay');
  if (!overlay || app.result.sessions.length === 0) return;
  const rect = overlay.getBoundingClientRect();
  const frac = Math.min(1, Math.max(0, (ev.clientX - rect.left) / rect.width));
  setPlaying(false);
  setTime(app.t0 + frac * (app.t1 - app.t0));
}

/* ---- schedule table ---- */
function renderSchedule() {
  const r = app.result;
  const span = Math.max(1, app.t1 - app.t0);
  const p = (v) => ((v - app.t0) / span) * 100;
  $('#sched-body').innerHTML = r.sessions.length === 0
    ? '<tr><td colspan="7" class="empty-cell">No sessions yet.</td></tr>'
    : r.sessions.map((s) =>
        `<tr><td><span class="dot" style="--h:${hueOf(s.studentId)}"></span>${esc(s.studentId)}</td>` +
        `<td>PC ${s.computer}</td><td class="num">${s.arrival}</td><td class="num">${s.start}</td><td class="num">${s.end}</td>` +
        `<td class="num ${s.wait > 0 ? 'wait-yes' : 'wait-no'}">${s.wait > 0 ? s.wait + ' min' : 'None'}</td>` +
        `<td class="strip-col"><div class="strip" role="img" aria-label="Waited ${s.wait} minutes, then used a computer for ${s.duration} minutes">` +
        (s.wait > 0 ? `<i class="s-wait" style="left:${p(s.arrival)}%;width:${(s.wait / span) * 100}%"></i>` : '') +
        `<i class="s-use" style="left:${p(s.start)}%;width:${Math.max(0.8, (s.duration / span) * 100)}%"></i></div></td></tr>`
      ).join('');
}

/* ---- compare policies ---- */
function renderCompare() {
  const cmp = app.compare;
  if (!cmp) return;
  const table = $('#compare-table'), insight = $('#compare-insight');
  const res = cmp.results;
  if (res[0].metrics.totalRequests === 0) {
    table.innerHTML = '';
    insight.textContent = 'Add some requests to compare how the policies behave.';
    return;
  }
  const rows = [
    { label: 'Average wait', get: (m) => m.avgWait, fmt: (v) => f1(v) + ' min', best: 'min' },
    { label: 'Longest wait', get: (m) => m.maxWait, fmt: (v) => v + ' min', best: 'min' },
    { label: 'Average turnaround', get: (m) => m.avgTurnaround, fmt: (v) => f1(v) + ' min', best: 'min' },
    { label: 'Students who waited', get: (m) => m.studentsWhoWaited, fmt: (v) => v, best: 'min' },
    { label: 'Longest queue', get: (m) => m.maxQueueLength, fmt: (v) => v, best: 'min' },
    { label: 'Lab empties at', get: (m) => m.windowEnd, fmt: (v) => 'minute ' + v, best: 'min' },
    { label: 'Utilization', get: (m) => m.utilization, fmt: (v) => pct(v), best: 'max' },
  ];
  const eps = 1e-9;
  let html = '<thead><tr><th scope="col"><span class="visually-hidden">Measure</span></th>' +
    res.map((x) => `<th scope="col" class="${x.policy === app.cfg.policy ? 'cur' : ''}">${POLICY_SHORT[x.policy]}${x.policy === app.cfg.policy ? '<span class="cur-tag">Selected</span>' : ''}</th>`).join('') +
    '</tr></thead><tbody>';
  for (const row of rows) {
    const vals = res.map((x) => row.get(x.metrics));
    const target = row.best === 'min' ? Math.min(...vals) : Math.max(...vals);
    const allEqual = vals.every((v) => Math.abs(v - vals[0]) < eps);
    html += `<tr><th scope="row">${row.label}</th>` + res.map((x, i) =>
      `<td class="${x.policy === app.cfg.policy ? 'cur' : ''}${!allEqual && Math.abs(vals[i] - target) < eps ? ' best' : ''}">${row.fmt(vals[i])}</td>`).join('') + '</tr>';
  }
  table.innerHTML = html + '</tbody>';

  const avg = res.map((x) => x.metrics.avgWait), mx = res.map((x) => x.metrics.maxWait);
  if (avg.every((v) => Math.abs(v - avg[0]) < eps) && mx.every((v) => v === mx[0])) {
    insight.textContent = 'All three policies give the same schedule here. The lab is rarely full enough for the order of the line to matter. Try fewer computers or more students.';
  } else {
    const bestAvg = res[avg.indexOf(Math.min(...avg))], bestMax = res[mx.indexOf(Math.min(...mx))];
    insight.innerHTML = bestAvg.policy === bestMax.policy
      ? `<strong>${POLICY_NAMES[bestAvg.policy]}</strong> wins on both counts: the lowest average wait (${f1(bestAvg.metrics.avgWait)} min) and the lowest longest wait (${bestAvg.metrics.maxWait} min).`
      : `<strong>${POLICY_NAMES[bestAvg.policy]}</strong> has the lowest average wait (${f1(bestAvg.metrics.avgWait)} min), but <strong>${POLICY_NAMES[bestMax.policy]}</strong> keeps the longest wait smallest (${bestMax.metrics.maxWait} min). Lower averages can come at the cost of fairness.`;
  }
}

/* ---- capacity charts ---- */
function niceCeil(v) {
  if (v <= 0) return 1;
  const e = Math.pow(10, Math.floor(Math.log10(v)));
  const n = v / e;
  return (n <= 1 ? 1 : n <= 2 ? 2 : n <= 2.5 ? 2.5 : n <= 5 ? 5 : 10) * e;
}

function lineChart({ points, getY, yMax, yFmt, current, label }) {
  const W = 520, H = 236, L = 48, R = 16, T = 14, B = 38;
  const pw = W - L - R, ph = H - T - B;
  const xs = points.map((p) => p.computers);
  const xMin = xs[0], xMax = xs[xs.length - 1];
  const X = (n) => L + (xMax === xMin ? pw / 2 : ((n - xMin) / (xMax - xMin)) * pw);
  const Y = (v) => T + ph - (yMax === 0 ? 0 : (v / yMax) * ph);
  let g = '';
  for (let i = 0; i <= 4; i++) {
    const v = (yMax * i) / 4, y = Y(v);
    g += `<line class="${i === 0 ? 'c-axis' : 'c-grid'}" x1="${L}" x2="${W - R}" y1="${y}" y2="${y}"/>` +
         `<text class="c-text" x="${L - 8}" y="${y + 4}" text-anchor="end">${yFmt(v)}</text>`;
  }
  const every = Math.ceil(xs.length / 12);
  xs.forEach((n, i) => {
    if (i % every === 0) g += `<text class="c-text" x="${X(n)}" y="${H - 18}" text-anchor="middle">${n}</text>`;
  });
  g += `<text class="c-text" x="${L + pw / 2}" y="${H - 3}" text-anchor="middle">Number of computers</text>`;

  const path = points.map((p, i) => `${i ? 'L' : 'M'}${X(p.computers).toFixed(1)},${Y(getY(p)).toFixed(1)}`).join(' ');
  let dots = points.map((p) =>
    `<circle class="c-dot" cx="${X(p.computers).toFixed(1)}" cy="${Y(getY(p)).toFixed(1)}" r="4"><title>${p.computers} ${plural(p.computers, 'computer', 'computers')}: ${yFmt(getY(p), true)}</title></circle>`).join('');

  let cur = '';
  const cp = points.find((p) => p.computers === current);
  if (cp) {
    const cx = X(cp.computers), cy = Y(getY(cp));
    const anchor = cx > W - 70 ? 'end' : 'middle';
    cur = `<line class="c-cur-line" x1="${cx}" x2="${cx}" y1="${T}" y2="${T + ph}"/><circle class="c-cur-dot" cx="${cx}" cy="${cy}" r="6"/>` +
          `<text class="c-cur-text" x="${cx}" y="${T + 10}" text-anchor="${anchor}" dx="${anchor === 'end' ? -8 : 0}">Now: ${current}</text>`;
  }
  return `<svg viewBox="0 0 ${W} ${H}" role="img" aria-label="${esc(label)}">${g}${cur}<path class="c-line" d="${path}"/>${dots}</svg>`;
}

function renderCapacity() {
  const sw = app.sweep;
  if (!sw) return;
  const pts = sw.points, insight = $('#capacity-insight');
  const hasData = app.result.metrics.totalRequests > 0;
  if (!hasData) {
    insight.textContent = 'Add some requests to see how the number of computers changes the waiting time.';
    $('#chart-wait').innerHTML = ''; $('#chart-util').innerHTML = '';
    return;
  }
  const cur = app.cfg.computers;
  $('#chart-wait').innerHTML = lineChart({
    points: pts, getY: (p) => p.avgWait, yMax: niceCeil(Math.max(...pts.map((p) => p.avgWait))),
    yFmt: (v, long) => (long ? f1(v) + ' min average wait' : f1(v)), current: cur,
    label: 'Average wait in minutes for each number of computers',
  });
  $('#chart-util').innerHTML = lineChart({
    points: pts, getY: (p) => p.utilization, yMax: 1,
    yFmt: (v, long) => (long ? pct(v) + ' utilization' : Math.round(v * 100) + '%'), current: cur,
    label: 'Computer utilization for each number of computers',
  });

  const none = pts.find((p) => p.maxWait === 0);
  const policy = POLICY_NAMES[sw.policy];
  if (!none) {
    insight.textContent = `With ${policy}, students still wait even with ${pts[pts.length - 1].computers} computers.`;
  } else if (none.computers === 1) {
    insight.innerHTML = `One computer is enough for nobody to wait under ${policy}.`;
  } else {
    const prev = pts[none.computers - 2];
    insight.innerHTML = `Under ${policy}, <strong>${none.computers} computers</strong> is the smallest lab where nobody waits (utilization ${pct(none.utilization)}). With ${prev.computers}, the average wait is ${f1(prev.avgWait)} min.`;
  }
}

/* ------------------------------------------------------------ playback -- */
function setTime(t) {
  app.time = Math.min(app.t1, Math.max(app.t0, t));
  renderTime();
}

function setPlaying(on) {
  app.playing = on;
  $('#btn-play').textContent = on ? 'Pause' : 'Play';
  if (on) {
    if (app.time >= app.t1) app.time = app.t0;
    app.lastFrame = performance.now();
    requestAnimationFrame(tick);
  }
}

function tick(now) {
  if (!app.playing) return;
  const dt = Math.min(0.25, (now - app.lastFrame) / 1000);
  app.lastFrame = now;
  setTime(app.time + dt * app.speed);
  if (app.time >= app.t1) { setPlaying(false); return; }
  requestAnimationFrame(tick);
}

/* ---------------------------------------------------------------- tabs -- */
function setTab(name) {
  app.tab = name;
  $$('.tabs [role="tab"]').forEach((b) => {
    const on = b.dataset.tab === name;
    b.setAttribute('aria-selected', String(on));
    b.tabIndex = on ? 0 : -1;
  });
  $$('[role="tabpanel"]').forEach((p) => { p.hidden = p.id !== `panel-${name}`; });
  if (name !== 'sim') setPlaying(false);
  if (name === 'compare') app.compare ? renderCompare() : loadCompare();
  if (name === 'capacity') app.sweep ? renderCapacity() : loadSweep();
}

/* ------------------------------------------------------------- wiring -- */
function init() {
  // policy + computers
  $('#policy-seg').addEventListener('click', (e) => {
    const b = e.target.closest('button[data-policy]');
    if (!b || b.dataset.policy === app.cfg.policy) return;
    mutate(() => api('PUT', '/api/config', { policy: b.dataset.policy }));
  });
  const setComputers = (n) => {
    if (!Number.isInteger(n)) { renderControls(); return; }
    n = Math.min(app.cfg.limits.maxComputers, Math.max(1, n));
    if (n === app.cfg.computers) { renderControls(); return; }
    mutate(() => api('PUT', '/api/config', { computers: n }));
  };
  $('#pc-minus').addEventListener('click', () => setComputers(app.cfg.computers - 1));
  $('#pc-plus').addEventListener('click', () => setComputers(app.cfg.computers + 1));
  $('#pc-input').addEventListener('change', (e) => setComputers(parseInt(e.target.value, 10)));

  // add request
  $('#add-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const err = $('#form-error');
    err.textContent = '';
    const num = (id) => { const v = $(id).value.trim(); return v === '' ? NaN : Number(v); };
    const arrival = num('#f-arrival'), duration = num('#f-duration');
    if (!Number.isInteger(arrival) || !Number.isInteger(duration)) {
      err.textContent = 'Arrival time and session length must be whole numbers.';
      return;
    }
    const body = { arrival, duration, priority: Number($('#f-priority').value) };
    const id = $('#f-id').value.trim();
    if (id) body.id = id;
    try {
      const cfg = await api('POST', '/api/requests', body);
      await afterChange(cfg);
      $('#f-id').value = '';
      $('#f-arrival').focus();
      $('#f-arrival').select();
    } catch (ex) {
      err.textContent = ex.message;
    }
  });

  // remove / clear
  $('#req-body').addEventListener('click', (e) => {
    const b = e.target.closest('[data-remove]');
    if (b) mutate(() => api('DELETE', `/api/requests/${encodeURIComponent(b.dataset.remove)}`));
  });
  $('#btn-clear').addEventListener('click', () => {
    const btn = $('#btn-clear');
    if (!btn.classList.contains('armed')) {
      btn.classList.add('armed');
      btn.textContent = 'Click again to clear';
      clearTimeout(app.clearTimer);
      app.clearTimer = setTimeout(() => { btn.classList.remove('armed'); btn.textContent = 'Clear all'; }, 3000);
      return;
    }
    clearTimeout(app.clearTimer);
    btn.classList.remove('armed');
    btn.textContent = 'Clear all';
    mutate(() => api('DELETE', '/api/requests'), 'All requests cleared.');
  });

  // samples
  $('#btn-example').addEventListener('click', () => mutate(() => api('POST', '/api/sample', { kind: 'example' })));
  $('#btn-demo').addEventListener('click', () => mutate(() => api('POST', '/api/sample', { kind: 'demo' })));
  $('#btn-random').addEventListener('click', () => {
    const count = parseInt($('#f-count').value, 10);
    mutate(() => api('POST', '/api/sample', { kind: 'random', count: Number.isInteger(count) ? count : 24 }));
  });

  // playback
  $('#btn-play').addEventListener('click', () => setPlaying(!app.playing));
  $('#btn-restart').addEventListener('click', () => { setPlaying(false); setTime(app.t0); });
  $('#time-slider').addEventListener('input', (e) => { setPlaying(false); setTime(Number(e.target.value)); });
  $('#speed-select').addEventListener('change', (e) => { app.speed = Number(e.target.value); });

  // gantt scrubbing
  const gantt = $('#gantt');
  let dragging = false;
  gantt.addEventListener('pointerdown', (e) => { dragging = true; gantt.setPointerCapture(e.pointerId); scrubFromPointer(e); });
  gantt.addEventListener('pointermove', (e) => { if (dragging) scrubFromPointer(e); });
  const stopDrag = () => { dragging = false; };
  gantt.addEventListener('pointerup', stopDrag);
  gantt.addEventListener('pointercancel', stopDrag);

  // tabs
  const tabs = $$('.tabs [role="tab"]');
  tabs.forEach((b, i) => {
    b.addEventListener('click', () => setTab(b.dataset.tab));
    b.addEventListener('keydown', (e) => {
      let next = null;
      if (e.key === 'ArrowRight') next = tabs[(i + 1) % tabs.length];
      if (e.key === 'ArrowLeft') next = tabs[(i - 1 + tabs.length) % tabs.length];
      if (e.key === 'Home') next = tabs[0];
      if (e.key === 'End') next = tabs[tabs.length - 1];
      if (next) { e.preventDefault(); next.focus(); setTab(next.dataset.tab); }
    });
  });

  // files
  const saveDialog = $('#save-dialog'), loadDialog = $('#load-dialog');
  $$('dialog [data-close]').forEach((b) => b.addEventListener('click', () => b.closest('dialog').close()));
  $('#btn-save').addEventListener('click', () => {
    $('#save-error').textContent = '';
    saveDialog.showModal();
    $('#save-name').focus();
  });
  $('#save-form').addEventListener('submit', async (e) => {
    e.preventDefault();
    const name = $('#save-name').value.trim();
    try {
      await api('POST', '/api/save', { name });
      saveDialog.close();
      toast(`Saved as ${name}.txt`);
    } catch (ex) {
      $('#save-error').textContent = ex.message;
    }
  });
  $('#btn-load').addEventListener('click', async () => {
    const list = $('#file-list');
    list.innerHTML = '<li class="none">Loading…</li>';
    loadDialog.showModal();
    try {
      const { files } = await api('GET', '/api/files');
      list.innerHTML = files.length
        ? files.map((f) => `<li><button type="button" data-file="${esc(f)}">${esc(f)}.txt</button></li>`).join('')
        : '<li class="none">No saved files yet. Use Save to create one.</li>';
    } catch (ex) {
      list.innerHTML = `<li class="none">${esc(ex.message)}</li>`;
    }
  });
  $('#file-list').addEventListener('click', async (e) => {
    const b = e.target.closest('[data-file]');
    if (!b) return;
    loadDialog.close();
    mutate(() => api('POST', '/api/load', { name: b.dataset.file }), `Loaded ${b.dataset.file}.txt`);
  });
  $('#btn-import').addEventListener('click', () => $('#import-file').click());
  $('#import-file').addEventListener('change', async (e) => {
    const file = e.target.files[0];
    e.target.value = '';
    if (!file) return;
    const text = await file.text();
    mutate(() => api('POST', '/api/import', text, true), `Imported ${file.name}`);
  });

  // theme
  $('#btn-theme').addEventListener('click', () => {
    const root = document.documentElement;
    const dark = root.dataset.theme ? root.dataset.theme === 'dark' : matchMedia('(prefers-color-scheme: dark)').matches;
    root.dataset.theme = dark ? 'light' : 'dark';
    try { localStorage.setItem('lab-theme', root.dataset.theme); } catch { /* private mode */ }
  });

  loadAll().catch((e) => toast(e.message, 'error'));
}

init();
